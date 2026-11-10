/* Rows of a CSV or JSON input: each one a network and its record. The
 * format, the CSV delimiter and header, and the IP columns are detected
 * unless the options name them. */
#ifndef ROWS_H
#define ROWS_H

#include "build.h"
#include "mmdb_enc.h"
#include "shared.h"

typedef struct {
	u128 start, end;
	bool is4;
	mval *rec; /* valid until the next rows_next; NULL until rows_build when key is set */
	long line;
	/* a record template's key (tmpl_key): rows with equal keys get equal
	 * records, so a build may reuse the record of an earlier row */
	const uint8_t *key;
	size_t keylen;
} row;

/* ROW_SKIP: a record template's rule leaves the row out (not an error). */
typedef enum { ROW_OK, ROW_BAD, ROW_EOF, ROW_FATAL, ROW_SKIP } row_status;

typedef struct row_source row_source;

row_source *rows_open(const build_opts *o);
/* ROW_BAD: the row is skipped, why says why (with err_set for ROW_FATAL). */
row_status rows_next(row_source *s, row *out, char *why, size_t whylen);
/* Makes out->rec for a row that rows_next left with a key and no record:
 * ROW_OK, ROW_BAD (why), ROW_SKIP or ROW_FATAL. */
row_status rows_build(row_source *s, row *out, char *why, size_t whylen);
/* What was detected, for the log: format, delimiter, header, columns. */
const char *rows_describe(const row_source *s);
uint64_t rows_raw_bytes(const row_source *s);
/* Strings that held invalid UTF-8 (each bad byte became U+FFFD). */
uint64_t rows_utf8_fixed(const row_source *s);
/* Rows whose id the --lookup lacks (their records have every field empty). */
uint64_t rows_lookup_missing(const row_source *s);
/* The record template's notes for the log (ids its tables lack, values left
 * out as invalid), one "note: ..." line each; nothing without a template. */
void rows_report(const row_source *s, buf_t *out);
/* A record template's big $table values become shared values in sh. */
void rows_set_shared(row_source *s, shared *sh);
void rows_close(row_source *s);

/* Shared with the tests: a value's text under a field type. *omit is set
 * when an empty value leaves the key out. */
int conv_text(const uint8_t *s, size_t n, vtype t, char array_sep, bool omit_empty, void *arena_p, mval *out,
	      bool *omit, char *why, size_t whylen);

#endif
