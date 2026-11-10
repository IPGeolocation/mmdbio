/* Record templates: the "record" object of an import config builds each
 * record from the columns of the input row and of keyed tables (tables.h),
 * shaped as its "shape" says:
 *
 *   "column"                         a column's value ("column?": "" when the
 *                                    file has no such column)
 *   {"key": node, ...}               a map
 *   [node, ...]                      an array
 *   {"$table": "t", "$on": "id_col", ...}
 *                                    the node below reads the row of table t
 *                                    whose key is the value of id_col
 *   {"$col": ..., "$trim": ..., ...} a value and the steps applied to it
 *
 * See README.md, "Record templates", for every directive. */
#ifndef TMPL_H
#define TMPL_H

#include "build.h"
#include "csv.h"
#include "mmdb_enc.h"
#include "mmdb_read.h"
#include "shared.h"

typedef enum { TM_OK, TM_SKIP, TM_BAD, TM_FATAL } tm_status;

typedef struct tmpl tmpl;

/* Compiles o->record for an input whose columns are names[0..ncols), and
 * opens its tables. NULL with err_set on a mistake in the template. */
tmpl *tmpl_new(const build_opts *o, char **names, size_t *lens, int ncols);
/* The record of the input row in row, allocated in a. TM_SKIP: a skip rule
 * matched; TM_BAD: why says what is wrong with the row; TM_FATAL: err_set. */
tm_status tmpl_record(tmpl *t, const csv_reader *row, arena *a, mval **out, char *why, size_t wl);
/* The values of the input columns the template reads, which decide the
 * record and the skip rules: rows with equal keys get equal records. NULL
 * when the record depends on more (the network, with --network-field,
 * --no-fields or --fill-gaps). Valid until the next call. */
const uint8_t *tmpl_key(tmpl *t, const csv_reader *row, size_t *n);
/* "record template, tables place (12 rows), ..." for the log. */
const char *tmpl_describe(const tmpl *t);
/* Lets $table maps and arrays whose encoding takes SHARE_MIN bytes or more
 * become shared values in sh (shared.h), built once per id. */
void tmpl_set_shared(tmpl *t, shared *sh);
/* Values that held invalid UTF-8 (each bad byte became U+FFFD). */
uint64_t tmpl_utf8_fixed(const tmpl *t);
/* "note: ..." lines: ids the tables lack, values left out as invalid. */
void tmpl_report(const tmpl *t, buf_t *out);
void tmpl_free(tmpl *t);

/* Shared with the tests: Python, Perl and JSON literals (['a', 'b'],
 * {'k': 1}, "x", 12, True, None, ...) to a value. Strings keep their text;
 * numbers, booleans and null are typed as JSON would (null is ""). Returns 0,
 * or -1 with why set. */
int tmpl_parse_literal(const uint8_t *s, size_t n, arena *a, mval *out, char *why, size_t wl);
/* Perl's "[A, B]" list fallback: one [ and one ] taken off, split on commas,
 * each item trimmed and a quote taken off either end; empty items are kept
 * but trailing ones dropped (Perl's split). */
void tmpl_parse_list(const uint8_t *s, size_t n, arena *a, mval *out);
/* Perl's trim, s/^\s+|\s+$//g, with Unicode white space. */
void tmpl_trim(const uint8_t **s, size_t *n);

#endif
