/* Rows of records for read and export: CSV, TSV and JSON Lines.
 *
 * CSV and TSV columns are the --fields, or every top-level key the records
 * have (every leaf path with --flatten, as location.city.name), sorted.
 * A cell is a string as it is, a number in its shortest exact form, true or
 * false, and a map or array as compact JSON. Fields are quoted only when
 * they hold the delimiter, a quote or a line break, as encoding/csv does, so
 * mmdbio import reads both formats back. */
#ifndef TABULAR_H
#define TABULAR_H

#include "flags.h"
#include "mmdb_read.h"

typedef enum { OUT_JSON, OUT_JSONL, OUT_JSON_PRETTY, OUT_CSV, OUT_TSV, OUT_CIDR } out_format;

/* json, jsonl (or ndjson, json-compact), json-pretty, csv, tsv, cidr. */
bool out_format_parse(const char *s, out_format *f);
/* The format a file name suggests: .csv, .tsv, .jsonl/.ndjson, .json. */
bool out_format_from_path(const char *path, out_format *f);

/* The column set, filled from records. */
typedef struct {
	char **names;
	size_t n, cap;
	char **slots; /* a hash set of names */
	size_t nslots;
	bool flatten;
} columns;

void columns_init(columns *c, bool flatten);
void columns_add_record(columns *c, const dnode *v);
void columns_add(columns *c, const char *name, size_t len);
void columns_sort(columns *c);
void columns_free(columns *c);

/* The value at a column of a record: a dotted path through maps. */
const dnode *column_value(const dnode *rec, const char *name);
/* A cell's text, appended. */
void cell_text(const dnode *v, buf_t *out);

/* One CSV (',') or TSV ('\t') row; cells[i] is lens[i] bytes. */
void row_append(buf_t *out, char delim, const char *const *cells, const size_t *lens, size_t n);
void row_write(FILE *f, char delim, const char *const *cells, const size_t *lens, size_t n);

/* A record as one JSON object with key (an "ip" or a "range") added; with
 * fields, only those (missing ones null). A record that is not a map goes
 * under "value". Pretty: indented, as json.MarshalIndent. With val NULL the
 * key's value is left out and *at says where it belongs. */
void json_row(buf_t *out, const dnode *rec, const char *key, const char *val, const strlist *fields, bool pretty,
	      size_t *at);

#endif
