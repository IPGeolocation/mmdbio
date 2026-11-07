#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "utf8.h"

#define MAX_DEPTH 64

typedef struct {
	const uint8_t *p;
	size_t n, i;
	int line, col, depth;
} parser;

static int fail(const parser *ps, const char *msg)
{
	err_set("line %d, column %d: %s", ps->line, ps->col, msg);
	return -1;
}

static void advance(parser *ps)
{
	uint8_t c = ps->p[ps->i++];
	if (c == '\n') {
		ps->line++;
		ps->col = 1;
	} else if ((c & 0xC0) != 0x80) {
		ps->col++;
	}
}

static void skip_ws(parser *ps)
{
	while (ps->i < ps->n) {
		uint8_t c = ps->p[ps->i];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
		advance(ps);
	}
}

static int unexpected(const parser *ps)
{
	if (ps->i >= ps->n)
		return fail(ps, "unexpected end of input");
	uint8_t c = ps->p[ps->i];
	char msg[48];
	if (c >= 0x20 && c < 0x7f)
		snprintf(msg, sizeof msg, "unexpected character '%c'", c);
	else
		snprintf(msg, sizeof msg, "unexpected byte 0x%02x", c);
	return fail(ps, msg);
}

static bool eat(parser *ps, char c)
{
	if (ps->i < ps->n && ps->p[ps->i] == (uint8_t)c) {
		advance(ps);
		return true;
	}
	return false;
}

static int hex4(parser *ps, uint32_t *out)
{
	uint32_t v = 0;
	for (int k = 0; k < 4; k++) {
		if (ps->i >= ps->n)
			return fail(ps, "unexpected end of input in a \\u escape");
		uint8_t c = ps->p[ps->i];
		int d = c >= '0' && c <= '9' ? c - '0'
			: c >= 'a' && c <= 'f' ? c - 'a' + 10
			: c >= 'A' && c <= 'F' ? c - 'A' + 10
					       : -1;
		if (d < 0)
			return fail(ps, "a \\u escape needs four hex digits");
		v = v << 4 | (uint32_t)d;
		advance(ps);
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

/* At the opening quote. */
static int parse_string(parser *ps, char **out, size_t *len, bool *has_nul)
{
	advance(ps);
	buf_t b = {0};
	*has_nul = false;
	for (;;) {
		if (ps->i >= ps->n) {
			buf_free(&b);
			return fail(ps, "unexpected end of input in a string");
		}
		uint8_t c = ps->p[ps->i];
		if (c == '"') {
			advance(ps);
			break;
		}
		if (c < 0x20) {
			buf_free(&b);
			return fail(ps, "a control character in a string must be escaped");
		}
		if (c != '\\') {
			buf_push(&b, c);
			advance(ps);
			continue;
		}
		advance(ps);
		if (ps->i >= ps->n) {
			buf_free(&b);
			return fail(ps, "unexpected end of input in a string");
		}
		c = ps->p[ps->i];
		const char *simple = strchr("\"\\/bfnrt", c);
		if (c && simple) {
			static const char repl[] = "\"\\/\b\f\n\r\t";
			buf_push(&b, (uint8_t)repl[simple - "\"\\/bfnrt"]);
			advance(ps);
			continue;
		}
		if (c != 'u') {
			buf_free(&b);
			return fail(ps, "invalid escape in a string");
		}
		advance(ps);
		uint32_t r;
		if (hex4(ps, &r) < 0) {
			buf_free(&b);
			return -1;
		}
		if (r >= 0xDC00 && r <= 0xDFFF) {
			buf_free(&b);
			return fail(ps, "a \\u escape holds a low surrogate without a high one");
		}
		if (r >= 0xD800 && r <= 0xDBFF) {
			uint32_t lo;
			if (!eat(ps, '\\') || !eat(ps, 'u') || hex4(ps, &lo) < 0 || lo < 0xDC00 || lo > 0xDFFF) {
				buf_free(&b);
				return fail(ps, "a \\u escape holds a high surrogate without a low one");
			}
			r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
		}
		if (r == 0)
			*has_nul = true;
		put_utf8(&b, r);
	}
	*len = b.len;
	buf_push(&b, 0);
	*out = (char *)b.p;
	return 0;
}

static bool is_digit(const parser *ps)
{
	return ps->i < ps->n && ps->p[ps->i] >= '0' && ps->p[ps->i] <= '9';
}

static int parse_number(parser *ps, jvalue *v)
{
	size_t start = ps->i;
	eat(ps, '-');
	if (!is_digit(ps))
		return fail(ps, "a number needs a digit here");
	if (!eat(ps, '0'))
		while (is_digit(ps))
			advance(ps);
	if (eat(ps, '.')) {
		if (!is_digit(ps))
			return fail(ps, "a number needs a digit after the decimal point");
		while (is_digit(ps))
			advance(ps);
	}
	if (eat(ps, 'e') || eat(ps, 'E')) {
		if (!eat(ps, '+'))
			eat(ps, '-');
		if (!is_digit(ps))
			return fail(ps, "a number needs a digit in the exponent");
		while (is_digit(ps))
			advance(ps);
	}
	size_t n = ps->i - start;
	v->raw = xmalloc(n + 1);
	memcpy(v->raw, ps->p + start, n);
	v->raw[n] = 0;
	v->num = strtod(v->raw, NULL);
	v->type = J_NUMBER;
	return 0;
}

static int parse_value(parser *ps, jvalue *v);

static void push_value(jvalue *arr, size_t *cap, jvalue *item)
{
	if (arr->len == *cap) {
		*cap = *cap ? *cap * 2 : 8;
		arr->items = xrealloc(arr->items, *cap * sizeof *arr->items);
		if (arr->type == J_OBJECT)
			arr->keys = xrealloc(arr->keys, *cap * sizeof *arr->keys);
	}
	arr->items[arr->len++] = *item;
}

static int parse_container(parser *ps, jvalue *v, bool object)
{
	char close = object ? '}' : ']';
	v->type = object ? J_OBJECT : J_ARRAY;
	if (++ps->depth > MAX_DEPTH)
		return fail(ps, "nested too deep");
	advance(ps);
	skip_ws(ps);
	size_t cap = 0;
	if (eat(ps, close)) {
		ps->depth--;
		return 0;
	}
	for (;;) {
		char *key = NULL;
		if (object) {
			if (ps->i >= ps->n || ps->p[ps->i] != '"')
				return ps->i >= ps->n ? unexpected(ps) : fail(ps, "an object key must be a string");
			int kline = ps->line, kcol = ps->col;
			size_t klen;
			bool nul;
			if (parse_string(ps, &key, &klen, &nul) < 0)
				return -1;
			for (size_t k = 0; k < v->len; k++) {
				if (strcmp(v->keys[k], key) == 0) {
					err_set("line %d, column %d: duplicate key \"%s\"", kline, kcol, key);
					free(key);
					return -1;
				}
			}
			if (nul) {
				err_set("line %d, column %d: an object key may not hold \\u0000", kline, kcol);
				free(key);
				return -1;
			}
			skip_ws(ps);
			if (!eat(ps, ':')) {
				free(key);
				return ps->i >= ps->n ? unexpected(ps) : fail(ps, "expected ':' after an object key");
			}
			skip_ws(ps);
		}
		jvalue item;
		int rc = parse_value(ps, &item);
		/* kept even on failure, so json_free releases what was parsed */
		push_value(v, &cap, &item);
		if (object)
			v->keys[v->len - 1] = key;
		if (rc < 0)
			return -1;
		skip_ws(ps);
		if (eat(ps, ','))
			skip_ws(ps);
		else if (eat(ps, close))
			break;
		else
			return ps->i >= ps->n ? unexpected(ps)
					      : fail(ps, object ? "expected ',' or '}' in an object" : "expected ',' or ']' in an array");
	}
	ps->depth--;
	return 0;
}

static int parse_literal(parser *ps, const char *word)
{
	size_t n = strlen(word);
	if (ps->n - ps->i < n || memcmp(ps->p + ps->i, word, n) != 0)
		return unexpected(ps);
	for (size_t k = 0; k < n; k++)
		advance(ps);
	return 0;
}

static int parse_value(parser *ps, jvalue *v)
{
	memset(v, 0, sizeof *v);
	v->line = ps->line;
	v->col = ps->col;
	if (ps->i >= ps->n)
		return unexpected(ps);
	switch (ps->p[ps->i]) {
	case '{':
		return parse_container(ps, v, true);
	case '[':
		return parse_container(ps, v, false);
	case '"':
		v->type = J_STRING;
		return parse_string(ps, &v->s, &v->n, &v->has_nul);
	case 't':
		v->type = J_BOOL;
		v->b = true;
		return parse_literal(ps, "true");
	case 'f':
		v->type = J_BOOL;
		return parse_literal(ps, "false");
	case 'n':
		return parse_literal(ps, "null");
	default:
		if (ps->p[ps->i] == '-' || is_digit(ps))
			return parse_number(ps, v);
		return unexpected(ps);
	}
}

int json_parse(const uint8_t *text, size_t n, jvalue *out)
{
	parser ps = {.p = text, .n = n, .line = 1, .col = 1};
	memset(out, 0, sizeof *out);
	while (ps.i < n) {
		uint32_t r;
		int w = go_decode_rune(text + ps.i, n - ps.i, &r);
		if (r == UTF8_RUNE_ERROR && w == 1)
			return fail(&ps, "the file is not valid UTF-8");
		for (int k = 0; k < w; k++)
			advance(&ps);
	}
	ps = (parser){.p = text, .n = n, .line = 1, .col = 1};
	if (n >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0)
		ps.i = 3; /* a byte order mark */
	skip_ws(&ps);
	if (parse_value(&ps, out) < 0) {
		json_free(out);
		return -1;
	}
	skip_ws(&ps);
	if (ps.i < n) {
		json_free(out);
		return fail(&ps, "unexpected text after the JSON value");
	}
	return 0;
}

void json_free(jvalue *v)
{
	for (size_t i = 0; i < v->len; i++) {
		json_free(&v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->s);
	free(v->raw);
	memset(v, 0, sizeof *v);
}

const jvalue *json_get(const jvalue *o, const char *key)
{
	if (o->type != J_OBJECT)
		return NULL;
	for (size_t i = 0; i < o->len; i++)
		if (strcmp(o->keys[i], key) == 0)
			return &o->items[i];
	return NULL;
}

const char *json_type_name(jtype t)
{
	static const char *const names[] = {"null", "a boolean", "a number", "a string", "an array", "an object"};
	return names[t];
}
