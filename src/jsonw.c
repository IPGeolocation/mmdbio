#include "jsonw.h"

#include <stdlib.h>
#include <string.h>

#include "mmdb_enc.h"

void jw_newline(buf_t *out, const char *prefix, int depth)
{
	buf_push(out, '\n');
	buf_append(out, prefix, strlen(prefix));
	for (int i = 0; i < depth; i++)
		buf_append(out, "  ", 2);
}

void jw_value(buf_t *out, const dnode *v, const char *prefix, int depth)
{
	if (v->kind == T_MAP) {
		if (v->n == 0) {
			buf_append(out, "{}", 2);
			return;
		}
		uint32_t *idx = dnode_sorted_pairs(v);
		buf_push(out, '{');
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ',');
			jw_newline(out, prefix, depth + 1);
			const dnode *k = &v->items[2 * idx[i]];
			json_string(out, k->s, k->n);
			buf_append(out, ": ", 2);
			jw_value(out, &v->items[2 * idx[i] + 1], prefix, depth + 1);
		}
		jw_newline(out, prefix, depth);
		buf_push(out, '}');
		free(idx);
		return;
	}
	if (v->kind == T_ARRAY) {
		if (v->n == 0) {
			buf_append(out, "[]", 2);
			return;
		}
		buf_push(out, '[');
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_push(out, ',');
			jw_newline(out, prefix, depth + 1);
			jw_value(out, &v->items[i], prefix, depth + 1);
		}
		jw_newline(out, prefix, depth);
		buf_push(out, ']');
		return;
	}
	dnode_json(v, out);
}

void jw_add(jw_object *o, const char *key, size_t klen, const buf_t *value)
{
	if (o->n == o->cap) {
		o->cap = o->cap ? o->cap * 2 : 16;
		o->m = xrealloc(o->m, o->cap * sizeof *o->m);
	}
	jw_member *m = &o->m[o->n++];
	m->key = xmalloc(klen + 1);
	if (klen)
		memcpy(m->key, key, klen);
	m->key[klen] = 0;
	m->klen = klen;
	m->json = xmalloc(value->len + 1);
	if (value->len)
		memcpy(m->json, value->p, value->len);
	m->json[value->len] = 0;
	m->jlen = value->len;
}

static int cmp_member(const void *a, const void *b)
{
	const jw_member *x = a, *y = b;
	size_t n = x->klen < y->klen ? x->klen : y->klen;
	int c = memcmp(x->key, y->key, n);
	if (c)
		return c;
	return x->klen < y->klen ? -1 : x->klen > y->klen;
}

void jw_write_object(jw_object *o, buf_t *out)
{
	if (o->n > 1)
		qsort(o->m, o->n, sizeof *o->m, cmp_member);
	/* Go's map keys are unique: a repeated key keeps its last value */
	size_t w = 0;
	for (size_t i = 0; i < o->n; i++) {
		if (w > 0 && o->m[w - 1].klen == o->m[i].klen && memcmp(o->m[w - 1].key, o->m[i].key, o->m[i].klen) == 0) {
			free(o->m[w - 1].key);
			free(o->m[w - 1].json);
			o->m[w - 1] = o->m[i];
			continue;
		}
		o->m[w++] = o->m[i];
	}
	o->n = w;
	if (o->n == 0) {
		buf_append(out, "{}", 2);
	} else {
		buf_push(out, '{');
		for (size_t i = 0; i < o->n; i++) {
			if (i)
				buf_push(out, ',');
			jw_newline(out, "", 1);
			json_string(out, (const uint8_t *)o->m[i].key, o->m[i].klen);
			buf_append(out, ": ", 2);
			buf_append(out, o->m[i].json, o->m[i].jlen);
		}
		buf_append(out, "\n}", 2);
	}
	for (size_t i = 0; i < o->n; i++) {
		free(o->m[i].key);
		free(o->m[i].json);
	}
	free(o->m);
	memset(o, 0, sizeof *o);
}
