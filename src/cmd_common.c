#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#include "cmds.h"
#include "mmdb_enc.h"

command *const all_commands[] = {&cmd_completion, &cmd_diff, &cmd_export, &cmd_import, &cmd_inspect,
				 &cmd_metadata,	  &cmd_read, &cmd_shrink, &cmd_stats,  &cmd_verify};
const int ncommands = (int)(sizeof all_commands / sizeof all_commands[0]);

void missing_flag(command *c, const char *msg)
{
	printf("Error: %s\n", msg);
	command_help(c, stdout);
	fflush(stdout);
	exit(1);
}

const dnode *extract_field(const dnode *v, const char *path)
{
	const char *p = path;
	for (;;) {
		const char *dot = strchr(p, '.');
		size_t n = dot ? (size_t)(dot - p) : strlen(p);
		if (v->kind != T_MAP)
			return NULL;
		const dnode *next = NULL;
		for (uint32_t i = 0; i < v->n; i++) {
			const dnode *k = &v->items[2 * i];
			if (k->n == n && memcmp(k->s, p, n) == 0) {
				next = &v->items[2 * i + 1];
				break;
			}
		}
		if (!next)
			return NULL;
		v = next;
		if (!dot)
			return v;
		p = dot + 1;
	}
}

/* strconv.FormatFloat(f, 'g', -1, bits), which fmt's %v uses. */
static void go_float_g(double f, int bits, buf_t *out)
{
	if (isnan(f)) {
		buf_append(out, "NaN", 3);
		return;
	}
	if (isinf(f)) {
		buf_append(out, f > 0 ? "+Inf" : "-Inf", 4);
		return;
	}
	if (f == 0) {
		buf_append(out, signbit(f) ? "-0" : "0", signbit(f) ? 2 : 1);
		return;
	}
	char tmp[64];
	int prec = 1;
	for (; prec <= 17; prec++) {
		snprintf(tmp, sizeof tmp, "%.*e", prec - 1, f);
		double back = strtod(tmp, NULL);
		if (bits == 32 ? (float)back == (float)f : back == f)
			break;
	}
	/* tmp is [-]d.ddde[+-]xx: split into digits and exponent */
	char digits[32];
	int nd = 0;
	const char *p = tmp;
	bool neg = *p == '-';
	if (neg)
		p++;
	for (; *p && *p != 'e'; p++)
		if (*p >= '0' && *p <= '9')
			digits[nd++] = *p;
	int exp = atoi(p + 1);
	while (nd > 1 && digits[nd - 1] == '0')
		nd--;
	if (neg)
		buf_push(out, '-');
	/* shortest 'g' switches to %e below 1e-4 and from 1e6 on */
	if (exp < -4 || exp >= 6) {
		buf_push(out, (uint8_t)digits[0]);
		if (nd > 1) {
			buf_push(out, '.');
			buf_append(out, digits + 1, (size_t)(nd - 1));
		}
		buf_printf(out, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
		return;
	}
	if (exp < 0) {
		buf_append(out, "0.", 2);
		for (int i = 0; i < -exp - 1; i++)
			buf_push(out, '0');
		buf_append(out, digits, (size_t)nd);
		return;
	}
	for (int i = 0; i <= exp; i++)
		buf_push(out, (uint8_t)(i < nd ? digits[i] : '0'));
	if (nd > exp + 1) {
		buf_push(out, '.');
		buf_append(out, digits + exp + 1, (size_t)(nd - exp - 1));
	}
}

void go_sprint(const dnode *v, buf_t *out)
{
	switch (v->kind) {
	case T_STRING:
		buf_append(out, v->s, v->n);
		break;
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
		buf_printf(out, "%llu", (unsigned long long)v->u);
		break;
	case T_INT32:
		buf_printf(out, "%lld", (long long)(int64_t)v->u);
		break;
	case T_UINT128:
		json_u128(out, v->u_hi, v->u);
		break;
	case T_BOOL:
		buf_append(out, v->u ? "true" : "false", v->u ? 4 : 5);
		break;
	case T_DOUBLE:
		go_float_g(v->f, 64, out);
		break;
	case T_FLOAT:
		go_float_g(v->f, 32, out);
		break;
	case T_BYTES:
		buf_push(out, '[');
		for (uint32_t i = 0; i < v->n; i++)
			buf_printf(out, i ? " %u" : "%u", v->s[i]);
		buf_push(out, ']');
		break;
	case T_ARRAY:
		buf_push(out, '[');
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ' ');
			go_sprint(&v->items[i], out);
		}
		buf_push(out, ']');
		break;
	case T_MAP: {
		uint32_t *idx = dnode_sorted_pairs(v);
		buf_append(out, "map[", 4);
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ' ');
			go_sprint(&v->items[2 * idx[i]], out);
			buf_push(out, ':');
			go_sprint(&v->items[2 * idx[i] + 1], out);
		}
		buf_push(out, ']');
		free(idx);
		break;
	}
	}
}

const char *go_type_name(const dnode *v)
{
	switch (v->kind) {
	case T_STRING:
		return "string";
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
		return "uint64";
	case T_INT32:
		return "int";
	case T_UINT128:
		return "*big.Int";
	case T_BOOL:
		return "bool";
	case T_DOUBLE:
		return "float64";
	case T_FLOAT:
		return "float32";
	case T_BYTES:
		return "[]uint8";
	case T_ARRAY:
		return "[]interface {}";
	case T_MAP:
		return "map[string]interface {}";
	}
	return "unknown";
}

static int go_class(uint32_t kind)
{
	return kind == T_UINT16 || kind == T_UINT32 ? (int)T_UINT64 : (int)kind;
}

bool go_deep_equal(const dnode *a, const dnode *b)
{
	if (go_class(a->kind) != go_class(b->kind))
		return false;
	switch (a->kind) {
	case T_STRING:
	case T_BYTES:
		return a->n == b->n && (a->n == 0 || memcmp(a->s, b->s, a->n) == 0);
	case T_DOUBLE:
	case T_FLOAT:
		return a->f == b->f;
	case T_UINT128:
		return a->u == b->u && a->u_hi == b->u_hi;
	case T_ARRAY:
		if (a->n != b->n)
			return false;
		for (uint32_t i = 0; i < a->n; i++)
			if (!go_deep_equal(&a->items[i], &b->items[i]))
				return false;
		return true;
	case T_MAP:
		if (a->n != b->n)
			return false;
		for (uint32_t i = 0; i < a->n; i++) {
			const dnode *ka = &a->items[2 * i];
			bool found = false;
			for (uint32_t j = 0; j < b->n; j++) {
				const dnode *kb = &b->items[2 * j];
				if (ka->n == kb->n && memcmp(ka->s, kb->s, ka->n) == 0) {
					if (!go_deep_equal(&a->items[2 * i + 1], &b->items[2 * j + 1]))
						return false;
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
		return true;
	default:
		return a->u == b->u;
	}
}

int write_file(const char *path, const uint8_t *p, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		err_set("open %s: %s", path, go_strerror(errno));
		return -1;
	}
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			err_set("write %s: %s", path, go_strerror(errno));
			close(fd);
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	if (close(fd) < 0) {
		err_set("close %s: %s", path, go_strerror(errno));
		return -1;
	}
	return 0;
}
