#include "tabular.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cmds.h"
#include "jsonw.h"

bool out_format_parse(const char *s, out_format *f)
{
	static const struct {
		const char *name;
		out_format f;
	} names[] = {
		{"json", OUT_JSON},	 {"jsonl", OUT_JSONL}, {"ndjson", OUT_JSONL}, {"json-compact", OUT_JSONL},
		{"json-pretty", OUT_JSON_PRETTY}, {"csv", OUT_CSV}, {"tsv", OUT_TSV},	{"cidr", OUT_CIDR},
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strcmp(s, names[i].name) == 0) {
			*f = names[i].f;
			return true;
		}
	return false;
}

static bool ends_with(const char *s, const char *suf)
{
	size_t n = strlen(s), m = strlen(suf);
	return n > m && strcasecmp(s + n - m, suf) == 0;
}

bool out_format_from_path(const char *path, out_format *f)
{
	if (ends_with(path, ".csv"))
		*f = OUT_CSV;
	else if (ends_with(path, ".tsv"))
		*f = OUT_TSV;
	else if (ends_with(path, ".jsonl") || ends_with(path, ".ndjson"))
		*f = OUT_JSONL;
	else if (ends_with(path, ".json"))
		*f = OUT_JSON;
	else
		return false;
	return true;
}

/* ---- columns ---- */

static uint64_t hash_bytes(const char *s, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
	return h;
}

void columns_init(columns *c, bool flatten)
{
	memset(c, 0, sizeof *c);
	c->flatten = flatten;
}

static void rehash(columns *c)
{
	size_t n = c->nslots ? c->nslots * 2 : 64;
	char **slots = xcalloc(n, sizeof *slots);
	for (size_t i = 0; i < c->n; i++) {
		size_t len = strlen(c->names[i]);
		size_t k = hash_bytes(c->names[i], len) & (n - 1);
		while (slots[k])
			k = (k + 1) & (n - 1);
		slots[k] = c->names[i];
	}
	free(c->slots);
	c->slots = slots;
	c->nslots = n;
}

void columns_add(columns *c, const char *name, size_t len)
{
	if (2 * (c->n + 1) > c->nslots)
		rehash(c);
	size_t k = hash_bytes(name, len) & (c->nslots - 1);
	while (c->slots[k]) {
		if (strlen(c->slots[k]) == len && memcmp(c->slots[k], name, len) == 0)
			return;
		k = (k + 1) & (c->nslots - 1);
	}
	char *copy = xmalloc(len + 1);
	if (len)
		memcpy(copy, name, len);
	copy[len] = 0;
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 32;
		c->names = xrealloc(c->names, c->cap * sizeof *c->names);
	}
	c->names[c->n++] = copy;
	c->slots[k] = copy;
}

static void add_paths(columns *c, const dnode *v, buf_t *path)
{
	size_t keep = path->len;
	for (uint32_t i = 0; i < v->n; i++) {
		const dnode *k = &v->items[2 * i], *x = &v->items[2 * i + 1];
		if (keep)
			buf_push(path, '.');
		buf_append(path, k->s, k->n);
		if (c->flatten && x->kind == T_MAP && x->n > 0)
			add_paths(c, x, path);
		else
			columns_add(c, (const char *)path->p, path->len);
		path->len = keep;
	}
}

void columns_add_record(columns *c, const dnode *v)
{
	if (v->kind != T_MAP) {
		columns_add(c, "value", 5);
		return;
	}
	buf_t path = {0};
	add_paths(c, v, &path);
	buf_free(&path);
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void columns_sort(columns *c)
{
	if (c->n > 1)
		qsort(c->names, c->n, sizeof *c->names, cmp_name);
}

void columns_free(columns *c)
{
	for (size_t i = 0; i < c->n; i++)
		free(c->names[i]);
	free(c->names);
	free(c->slots);
	memset(c, 0, sizeof *c);
}

const dnode *column_value(const dnode *rec, const char *name)
{
	if (rec->kind != T_MAP)
		return strcmp(name, "value") == 0 ? rec : NULL;
	/* a key with a dot in it wins over the path through it */
	size_t n = strlen(name);
	for (uint32_t i = 0; i < rec->n; i++) {
		const dnode *k = &rec->items[2 * i];
		if (k->n == n && memcmp(k->s, name, n) == 0)
			return &rec->items[2 * i + 1];
	}
	return extract_field(rec, name);
}

void cell_text(const dnode *v, buf_t *out)
{
	switch (v->kind) {
	case T_STRING:
		buf_append(out, v->s, v->n);
		break;
	case T_DOUBLE:
		json_float(out, v->f, 64);
		break;
	case T_FLOAT:
		json_float(out, v->f, 32);
		break;
	case T_MAP:
	case T_ARRAY:
		dnode_json(v, out);
		break;
	case T_BYTES: {
		/* base64, without the quotes JSON puts around it */
		size_t start = out->len;
		json_base64(out, v->s, v->n);
		memmove(out->p + start, out->p + start + 1, out->len - start - 2);
		out->len -= 2;
		break;
	}
	default:
		go_sprint(v, out);
	}
}

void row_append(buf_t *out, char delim, const char *const *cells, const size_t *lens, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (i)
			buf_push(out, (uint8_t)delim);
		const char *s = cells[i];
		size_t len = lens[i];
		bool quote = len > 0 && (s[0] == ' ' || s[0] == '\t' || (len == 2 && s[0] == '\\' && s[1] == '.'));
		for (size_t k = 0; k < len && !quote; k++)
			quote = s[k] == delim || s[k] == '"' || s[k] == '\n' || s[k] == '\r';
		if (!quote) {
			buf_append(out, s, len);
			continue;
		}
		buf_push(out, '"');
		for (size_t k = 0; k < len; k++) {
			if (s[k] == '"')
				buf_push(out, '"');
			buf_push(out, (uint8_t)s[k]);
		}
		buf_push(out, '"');
	}
	buf_push(out, '\n');
}

void row_write(FILE *f, char delim, const char *const *cells, const size_t *lens, size_t n)
{
	buf_t b = {0};
	row_append(&b, delim, cells, lens, n);
	fwrite(b.p, 1, b.len, f);
	buf_free(&b);
}

/* ---- JSON objects ---- */

typedef struct {
	const uint8_t *k;
	size_t kn;
	const dnode *v;   /* NULL: null, or the extra string */
	const char *str;  /* the extra key's value */
	bool extra;       /* the added key */
} member;

static int cmp_member(const void *a, const void *b)
{
	const member *x = a, *y = b;
	size_t n = x->kn < y->kn ? x->kn : y->kn;
	int c = n ? memcmp(x->k, y->k, n) : 0;
	return c ? c : (x->kn < y->kn ? -1 : x->kn > y->kn);
}

void json_row(buf_t *out, const dnode *rec, const char *key, const char *val, const strlist *fields, bool pretty,
	      size_t *at)
{
	size_t cap = (fields && fields->n ? fields->n : rec->kind == T_MAP ? rec->n : 1) + 1;
	member *m = xcalloc(cap, sizeof *m);
	size_t n = 0;
	size_t klen = strlen(key);
	m[n++] = (member){.k = (const uint8_t *)key, .kn = klen, .str = val ? val : "", .extra = true};
	if (fields && fields->n) {
		for (size_t i = 0; i < fields->n; i++) {
			const char *f = fields->v[i];
			if (strlen(f) == klen && memcmp(f, key, klen) == 0)
				continue;
			m[n++] = (member){.k = (const uint8_t *)f, .kn = strlen(f), .v = rec ? extract_field(rec, f) : NULL};
		}
	} else if (rec && rec->kind == T_MAP) {
		for (uint32_t i = 0; i < rec->n; i++) {
			const dnode *k = &rec->items[2 * i];
			if (k->n == klen && memcmp(k->s, key, klen) == 0)
				continue;
			m[n++] = (member){.k = k->s, .kn = k->n, .v = &rec->items[2 * i + 1]};
		}
	} else if (rec) {
		m[n++] = (member){.k = (const uint8_t *)"value", .kn = 5, .v = rec};
	}
	qsort(m, n, sizeof *m, cmp_member);
	buf_push(out, '{');
	for (size_t i = 0; i < n; i++) {
		if (i)
			buf_push(out, ',');
		if (pretty)
			jw_newline(out, "", 1);
		json_string(out, m[i].k, m[i].kn);
		buf_append(out, pretty ? ": " : ":", pretty ? 2 : 1);
		if (m[i].extra && !val)
			*at = out->len;
		else if (m[i].str)
			json_string(out, (const uint8_t *)m[i].str, strlen(m[i].str));
		else if (!m[i].v)
			buf_append(out, "null", 4);
		else if (pretty)
			jw_value(out, m[i].v, "", 1);
		else
			dnode_json(m[i].v, out);
	}
	if (pretty && n)
		jw_newline(out, "", 0);
	buf_push(out, '}');
	free(m);
}
