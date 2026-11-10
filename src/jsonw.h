/* JSON output as Go's json.MarshalIndent(v, prefix, "  ") writes it: maps
 * with sorted keys, ": " after keys, one member per line, {} and [] for
 * empty containers, HTML-safe strings. */
#ifndef JSONW_H
#define JSONW_H

#include "mmdb_read.h"

/* A decoded value whose own line is at nesting depth. */
void jw_value(buf_t *out, const dnode *v, const char *prefix, int depth);
/* "\n" + prefix + two spaces per depth. */
void jw_newline(buf_t *out, const char *prefix, int depth);

/* An object built from members rendered one at a time, written with its
 * keys sorted, as Go marshals a map[string]any. */
typedef struct {
	char *key;
	size_t klen;
	char *json; /* the value, rendered at depth 1 */
	size_t jlen;
} jw_member;

typedef struct {
	jw_member *m;
	size_t n, cap;
} jw_object;

void jw_add(jw_object *o, const char *key, size_t klen, const buf_t *value);
/* Writes the object at depth 0 and frees its members. */
void jw_write_object(jw_object *o, buf_t *out);

#endif
