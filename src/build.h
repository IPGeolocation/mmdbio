/* Building an MMDB from CSV or JSON (optionally gzipped): the options, the
 * config file that sets them, and the build itself. */
#ifndef BUILD_H
#define BUILD_H

#include "common.h"

typedef enum { FMT_AUTO, FMT_CSV, FMT_JSON, FMT_JSONL } in_format;
typedef enum { MERGE_REPLACE, MERGE_KEEP, MERGE_TOPLEVEL, MERGE_RECURSE } merge_mode;

/* Field types. VT_NONE: CSV string, or the JSON value's own type. */
typedef enum {
	VT_NONE,
	VT_STRING,
	VT_UINT16,
	VT_UINT32,
	VT_UINT64,
	VT_UINT128,
	VT_INT32,
	VT_DOUBLE,
	VT_FLOAT,
	VT_BOOL,
	VT_ARRAY,
	VT_AUTO, /* inferred per value: integer, double, boolean or string */
} vtype;

typedef struct {
	char *path; /* a column or a dotted JSON path */
	vtype type;
} type_rule;

typedef struct {
	char *column, *key; /* a column (or top-level JSON key) stored as key */
} rename_rule;

typedef struct {
	char *column, *value; /* this column's value that reads as empty */
} empty_rule;

typedef struct {
	char *input, *output;
	in_format format;
	char delimiter;  /* 0: detect */
	int header;      /* -1 detect, 0 none, 1 first row */
	bool lazy_quotes;
	char *network_column, *start_column, *end_column;
	int ip_version;  /* 4 or 6 */
	int record_size; /* 0: the smallest that fits */
	char *database_type;
	char **desc_langs, **desc_texts;
	int ndesc;
	char **languages;
	int nlanguages;
	bool alias_ipv4, include_reserved;
	merge_mode merge;
	bool strict;
	char **include, **exclude;
	int ninclude, nexclude;
	type_rule *types;
	int ntypes;
	bool infer_types, nest_keys, omit_empty;
	char array_sep;
	char *tmp_dir;
	size_t memory;   /* bytes for sorting in memory before spilling to disk */
	bool quiet;
	bool silent;     /* not even the final line (the library) */
	char **columns;  /* names for the CSV's columns, in order */
	int ncolumns;
	bool network_field; /* each record gets "network": the row's CIDR or range */
	bool no_fields;     /* records hold no columns (with network_field, only it) */
	bool fill_gaps;     /* addresses no row covers get every field as "" */
	bool shrink;        /* shrink the result (shrink.h) */
	const char *label;  /* the output's name in messages, when not the path */
	char *lookup_path;  /* records come from this CSV's rows, by id */
	char *lookup_key;   /* its id column (default: its first column) */
	char *lookup_on;    /* the input's id column (default: lookup_key's name) */
	rename_rule *renames;
	int nrenames;
	empty_rule *empties;
	int nempties;
	bool languages_set; /* languages given, possibly as none */
	bool range_first2;  /* the first two columns are the range (mmdbctl's --range-multicol) */
	bool joinkey_col;   /* and the third is left out (mmdbctl's --joinkey-col) */
} build_opts;

/* The defaults; every string is owned (freed by build_opts_free). */
void build_opts_init(build_opts *o);
void build_opts_free(build_opts *o);
/* Applies a config file over o. Relative paths in it are taken from the
 * config file's directory. */
int build_opts_load_config(build_opts *o, const char *path);
/* vtype from its name: string, uint16, uint32, uint64, uint128, int32,
 * double, float, boolean, array, auto. */
bool vtype_parse(const char *name, vtype *out);
const char *vtype_name(vtype t);
void opts_set_str(char **dst, const char *v);
/* Adds or replaces the type rule for path. */
void opts_set_type(build_opts *o, const char *path, vtype t);
/* Adds or replaces the new name of column. */
void opts_set_rename(build_opts *o, const char *column, const char *key);
/* Adds a value of column that reads as empty. */
void opts_add_empty(build_opts *o, const char *column, const char *value);

/* Builds o->output. Returns 0, or -1 with err_set. */
int build_mmdb(build_opts *o);

#endif
