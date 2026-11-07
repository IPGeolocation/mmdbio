/* Rows of a CSV or JSON input: each one a network and its record. The
 * format, the CSV delimiter and header, and the IP columns are detected
 * unless the options name them. */
#ifndef ROWS_H
#define ROWS_H

#include "build.h"
#include "mmdb_enc.h"

typedef struct {
	u128 start, end;
	bool is4;
	mval *rec; /* valid until the next rows_next */
	long line;
} row;

typedef enum { ROW_OK, ROW_BAD, ROW_EOF, ROW_FATAL } row_status;

typedef struct row_source row_source;

row_source *rows_open(const build_opts *o);
/* ROW_BAD: the row is skipped, why says why (with err_set for ROW_FATAL). */
row_status rows_next(row_source *s, row *out, char *why, size_t whylen);
/* What was detected, for the log: format, delimiter, header, columns. */
const char *rows_describe(const row_source *s);
uint64_t rows_raw_bytes(const row_source *s);
/* Strings that held invalid UTF-8 (each bad byte became U+FFFD). */
uint64_t rows_utf8_fixed(const row_source *s);
/* Rows whose id the --lookup lacks (their records have every field empty). */
uint64_t rows_lookup_missing(const row_source *s);
void rows_close(row_source *s);

/* Shared with the tests: a value's text under a field type. *omit is set
 * when an empty value leaves the key out. */
int conv_text(const uint8_t *s, size_t n, vtype t, char array_sep, bool omit_empty, void *arena_p, mval *out,
	      bool *omit, char *why, size_t whylen);

#endif
