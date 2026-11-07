/* A streaming JSON reader: values are parsed one at a time into small trees
 * (in an arena the caller resets), so a file of any size can be read. */
#ifndef JSTREAM_H
#define JSTREAM_H

#include "csv.h"
#include "mmdb_read.h"

typedef enum { JN_NULL, JN_BOOL, JN_NUM, JN_STR, JN_ARR, JN_OBJ } jn_type;

typedef struct jnode {
	uint8_t type;
	bool b;
	uint32_t n;            /* items of an array or object */
	const char *s;         /* a string (unescaped) or a number's text */
	size_t len;
	struct jnode *items;   /* array items, object values */
	const char **keys;     /* object keys */
	size_t *klens;
	long line;
} jnode;

typedef struct {
	csv_read_fn fn;
	void *ctx;
	uint8_t *buf;
	size_t len, pos;
	bool eof;
	long line;
	arena *a;
	buf_t tmp;
	int depth;
} jstream;

void js_init(jstream *j, csv_read_fn fn, void *ctx, arena *a);
void js_free(jstream *j);
/* The next non-space byte without taking it; -1 at the end, -2 on error. */
int js_peek(jstream *j);
/* Takes the next non-space byte, which must be c. */
int js_expect(jstream *j, char c);
/* Parses one value; errors say "line N: ...". */
int js_value(jstream *j, jnode *out);
/* Parses a string (at its opening quote) into the arena. */
int js_string(jstream *j, const char **s, size_t *n);
/* Finishes an object whose first member (key, value) the caller has read:
 * the rest of the members up to and including the closing brace. */
int js_object_rest(jstream *j, const char *key, size_t klen, const jnode *first, jnode *out);

#endif
