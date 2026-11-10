/* A small JSON (RFC 8259) parser for the config file. The whole text must be
 * valid UTF-8; objects keep their keys in file order and may not repeat one. */
#ifndef JSON_H
#define JSON_H

#include "common.h"

typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } jtype;

typedef struct jvalue {
	jtype type;
	int line, col; /* where the value starts, for messages */
	bool b;
	double num;
	char *s;       /* J_STRING, NUL-terminated (a \u0000 in it shows up as has_nul) */
	size_t n;
	bool has_nul;
	char *raw;     /* J_NUMBER: the text as written */
	struct jvalue *items; /* J_ARRAY items, J_OBJECT values */
	char **keys;          /* J_OBJECT keys */
	size_t len;
} jvalue;

/* Parses text; on failure sets "line L, column C: ..." and returns -1. */
int json_parse(const uint8_t *text, size_t n, jvalue *out);
void json_free(jvalue *v);

/* The value of key in object o, or NULL. */
const jvalue *json_get(const jvalue *o, const char *key);
const char *json_type_name(jtype t);

#endif
