#include "mmdb_read.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"
#include "mmdb_enc.h"
#include "utf8.h"

#define MAX_DEPTH 512

static const uint8_t MARKER[] = "\xAB\xCD\xEFMaxMind.com";
#define MARKER_LEN 14

/* ---- arena ---- */

struct arena_block {
	struct arena_block *next;
	size_t pad;
};

void arena_init(arena *a) { memset(a, 0, sizeof *a); }

void *arena_alloc(arena *a, size_t n)
{
	n = (n + 15) & ~(size_t)15;
	if (a->used + n > a->cap) {
		size_t cap = n > (1u << 20) ? n : (1u << 20);
		struct arena_block *b = xmalloc(sizeof *b + cap);
		b->next = a->blocks;
		a->blocks = b;
		a->p = (uint8_t *)(b + 1);
		a->cap = cap;
		a->used = 0;
	}
	void *r = a->p + a->used;
	a->used += n;
	return r;
}

void arena_reset(arena *a)
{
	if (!a->blocks)
		return;
	struct arena_block *keep = a->blocks, *b = keep->next;
	while (b) {
		struct arena_block *next = b->next;
		free(b);
		b = next;
	}
	keep->next = NULL;
	a->used = 0;
}

void arena_free(arena *a)
{
	struct arena_block *b = a->blocks;
	while (b) {
		struct arena_block *next = b->next;
		free(b);
		b = next;
	}
	memset(a, 0, sizeof *a);
}

/* ---- decoding (internal/decoder/data_decoder.go) ---- */

static int offset_err(void)
{
	err_set("unexpected end of database");
	return -1;
}

static bool has_range(uint64_t len, uint64_t off, uint64_t size) { return size <= len && off <= len - size; }

/* decodeCtrlData */
static int ctrl(const mdec *d, uint64_t off, uint32_t *kind, uint64_t *size, uint64_t *next)
{
	if (off >= d->len)
		return offset_err();
	uint8_t c = d->buf[off];
	uint64_t no = off + 1;
	uint32_t k = c >> 5;
	if (k == T_EXTENDED) {
		if (no >= d->len)
			return offset_err();
		k = (uint32_t)(uint8_t)(d->buf[no] - 1) + 8;
		no++;
	}
	uint64_t sz = c & 0x1f;
	if (sz < 29 || k == T_POINTER) {
		*kind = k;
		*size = sz;
		*next = no;
		return 0;
	}
	uint64_t end = no + sz - 28;
	if (end > d->len)
		return offset_err();
	const uint8_t *b = d->buf + no;
	if (sz == 29) {
		*size = 29 + (uint64_t)b[0];
		*next = no + 1;
	} else if (sz == 30) {
		*size = 285 + ((uint64_t)b[0] << 8 | b[1]);
		*next = end;
	} else {
		*size = 65821 + ((uint64_t)b[0] << 16 | (uint64_t)b[1] << 8 | b[2]);
		*next = end;
	}
	*kind = k;
	return 0;
}

/* decodePointer */
static int pointer_at(const mdec *d, uint64_t size, uint64_t off, uint64_t *target, uint64_t *next)
{
	uint64_t psz = ((size >> 3) & 3) + 1;
	uint64_t no = off + psz;
	if (no > d->len)
		return offset_err();
	uint64_t v = psz == 4 ? 0 : (size & 7);
	for (uint64_t i = 0; i < psz; i++)
		v = v << 8 | d->buf[off + i];
	if (psz == 2)
		v += 2048;
	else if (psz == 3)
		v += 526336;
	*target = v;
	*next = no;
	return 0;
}

int mdec_skip(const mdec *d, uint64_t off, uint64_t *next)
{
	uint64_t todo = 1;
	while (todo > 0) {
		uint32_t kind;
		uint64_t size, no;
		if (ctrl(d, off, &kind, &size, &no) < 0)
			return -1;
		switch (kind) {
		case T_POINTER: {
			uint64_t end = no + ((size >> 3) & 3) + 1;
			if (end > d->len)
				return offset_err();
			no = end;
			break;
		}
		case T_MAP:
			if (size > (UINT64_MAX - todo) / 2) {
				err_set("container size overflow");
				return -1;
			}
			todo += 2 * size;
			break;
		case T_ARRAY:
			if (size > UINT64_MAX - todo) {
				err_set("container size overflow");
				return -1;
			}
			todo += size;
			break;
		case T_BOOL:
			break;
		default:
			if (!has_range(d->len, no, size))
				return offset_err();
			no += size;
		}
		off = no;
		todo--;
	}
	*next = off;
	return 0;
}

static int decode(const mdec *d, uint64_t off, arena *a, dnode *out, uint64_t *next, int depth);

static int bad_size(const char *what, uint64_t size)
{
	err_set("the MaxMind DB file's data section contains bad data (%s size of %llu)", what,
		(unsigned long long)size);
	return -1;
}

static int decode_key(const mdec *d, uint64_t off, dnode *out, uint64_t *next)
{
	uint32_t kind;
	uint64_t size, p;
	if (ctrl(d, off, &kind, &size, &p) < 0)
		return -1;
	uint64_t after = 0;
	if (kind == T_POINTER) {
		uint64_t target;
		if (pointer_at(d, size, p, &target, &after) < 0 || ctrl(d, target, &kind, &size, &p) < 0)
			return -1;
		if (kind == T_POINTER) {
			err_set("invalid pointer to pointer at offset %llu", (unsigned long long)off);
			return -1;
		}
	}
	if (kind != T_STRING) {
		err_set("unexpected map key type: %u", kind);
		return -1;
	}
	if (!has_range(d->len, p, size))
		return offset_err();
	memset(out, 0, sizeof *out);
	out->kind = T_STRING;
	out->s = d->buf + p;
	out->n = (uint32_t)size;
	*next = after ? after : p + size;
	return 0;
}

static int decode_payload(const mdec *d, uint32_t kind, uint64_t size, uint64_t p, arena *a, dnode *out,
			  uint64_t *next, int depth)
{
	memset(out, 0, sizeof *out);
	out->kind = kind;
	switch (kind) {
	case T_STRING:
	case T_BYTES:
		if (!has_range(d->len, p, size))
			return offset_err();
		out->s = d->buf + p;
		out->n = (uint32_t)size;
		*next = p + size;
		return 0;
	case T_DOUBLE:
	case T_FLOAT: {
		uint64_t want = kind == T_DOUBLE ? 8 : 4;
		if (size != want)
			return bad_size(kind == T_DOUBLE ? "float 64" : "float32", size);
		if (!has_range(d->len, p, size))
			return offset_err();
		uint64_t bits = 0;
		for (uint64_t i = 0; i < size; i++)
			bits = bits << 8 | d->buf[p + i];
		if (kind == T_DOUBLE) {
			memcpy(&out->f, &bits, 8);
		} else {
			uint32_t b32 = (uint32_t)bits;
			float f;
			memcpy(&f, &b32, 4);
			out->f = f;
			out->u = b32;
		}
		*next = p + size;
		return 0;
	}
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
	case T_UINT128:
	case T_INT32: {
		uint64_t max = kind == T_UINT16 ? 2 : kind == T_UINT32 || kind == T_INT32 ? 4 : kind == T_UINT64 ? 8 : 16;
		if (size > max)
			return bad_size(kind == T_UINT16   ? "uint16"
					: kind == T_UINT32 ? "uint32"
					: kind == T_INT32  ? "int32"
					: kind == T_UINT64 ? "uint64"
							   : "uint128",
					size);
		if (!has_range(d->len, p, size))
			return offset_err();
		uint64_t hi = 0, lo = 0;
		for (uint64_t i = 0; i < size; i++) {
			hi = hi << 8 | lo >> 56;
			lo = lo << 8 | d->buf[p + i];
		}
		if (kind == T_INT32) {
			int32_t v = 0;
			for (uint64_t i = 0; i < size; i++)
				v = (int32_t)((uint32_t)v << 8 | d->buf[p + i]);
			lo = (uint64_t)(int64_t)v;
		}
		out->u = lo;
		out->u_hi = hi;
		*next = p + size;
		return 0;
	}
	case T_BOOL:
		if (size > 1)
			return bad_size("bool", size);
		out->u = size;
		*next = p;
		return 0;
	case T_MAP:
	case T_ARRAY: {
		uint64_t per = kind == T_MAP ? 2 : 1;
		if (size > d->len - (p < d->len ? p : d->len))
			return offset_err();
		out->n = (uint32_t)size;
		out->items = size ? arena_alloc(a, (size_t)(per * size) * sizeof(dnode)) : NULL;
		uint64_t cur = p;
		for (uint64_t i = 0; i < size; i++) {
			if (kind == T_MAP) {
				if (decode_key(d, cur, &out->items[2 * i], &cur) < 0 ||
				    decode(d, cur, a, &out->items[2 * i + 1], &cur, depth + 1) < 0)
					return -1;
			} else if (decode(d, cur, a, &out->items[i], &cur, depth + 1) < 0) {
				return -1;
			}
		}
		*next = cur;
		return 0;
	}
	}
	err_set("unknown type: %u", kind);
	return -1;
}

static int decode(const mdec *d, uint64_t off, arena *a, dnode *out, uint64_t *next, int depth)
{
	if (depth > MAX_DEPTH) {
		err_set("exceeded maximum data structure depth; database is likely corrupt");
		return -1;
	}
	uint32_t kind;
	uint64_t size, p;
	if (ctrl(d, off, &kind, &size, &p) < 0)
		return -1;
	if (kind == T_POINTER) {
		uint64_t target, after, ignored;
		if (pointer_at(d, size, p, &target, &after) < 0 || ctrl(d, target, &kind, &size, &p) < 0)
			return -1;
		if (kind == T_POINTER) {
			err_set("invalid pointer to pointer at offset %llu", (unsigned long long)off);
			return -1;
		}
		if (decode_payload(d, kind, size, p, a, out, &ignored, depth) < 0)
			return -1;
		*next = after;
		return 0;
	}
	return decode_payload(d, kind, size, p, a, out, next, depth);
}

int mdec_value(const mdec *d, uint64_t off, arena *a, dnode **out, uint64_t *next)
{
	dnode *v = arena_alloc(a, sizeof *v);
	if (decode(d, off, a, v, next, 0) < 0)
		return -1;
	*out = v;
	return 0;
}

/* ---- canonical form, JSON, UTF-8 ---- */

static void put_uvarint(buf_t *b, uint64_t x)
{
	while (x >= 0x80) {
		buf_push(b, (uint8_t)(x | 0x80));
		x >>= 7;
	}
	buf_push(b, (uint8_t)x);
}

static void put_be(buf_t *b, uint64_t v, int n)
{
	for (int i = n - 1; i >= 0; i--)
		buf_push(b, (uint8_t)(v >> (8 * i)));
}

static int bytes_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	size_t n = an < bn ? an : bn;
	int c = n ? memcmp(a, b, n) : 0;
	return c ? c : (an < bn ? -1 : an > bn);
}

static const dnode *sort_ctx;
static int cmp_pair(const void *x, const void *y)
{
	const dnode *a = &sort_ctx[2 * *(const uint32_t *)x], *b = &sort_ctx[2 * *(const uint32_t *)y];
	return bytes_cmp(a->s, a->n, b->s, b->n);
}

uint32_t *dnode_sorted_pairs(const dnode *v)
{
	uint32_t *idx = xmalloc((v->n ? v->n : 1) * sizeof *idx);
	for (uint32_t i = 0; i < v->n; i++)
		idx[i] = i;
	sort_ctx = v->items;
	qsort(idx, v->n, sizeof *idx, cmp_pair);
	return idx;
}

void dnode_canonical(const dnode *v, buf_t *out)
{
	buf_push(out, (uint8_t)v->kind);
	switch (v->kind) {
	case T_STRING:
	case T_BYTES:
		put_uvarint(out, v->n);
		buf_append(out, v->s, v->n);
		break;
	case T_UINT16:
		put_be(out, v->u, 2);
		break;
	case T_UINT32:
	case T_INT32:
		put_be(out, v->u, 4);
		break;
	case T_UINT64:
		put_be(out, v->u, 8);
		break;
	case T_UINT128:
		put_be(out, v->u_hi, 8);
		put_be(out, v->u, 8);
		break;
	case T_BOOL:
		buf_push(out, (uint8_t)v->u);
		break;
	case T_FLOAT:
		put_be(out, v->u, 4);
		break;
	case T_DOUBLE: {
		uint64_t bits;
		memcpy(&bits, &v->f, 8);
		put_be(out, bits, 8);
		break;
	}
	case T_ARRAY:
		put_uvarint(out, v->n);
		for (uint32_t i = 0; i < v->n; i++)
			dnode_canonical(&v->items[i], out);
		break;
	case T_MAP: {
		uint32_t *idx = dnode_sorted_pairs(v);
		put_uvarint(out, v->n);
		for (uint32_t i = 0; i < v->n; i++) {
			const dnode *k = &v->items[2 * idx[i]];
			put_uvarint(out, k->n);
			buf_append(out, k->s, k->n);
			dnode_canonical(&v->items[2 * idx[i] + 1], out);
		}
		free(idx);
		break;
	}
	}
}

/* encoding/json string escaping, HTML-safe as json.Marshal does. */
void json_string(buf_t *out, const uint8_t *s, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	buf_push(out, '"');
	for (size_t i = 0; i < n;) {
		uint8_t c = s[i];
		if (c < 0x80) {
			if (c == '"' || c == '\\') {
				buf_push(out, '\\');
				buf_push(out, c);
			} else if (c == '\n') {
				buf_append(out, "\\n", 2);
			} else if (c == '\r') {
				buf_append(out, "\\r", 2);
			} else if (c == '\t') {
				buf_append(out, "\\t", 2);
			} else if (c == '\b') {
				buf_append(out, "\\b", 2);
			} else if (c == '\f') {
				buf_append(out, "\\f", 2);
			} else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
				buf_append(out, "\\u00", 4);
				buf_push(out, (uint8_t)hex[c >> 4]);
				buf_push(out, (uint8_t)hex[c & 15]);
			} else {
				buf_push(out, c);
			}
			i++;
			continue;
		}
		uint32_t r;
		int size = go_decode_rune(s + i, n - i, &r);
		if (r == UTF8_RUNE_ERROR && size == 1) {
			buf_append(out, "\\ufffd", 6);
		} else if (r == 0x2028 || r == 0x2029) {
			buf_printf(out, "\\u%04x", r);
		} else {
			buf_append(out, s + i, (size_t)size);
		}
		i += (size_t)size;
	}
	buf_push(out, '"');
}

void json_float(buf_t *out, double f, int bits)
{
	if (isnan(f) || isinf(f)) {
		buf_append(out, "null", 4);
		return;
	}
	char tmp[128];
	int prec = 1;
	for (; prec <= 17; prec++) {
		snprintf(tmp, sizeof tmp, "%.*g", prec, f);
		double back = strtod(tmp, NULL);
		if (bits == 32 ? (float)back == (float)f : back == f)
			break;
	}
	double af = fabs(f);
	if (af != 0 && (af < 1e-6 || af >= 1e21)) {
		snprintf(tmp, sizeof tmp, "%.*e", prec - 1, f);
		/* encoding/json cleans e-07 up to e-7 */
		size_t n = strlen(tmp);
		if (n >= 4 && tmp[n - 4] == 'e' && tmp[n - 3] == '-' && tmp[n - 2] == '0') {
			tmp[n - 2] = tmp[n - 1];
			tmp[n - 1] = 0;
		}
	} else {
		snprintf(tmp, sizeof tmp, "%.*g", prec, f);
		if (strchr(tmp, 'e')) {
			int dec = 0;
			for (; dec < 40; dec++) {
				char t2[96];
				snprintf(t2, sizeof t2, "%.*f", dec, f);
				if (strtod(t2, NULL) == strtod(tmp, NULL)) {
					snprintf(tmp, sizeof tmp, "%s", t2);
					break;
				}
			}
		}
	}
	buf_append(out, tmp, strlen(tmp));
}

void json_u128(buf_t *out, uint64_t hi, uint64_t lo)
{
	u128 v = (u128)hi << 64 | lo;
	char tmp[48];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + (int)(v % 10));
		v /= 10;
	} while (v);
	while (n)
		buf_push(out, (uint8_t)tmp[--n]);
}

void json_base64(buf_t *out, const uint8_t *s, size_t n)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	buf_push(out, '"');
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
		char q[4] = {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], t[v & 63]};
		buf_append(out, q, 4);
	}
	if (n - i == 1) {
		uint32_t v = (uint32_t)s[i] << 16;
		char q[4] = {t[v >> 18], t[(v >> 12) & 63], '=', '='};
		buf_append(out, q, 4);
	} else if (n - i == 2) {
		uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8;
		char q[4] = {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], '='};
		buf_append(out, q, 4);
	}
	buf_push(out, '"');
}

void dnode_json(const dnode *v, buf_t *out)
{
	switch (v->kind) {
	case T_STRING:
		json_string(out, v->s, v->n);
		break;
	case T_BYTES:
		json_base64(out, v->s, v->n);
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
	case T_FLOAT:
		json_float(out, v->f, 32);
		break;
	case T_DOUBLE:
		json_float(out, v->f, 64);
		break;
	case T_ARRAY:
		buf_push(out, '[');
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ',');
			dnode_json(&v->items[i], out);
		}
		buf_push(out, ']');
		break;
	case T_MAP: {
		uint32_t *idx = dnode_sorted_pairs(v);
		buf_push(out, '{');
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ',');
			const dnode *k = &v->items[2 * idx[i]];
			json_string(out, k->s, k->n);
			buf_push(out, ':');
			dnode_json(&v->items[2 * idx[i] + 1], out);
		}
		buf_push(out, '}');
		free(idx);
		break;
	}
	}
}

bool dnode_utf8_ok(const dnode *v)
{
	switch (v->kind) {
	case T_STRING:
		return utf8_valid(v->s, v->n);
	case T_ARRAY:
		for (uint32_t i = 0; i < v->n; i++)
			if (!dnode_utf8_ok(&v->items[i]))
				return false;
		return true;
	case T_MAP:
		for (uint32_t i = 0; i < 2 * v->n; i++)
			if (!dnode_utf8_ok(&v->items[i]))
				return false;
		return true;
	}
	return true;
}

/* ---- open ---- */

static bool key_is(const dnode *k, const char *name)
{
	size_t n = strlen(name);
	return k->n == n && memcmp(k->s, name, n) == 0;
}

static int meta_uint(const dnode *v, const char *name, uint64_t *out)
{
	if ((v->kind == T_UINT16 || v->kind == T_UINT32 || v->kind == T_UINT64) ||
	    (v->kind == T_UINT128 && v->u_hi == 0) || (v->kind == T_INT32 && (int64_t)v->u >= 0)) {
		*out = v->u;
		return 0;
	}
	err_set("invalid metadata: %s has MMDB type %u, not an unsigned integer", name, v->kind);
	return -1;
}

int mmdb_read_node(const mmdb_reader *r, uint64_t node, uint64_t *left, uint64_t *right)
{
	uint64_t off = node * r->node_bytes;
	if (off > r->size || r->size - off < r->node_bytes) {
		err_set("bounds check failed during tree traversal");
		return -1;
	}
	const uint8_t *b = r->map + off;
	switch (r->record_size) {
	case 24:
		*left = (uint64_t)b[0] << 16 | (uint64_t)b[1] << 8 | b[2];
		*right = (uint64_t)b[3] << 16 | (uint64_t)b[4] << 8 | b[5];
		break;
	case 28:
		*left = ((uint64_t)(b[3] & 0xF0) << 20) | (uint64_t)b[0] << 16 | (uint64_t)b[1] << 8 | b[2];
		*right = ((uint64_t)(b[3] & 0x0F) << 24) | (uint64_t)b[4] << 16 | (uint64_t)b[5] << 8 | b[6];
		break;
	default:
		*left = (uint64_t)b[0] << 24 | (uint64_t)b[1] << 16 | (uint64_t)b[2] << 8 | b[3];
		*right = (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 | (uint64_t)b[6] << 8 | b[7];
	}
	return 0;
}

int mmdb_open(mmdb_reader *r, const char *path)
{
	memset(r, 0, sizeof *r);
	r->path = path;
	arena_init(&r->meta_arena);
	const uint8_t *m;
	size_t size;
	if (map_file(path, &m, &size) < 0)
		return -1;
	r->size = size;
	r->map = m;

	size_t marker = (size_t)-1;
	if (r->size >= MARKER_LEN) {
		for (size_t i = r->size - MARKER_LEN + 1; i-- > 0;) {
			if (r->map[i] == 0xAB && memcmp(r->map + i, MARKER, MARKER_LEN) == 0) {
				marker = i;
				break;
			}
		}
	}
	if (marker == (size_t)-1) {
		err_set("error opening database: invalid MaxMind DB file");
		return -1;
	}
	r->meta.buf = r->map + marker + MARKER_LEN;
	r->meta.len = r->size - marker - MARKER_LEN;
	uint64_t next;
	if (mdec_value(&r->meta, 0, &r->meta_arena, &r->meta_root, &next) < 0)
		return -1;
	if (r->meta_root->kind != T_MAP) {
		err_set("invalid metadata: not a map");
		return -1;
	}
	const dnode *m0 = r->meta_root;
	for (uint32_t i = 0; i < m0->n; i++) {
		const dnode *k = &m0->items[2 * i], *v = &m0->items[2 * i + 1];
		if (key_is(k, "binary_format_major_version")) {
			if (meta_uint(v, "binary_format_major_version", &r->major) < 0)
				return -1;
		} else if (key_is(k, "binary_format_minor_version")) {
			if (meta_uint(v, "binary_format_minor_version", &r->minor) < 0)
				return -1;
		} else if (key_is(k, "build_epoch")) {
			if (meta_uint(v, "build_epoch", &r->build_epoch) < 0)
				return -1;
		} else if (key_is(k, "ip_version")) {
			if (meta_uint(v, "ip_version", &r->ip_version) < 0)
				return -1;
		} else if (key_is(k, "node_count")) {
			if (meta_uint(v, "node_count", &r->node_count) < 0)
				return -1;
		} else if (key_is(k, "record_size")) {
			if (meta_uint(v, "record_size", &r->record_size) < 0)
				return -1;
		} else if (key_is(k, "database_type")) {
			if (v->kind != T_STRING) {
				err_set("invalid metadata: database_type is not a string");
				return -1;
			}
			r->database_type = v->s;
			r->database_type_len = v->n;
		} else if (key_is(k, "description")) {
			if (v->kind != T_MAP) {
				err_set("invalid metadata: description is not a map");
				return -1;
			}
			for (uint32_t j = 0; j < v->n; j++)
				if (v->items[2 * j + 1].kind != T_STRING) {
					err_set("invalid metadata: description values must be strings");
					return -1;
				}
			r->description = v->items;
			r->ndescription = v->n;
		} else if (key_is(k, "languages")) {
			if (v->kind != T_ARRAY) {
				err_set("invalid metadata: languages is not an array");
				return -1;
			}
			for (uint32_t j = 0; j < v->n; j++)
				if (v->items[j].kind != T_STRING) {
					err_set("invalid metadata: languages must be strings");
					return -1;
				}
			r->languages = v->items;
			r->nlanguages = v->n;
		}
	}
	uint64_t quarter = r->record_size / 4;
	if (r->node_count > 0 && quarter > 0 && r->node_count > UINT64_MAX / quarter) {
		err_set("database tree size would overflow");
		return -1;
	}
	r->node_bytes = quarter;
	r->tree_size = r->node_count * quarter;
	uint64_t data_start = r->tree_size + 16, data_end = marker;
	if (r->tree_size > UINT64_MAX - 16 || data_start > data_end) {
		err_set("the MaxMind DB contains invalid metadata");
		return -1;
	}
	r->data.buf = r->map + data_start;
	r->data.len = data_end - data_start;

	if (r->ip_version != 6) {
		r->ipv4_start_bit_depth = 96;
		return 0;
	}
	if (r->record_size != 24 && r->record_size != 28 && r->record_size != 32) {
		err_set("unknown record size: %llu", (unsigned long long)r->record_size);
		return -1;
	}
	uint64_t node = 0;
	int i = 0;
	for (; i < 96 && node < r->node_count; i++) {
		uint64_t left, right;
		if (mmdb_read_node(r, node, &left, &right) < 0)
			return -1;
		node = left;
	}
	r->ipv4_start = node;
	r->ipv4_start_bit_depth = i;
	return 0;
}

void mmdb_close(mmdb_reader *r)
{
	if (r->map)
		unmap_file(r->map, r->size);
	arena_free(&r->meta_arena);
	memset(r, 0, sizeof *r);
}

/* ---- lookups (reader.go) ---- */

int mmdb_lookup(const mmdb_reader *r, u128 ip, bool is4, uint64_t *offset, bool *found)
{
	int bits;
	return mmdb_lookup_net(r, ip, is4, offset, found, &bits);
}

int mmdb_lookup_net(const mmdb_reader *r, u128 ip, bool is4, uint64_t *offset, bool *found, int *prefix)
{
	*found = false;
	uint64_t node = 0;
	int bits = 128, depth = 0;
	if (r->ip_version == 4 && !is4) {
		err_set("you attempted to look up an IPv6 address in an IPv4-only database");
		return -1;
	}
	if (is4) {
		/* IPv4 sits at ::a.b.c.d; ipv4_start is the ::/96 node (or where the
		 * tree ended above it) */
		node = r->ip_version == 6 ? r->ipv4_start : 0;
		depth = r->ip_version == 6 ? r->ipv4_start_bit_depth : 96;
		bits = 32;
		ip <<= 96;
	}
	int i = 0;
	for (; i < bits && node < r->node_count; i++) {
		uint64_t left, right;
		if (mmdb_read_node(r, node, &left, &right) < 0)
			return -1;
		node = (ip >> (127 - i) & 1) ? right : left;
	}
	*prefix = depth + i;
	if (node == r->node_count)
		return 0;
	if (node > r->node_count) {
		if (node < r->node_count + 16 || node - r->node_count - 16 >= r->data.len) {
			err_set("the MaxMind DB file's search tree is corrupt");
			return -1;
		}
		*offset = node - r->node_count - 16;
		*found = true;
		return 0;
	}
	err_set("invalid node in search tree");
	return -1;
}

/* ---- networks (traverse.go) ---- */

void net_iter_init(net_iter *it, const mmdb_reader *r)
{
	memset(it, 0, sizeof *it);
	it->r = r;
	it->cap = 160;
	it->stack = xmalloc(it->cap * sizeof *it->stack);
	it->stack[0] = (net_node){0, r->ip_version == 4 ? 96 : 0, 0};
	it->n = 1;
}

void net_iter_free(net_iter *it)
{
	free(it->stack);
	it->stack = NULL;
}

static bool in_ipv4_subtree(u128 ip) { return (ip >> 32) == 0; }

int net_iter_next(net_iter *it, u128 *ip, int *bit, uint64_t *offset)
{
	const mmdb_reader *r = it->r;
	while (it->n > 0) {
		net_node node = it->stack[--it->n];
		for (;;) {
			if (node.pointer == r->node_count)
				break;
			if (r->ipv4_start != 0 && node.pointer == r->ipv4_start && !in_ipv4_subtree(node.ip))
				break;
			if (node.pointer > r->node_count) {
				if (node.pointer < r->node_count + 16 || node.pointer - r->node_count - 16 >= r->data.len) {
					err_set("the MaxMind DB file's search tree is corrupt");
					return -1;
				}
				*ip = node.ip;
				*bit = node.bit;
				*offset = node.pointer - r->node_count - 16;
				return 1;
			}
			if (node.bit >= 128) {
				char net[80];
				char a[64];
				snprintf(net, sizeof net, "%s/%d", fmt_addr(node.ip, a), node.bit);
				err_set("invalid search tree at %s", net);
				return -1;
			}
			uint64_t left, right;
			if (mmdb_read_node(r, node.pointer, &left, &right) < 0)
				return -1;
			u128 ip_right = node.ip | ((u128)1 << (127 - node.bit));
			node.bit++;
			if (it->n == it->cap) {
				it->cap *= 2;
				it->stack = xrealloc(it->stack, it->cap * sizeof *it->stack);
			}
			it->stack[it->n++] = (net_node){ip_right, node.bit, right};
			node.pointer = left;
		}
	}
	return 0;
}

mval *dnode_to_mval(arena *a, const dnode *v)
{
	mval *m = arena_alloc(a, sizeof *m);
	memset(m, 0, sizeof *m);
	m->type = (uint8_t)v->kind;
	m->n = v->n;
	m->s = v->s;
	m->u = v->u;
	m->u_hi = v->u_hi;
	m->f = v->f;
	if (v->kind == T_MAP || v->kind == T_ARRAY) {
		size_t k = v->kind == T_MAP ? 2 * (size_t)v->n : v->n;
		m->items = arena_alloc(a, sizeof *m->items * (k + 1));
		for (size_t i = 0; i < k; i++)
			m->items[i] = *dnode_to_mval(a, &v->items[i]);
	}
	return m;
}

/* ---- verify (verifier.go) ---- */

typedef struct {
	const mmdb_reader *r;
	uint8_t *states;
	ht64 offsets;
} tree_walk;

static int verify_node(tree_walk *w, uint64_t node, unsigned depth, unsigned *height);

static int verify_pointer(tree_walk *w, uint64_t p, unsigned depth, unsigned *height)
{
	const mmdb_reader *r = w->r;
	*height = 0;
	if (p == r->node_count)
		return 0;
	if (p < r->node_count)
		return verify_node(w, p, depth, height);
	if (p < r->node_count + 16 || p - r->node_count - 16 >= r->data.len) {
		err_set("the MaxMind DB file's search tree is corrupt");
		return -1;
	}
	uint32_t dummy;
	ht64_get_or_put(&w->offsets, p - r->node_count - 16, 0, &dummy);
	return 0;
}

static int verify_node(tree_walk *w, uint64_t node, unsigned depth, unsigned *height)
{
	if (depth >= 128) {
		err_set("invalid search tree: internal node at bit depth %u", depth);
		return -1;
	}
	uint8_t st = w->states[node];
	if (st == 255) {
		err_set("invalid search tree: cycle at node %llu", (unsigned long long)node);
		return -1;
	}
	if (st != 0) {
		*height = st;
		return 0;
	}
	w->states[node] = 255;
	uint64_t left, right;
	if (mmdb_read_node(w->r, node, &left, &right) < 0)
		return -1;
	unsigned lh, rh;
	if (verify_pointer(w, left, depth + 1, &lh) < 0 || verify_pointer(w, right, depth + 1, &rh) < 0)
		return -1;
	unsigned mh = lh > rh ? lh : rh;
	if (mh == 128) {
		err_set("invalid search tree: path exceeds 128 bits");
		return -1;
	}
	w->states[node] = (uint8_t)(mh + 1);
	*height = mh + 1;
	return 0;
}

static bool bit_has(const uint64_t *b, uint64_t i) { return b[i >> 6] >> (i & 63) & 1; }
static void bit_set(uint64_t *b, uint64_t i) { b[i >> 6] |= (uint64_t)1 << (i & 63); }

/* verifyFieldPointers: field starts, and every pointer lands on one. */
static int verify_field_pointers(const mdec *d, const char *section, uint64_t **starts_out)
{
	uint64_t end = d->len;
	uint64_t *starts = xcalloc((size_t)(end / 64 + 1), sizeof *starts);
	bool forward = false;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && !forward)
			break;
		for (uint64_t off = 0; off < end;) {
			uint32_t kind;
			uint64_t size, next, target = 0;
			if (ctrl(d, off, &kind, &size, &next) < 0) {
				err_prefix("received decoding error at offset of %llu", (unsigned long long)off);
				free(starts);
				return -1;
			}
			if (kind == T_POINTER) {
				if (pointer_at(d, size, next, &target, &next) < 0) {
					free(starts);
					return -1;
				}
			} else if (kind != T_MAP && kind != T_ARRAY && kind != T_BOOL) {
				next += size;
			}
			if (pass == 0) {
				bit_set(starts, off);
				if (kind == T_POINTER) {
					if (target > off)
						forward = true;
					else if (!bit_has(starts, target)) {
						err_set("%s pointer at offset %llu does not point to the start of a field (offset %llu)",
							section, (unsigned long long)off, (unsigned long long)target);
						free(starts);
						return -1;
					}
				}
			} else if (kind == T_POINTER && target > off && (target >= end || !bit_has(starts, target))) {
				err_set("%s pointer at offset %llu does not point to the start of a field (offset %llu)",
					section, (unsigned long long)off, (unsigned long long)target);
				free(starts);
				return -1;
			}
			off = next;
		}
	}
	if (starts_out)
		*starts_out = starts;
	else
		free(starts);
	return 0;
}

static int verify_metadata(const mmdb_reader *r)
{
	arena a;
	arena_init(&a);
	uint64_t off = 0;
	bool first = true;
	while (first || off < r->meta.len) {
		dnode *v;
		uint64_t next;
		if (mdec_value(&r->meta, off, &a, &v, &next) < 0) {
			if (!first)
				err_prefix("invalid value after the metadata map at offset of %llu", (unsigned long long)off);
			arena_free(&a);
			return -1;
		}
		if (!dnode_utf8_ok(v)) {
			err_set("invalid UTF-8 string in the metadata");
			arena_free(&a);
			return -1;
		}
		off = next;
		first = false;
		arena_reset(&a);
	}
	arena_free(&a);
	if (verify_field_pointers(&r->meta, "metadata", NULL) < 0)
		return -1;

	if (r->major != 2) {
		err_set("binary_format_major_version - Expected: 2 Actual: %llu", (unsigned long long)r->major);
		return -1;
	}
	if (r->minor != 0) {
		err_set("binary_format_minor_version - Expected: 0 Actual: %llu", (unsigned long long)r->minor);
		return -1;
	}
	if (r->database_type_len == 0) {
		err_set("database_type - Expected: non-empty string Actual: ");
		return -1;
	}
	if (!utf8_valid(r->database_type, r->database_type_len)) {
		err_set("database_type contains invalid UTF-8");
		return -1;
	}
	for (uint32_t i = 0; i < 2 * r->ndescription; i++)
		if (!utf8_valid(r->description[i].s, r->description[i].n)) {
			err_set("description contains invalid UTF-8");
			return -1;
		}
	for (uint32_t i = 0; i < r->nlanguages; i++)
		if (!utf8_valid(r->languages[i].s, r->languages[i].n)) {
			err_set("languages contains invalid UTF-8");
			return -1;
		}
	if (r->ndescription == 0) {
		err_set("description - Expected: non-empty map Actual: map[]");
		return -1;
	}
	if (r->ip_version != 4 && r->ip_version != 6) {
		err_set("ip_version - Expected: 4 or 6 Actual: %llu", (unsigned long long)r->ip_version);
		return -1;
	}
	if (r->record_size != 24 && r->record_size != 28 && r->record_size != 32) {
		err_set("record_size - Expected: 24, 28, or 32 Actual: %llu", (unsigned long long)r->record_size);
		return -1;
	}
	if (r->node_count == 0) {
		err_set("node_count - Expected: positive integer Actual: 0");
		return -1;
	}
	return 0;
}

static int verify_data_section(const mmdb_reader *r, ht64 *offsets)
{
	const mdec *d = &r->data;
	arena a;
	arena_init(&a);
	uint64_t off = 0;
	int rc = -1;
	uint64_t *starts = NULL;
	while (off < d->len) {
		dnode *v;
		uint64_t next;
		if (mdec_value(d, off, &a, &v, &next) < 0) {
			char inner[1024];
			snprintf(inner, sizeof inner, "%s", err_msg());
			err_set("received decoding error (%s) at offset of %llu", inner, (unsigned long long)off);
			goto out;
		}
		if (!dnode_utf8_ok(v)) {
			err_set("received validation error (invalid UTF-8 string) at offset of %llu", (unsigned long long)off);
			goto out;
		}
		if (next <= off) {
			err_set("data section offset unexpectedly went from %llu to %llu", (unsigned long long)off,
				(unsigned long long)next);
			goto out;
		}
		uint32_t dummy;
		if (!ht64_get(offsets, off, &dummy)) {
			buf_t j = {0};
			dnode_json(v, &j);
			if (j.len > 300)
				j.len = 300;
			err_set("found data (%s) at %llu that the search tree does not point to", buf_cstr(&j),
				(unsigned long long)off);
			buf_free(&j);
			goto out;
		}
		off = next;
		arena_reset(&a);
	}
	if (off != d->len) {
		err_set("unexpected data at the end of the data section (last offset: %llu, end: %llu)",
			(unsigned long long)off, (unsigned long long)d->len);
		goto out;
	}
	if (verify_field_pointers(d, "data section", &starts) < 0)
		goto out;
	uint64_t missing = 0, bad = d->len;
	for (size_t i = 0; i < offsets->cap; i++) {
		if (!(offsets->used[i >> 6] >> (i & 63) & 1))
			continue;
		uint64_t t = offsets->keys[i];
		if (t >= d->len)
			missing++;
		else if (!bit_has(starts, t) && t < bad)
			bad = t;
	}
	if (bad != d->len) {
		err_set("search tree points into the middle of a field in the data section (offset %llu)",
			(unsigned long long)bad);
		goto out;
	}
	if (missing) {
		err_set("found %llu pointers (of %zu) in the search tree that we did not see in the data section",
			(unsigned long long)missing, offsets->len);
		goto out;
	}
	rc = 0;
out:
	free(starts);
	arena_free(&a);
	return rc;
}

int mmdb_verify(const mmdb_reader *r)
{
	if (verify_metadata(r) < 0)
		return -1;
	if (r->record_size != 24 && r->record_size != 28 && r->record_size != 32) {
		err_set("unsupported record size");
		return -1;
	}
	if (r->node_bytes == 0 || r->node_count > r->size / r->node_bytes) {
		err_set("bounds check failed during search tree verification");
		return -1;
	}
	tree_walk w = {.r = r};
	w.states = xcalloc((size_t)r->node_count, 1);
	ht64_init(&w.offsets);
	unsigned depth = r->ip_version == 4 ? 96 : 0, height;
	int rc = verify_node(&w, 0, depth, &height);
	free(w.states);
	if (rc == 0 && depth + height > 128) {
		err_set("invalid search tree: path exceeds 128 bits");
		rc = -1;
	}
	if (rc == 0) {
		uint64_t sep = r->tree_size;
		if (sep + 16 > r->size) {
			err_set("unexpected end of database while reading data section separator");
			rc = -1;
		} else {
			for (int i = 0; i < 16; i++)
				if (r->map[sep + (uint64_t)i] != 0) {
					err_set("unexpected byte in data separator");
					rc = -1;
					break;
				}
		}
	}
	if (rc == 0)
		rc = verify_data_section(r, &w.offsets);
	ht64_free(&w.offsets);
	return rc;
}
