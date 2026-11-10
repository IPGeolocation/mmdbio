#include "jstream.h"

#include <stdlib.h>
#include <string.h>

#define JS_BUF (1u << 20)
#define MAX_DEPTH 256

void js_init(jstream *j, csv_read_fn fn, void *ctx, arena *a)
{
	memset(j, 0, sizeof *j);
	j->fn = fn;
	j->ctx = ctx;
	j->buf = xmalloc(JS_BUF);
	j->line = 1;
	j->a = a;
}

void js_free(jstream *j)
{
	free(j->buf);
	buf_free(&j->tmp);
	memset(j, 0, sizeof *j);
}

static int fill(jstream *j)
{
	if (j->eof)
		return 0;
	ssize_t n = j->fn(j->ctx, j->buf, JS_BUF);
	if (n < 0)
		return -1;
	if (n == 0)
		j->eof = true;
	j->len = (size_t)n;
	j->pos = 0;
	return 0;
}

/* the next byte (-1 at the end, -2 on a read error) */
static inline int getb(jstream *j)
{
	if (j->pos == j->len) {
		if (fill(j) < 0)
			return -2;
		if (j->pos == j->len)
			return -1;
	}
	int c = j->buf[j->pos++];
	if (c == '\n')
		j->line++;
	return c;
}

static inline int peekb(jstream *j)
{
	if (j->pos == j->len) {
		if (fill(j) < 0)
			return -2;
		if (j->pos == j->len)
			return -1;
	}
	return j->buf[j->pos];
}

static int fail(jstream *j, const char *msg)
{
	err_set("line %ld: %s", j->line, msg);
	return -1;
}

static int unexpected(jstream *j, int c)
{
	if (c == -2)
		return -1;
	if (c == -1)
		return fail(j, "unexpected end of JSON input");
	char msg[64];
	if (c >= 0x20 && c < 0x7f)
		snprintf(msg, sizeof msg, "invalid character '%c'", c);
	else
		snprintf(msg, sizeof msg, "invalid byte 0x%02x", c);
	return fail(j, msg);
}

int js_peek(jstream *j)
{
	for (;;) {
		int c = peekb(j);
		if (c < 0)
			return c;
		if (c == 0xEF && j->line == 1) { /* a byte order mark */
			getb(j);
			if (getb(j) != 0xBB || getb(j) != 0xBF)
				return unexpected(j, 0xEF), -2;
			continue;
		}
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			return c;
		getb(j);
	}
}

int js_expect(jstream *j, char want)
{
	int c = js_peek(j);
	if (c != (unsigned char)want)
		return unexpected(j, c);
	getb(j);
	return 0;
}

static int hex4(jstream *j, uint32_t *out)
{
	uint32_t v = 0;
	for (int k = 0; k < 4; k++) {
		int c = getb(j);
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0)
			return fail(j, "invalid \\u escape");
		v = v << 4 | (uint32_t)d;
	}
	*out = v;
	return 0;
}

static void put_utf8(buf_t *b, uint32_t r)
{
	if (r < 0x80) {
		buf_push(b, (uint8_t)r);
	} else if (r < 0x800) {
		buf_push(b, (uint8_t)(0xC0 | r >> 6));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	} else if (r < 0x10000) {
		buf_push(b, (uint8_t)(0xE0 | r >> 12));
		buf_push(b, (uint8_t)(0x80 | (r >> 6 & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	} else {
		buf_push(b, (uint8_t)(0xF0 | r >> 18));
		buf_push(b, (uint8_t)(0x80 | (r >> 12 & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | (r >> 6 & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	}
}

static char *arena_copy(arena *a, const uint8_t *p, size_t n)
{
	char *d = arena_alloc(a, n + 1);
	if (n)
		memcpy(d, p, n);
	d[n] = 0;
	return d;
}

int js_string(jstream *j, const char **s, size_t *n)
{
	if (getb(j) != '"')
		return fail(j, "expected a string");
	buf_t *b = &j->tmp;
	b->len = 0;
	for (;;) {
		/* copy a run of plain bytes at once */
		if (j->pos < j->len) {
			size_t start = j->pos;
			while (j->pos < j->len) {
				uint8_t c = j->buf[j->pos];
				if (c == '"' || c == '\\' || c < 0x20)
					break;
				j->pos++;
			}
			if (j->pos > start)
				buf_append(b, j->buf + start, j->pos - start);
		}
		int c = getb(j);
		if (c < 0)
			return c == -2 ? -1 : fail(j, "unexpected end of JSON input in a string");
		if (c == '"')
			break;
		if (c < 0x20)
			return fail(j, "invalid control character in a string");
		if (c != '\\') {
			buf_push(b, (uint8_t)c);
			continue;
		}
		c = getb(j);
		switch (c) {
		case '"':
		case '\\':
		case '/':
			buf_push(b, (uint8_t)c);
			break;
		case 'b':
			buf_push(b, '\b');
			break;
		case 'f':
			buf_push(b, '\f');
			break;
		case 'n':
			buf_push(b, '\n');
			break;
		case 'r':
			buf_push(b, '\r');
			break;
		case 't':
			buf_push(b, '\t');
			break;
		case 'u': {
			uint32_t r;
			if (hex4(j, &r) < 0)
				return -1;
			if (r >= 0xD800 && r <= 0xDBFF && peekb(j) == '\\') {
				getb(j);
				uint32_t lo;
				if (getb(j) != 'u' || hex4(j, &lo) < 0)
					return fail(j, "invalid \\u escape");
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
				} else {
					put_utf8(b, 0xFFFD);
					r = lo;
				}
			}
			if (r >= 0xD800 && r <= 0xDFFF)
				r = 0xFFFD; /* a lone surrogate, as Go decodes it */
			put_utf8(b, r);
			break;
		}
		default:
			return fail(j, "invalid escape in a string");
		}
	}
	*s = arena_copy(j->a, b->p, b->len);
	*n = b->len;
	return 0;
}

static int number(jstream *j, jnode *out)
{
	buf_t *b = &j->tmp;
	b->len = 0;
	int c = peekb(j);
	if (c == '-') {
		buf_push(b, (uint8_t)getb(j));
		c = peekb(j);
	}
	if (c < '0' || c > '9')
		return unexpected(j, c);
	if (c == '0') {
		buf_push(b, (uint8_t)getb(j));
	} else {
		while ((c = peekb(j)) >= '0' && c <= '9')
			buf_push(b, (uint8_t)getb(j));
	}
	if (peekb(j) == '.') {
		buf_push(b, (uint8_t)getb(j));
		if ((c = peekb(j)) < '0' || c > '9')
			return unexpected(j, c);
		while ((c = peekb(j)) >= '0' && c <= '9')
			buf_push(b, (uint8_t)getb(j));
	}
	c = peekb(j);
	if (c == 'e' || c == 'E') {
		buf_push(b, (uint8_t)getb(j));
		c = peekb(j);
		if (c == '+' || c == '-') {
			buf_push(b, (uint8_t)getb(j));
			c = peekb(j);
		}
		if (c < '0' || c > '9')
			return unexpected(j, c);
		while ((c = peekb(j)) >= '0' && c <= '9')
			buf_push(b, (uint8_t)getb(j));
	}
	out->type = JN_NUM;
	out->s = arena_copy(j->a, b->p, b->len);
	out->len = b->len;
	return 0;
}

static int literal(jstream *j, const char *word)
{
	for (const char *p = word; *p; p++) {
		int c = getb(j);
		if (c != (unsigned char)*p)
			return unexpected(j, c);
	}
	return 0;
}

typedef struct {
	jnode *v;
	const char **k;
	size_t *kl;
	size_t n, cap;
} members;

static void members_push(members *m, const char *k, size_t kl, const jnode *v)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 8;
		m->v = xrealloc(m->v, m->cap * sizeof *m->v);
		m->k = xrealloc(m->k, m->cap * sizeof *m->k);
		m->kl = xrealloc(m->kl, m->cap * sizeof *m->kl);
	}
	m->v[m->n] = *v;
	m->k[m->n] = k;
	m->kl[m->n] = kl;
	m->n++;
}

static void members_done(jstream *j, members *m, jnode *out, bool object)
{
	out->n = (uint32_t)m->n;
	out->items = arena_alloc(j->a, sizeof *out->items * (m->n + 1));
	if (m->n)
		memcpy(out->items, m->v, sizeof *out->items * m->n);
	if (object) {
		out->keys = arena_alloc(j->a, sizeof *out->keys * (m->n + 1));
		out->klens = arena_alloc(j->a, sizeof *out->klens * (m->n + 1));
		if (m->n) {
			memcpy(out->keys, m->k, sizeof *out->keys * m->n);
			memcpy(out->klens, m->kl, sizeof *out->klens * m->n);
		}
	}
	free(m->v);
	free(m->k);
	free(m->kl);
}

static int object_members(jstream *j, members *m)
{
	for (;;) {
		int c = js_peek(j);
		if (c == '}') {
			getb(j);
			return 0;
		}
		if (c != ',')
			return unexpected(j, c);
		getb(j);
		if ((c = js_peek(j)) != '"')
			return unexpected(j, c);
		const char *k;
		size_t kl;
		if (js_string(j, &k, &kl) < 0 || js_expect(j, ':') < 0)
			return -1;
		jnode v;
		if (js_value(j, &v) < 0)
			return -1;
		members_push(m, k, kl, &v);
	}
}

int js_object_rest(jstream *j, const char *key, size_t klen, const jnode *first, jnode *out)
{
	members m = {0};
	members_push(&m, key, klen, first);
	memset(out, 0, sizeof *out);
	out->type = JN_OBJ;
	out->line = first->line;
	int rc = object_members(j, &m);
	members_done(j, &m, out, true);
	return rc;
}

int js_value(jstream *j, jnode *out)
{
	memset(out, 0, sizeof *out);
	int c = js_peek(j);
	out->line = j->line;
	if (c < 0)
		return unexpected(j, c);
	if (++j->depth > MAX_DEPTH) {
		j->depth--;
		return fail(j, "JSON nested too deeply");
	}
	int rc = 0;
	switch (c) {
	case '{': {
		getb(j);
		out->type = JN_OBJ;
		members m = {0};
		if (js_peek(j) == '}') {
			getb(j);
		} else {
			if ((c = js_peek(j)) != '"') {
				rc = unexpected(j, c);
			} else {
				const char *k;
				size_t kl;
				jnode v;
				if (js_string(j, &k, &kl) < 0 || js_expect(j, ':') < 0 || js_value(j, &v) < 0) {
					rc = -1;
				} else {
					members_push(&m, k, kl, &v);
					rc = object_members(j, &m);
				}
			}
		}
		members_done(j, &m, out, true);
		break;
	}
	case '[': {
		getb(j);
		out->type = JN_ARR;
		members m = {0};
		if (js_peek(j) == ']') {
			getb(j);
		} else {
			for (;;) {
				jnode v;
				if (js_value(j, &v) < 0) {
					rc = -1;
					break;
				}
				members_push(&m, NULL, 0, &v);
				c = js_peek(j);
				if (c == ']') {
					getb(j);
					break;
				}
				if (c != ',') {
					rc = unexpected(j, c);
					break;
				}
				getb(j);
			}
		}
		members_done(j, &m, out, false);
		break;
	}
	case '"':
		out->type = JN_STR;
		rc = js_string(j, &out->s, &out->len);
		break;
	case 't':
		out->type = JN_BOOL;
		out->b = true;
		rc = literal(j, "true");
		break;
	case 'f':
		out->type = JN_BOOL;
		rc = literal(j, "false");
		break;
	case 'n':
		out->type = JN_NULL;
		rc = literal(j, "null");
		break;
	default:
		rc = number(j, out);
	}
	j->depth--;
	return rc;
}
