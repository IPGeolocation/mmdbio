#include "tmpl.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "rows.h"
#include "shared.h"
#include "tables.h"
#include "utf8.h"

/* ---- the compiled template ---- */

typedef enum { N_COL, N_CONST, N_MAP, N_ARRAY, N_FIRST, N_USE } nkind;
enum { MISS_EMPTY, MISS_OMIT, MISS_SKIP };
enum { INV_OMIT, INV_SKIP, INV_BAD };
enum { P_NONE, P_LITERAL, P_LIST };
enum { CASE_NONE, CASE_UPPER, CASE_LOWER };

/* A row to read columns from: the input's, or a table's. */
typedef struct {
	const uint8_t *p;
	const size_t *ends;
	size_t n;
	bool blank; /* the table has no row for the id: every column reads "" */
} rowv;

typedef struct node node;
struct node {
	nkind kind;
	char *path; /* "location.country.code2", for messages */
	/* "$table" and "$on": the node's content reads that table's row */
	int table;  /* -1: the enclosing row */
	int on_col; /* the id column in the enclosing row; -1: absent, reads "" */
	int missing;
	buf_t last_id; /* the id of the row in snap: neighbours often share one */
	bool has_last, last_found;
	buf_t snap;    /* that row's fields, back to back */
	size_t *snap_ends, snap_n, snap_cap;
	/* N_COL: a column (-1: absent, reads ""); N_CONST: a value */
	int col;
	mval cval;
	bool cstr; /* a string constant: it goes through the steps like a column */
	/* N_MAP (with keys), N_ARRAY, N_FIRST and N_USE (one kid) */
	node *kids;
	char **keys;
	int nkids;
	/* the steps of a value */
	bool trim;
	int casing;
	char **empty_if, **one_of;
	int nempty_if, none_of;
	bool has_chars;
	uint8_t chars[32];
	bool has_len;
	size_t len_min, len_max;
	bool has_num, ceil;
	double num_min, num_max;
	node *strip_eq; /* "$strip_prefix": its "equals" */
	uint8_t strip_sep;
	int parse;
	char *defval;
	vtype type;
	int invalid;
	bool omit_empty;
	uint64_t invalid_n;
	/* a $table map or array: what each id made (eval_shared) */
	ht16 memo;         /* SipHash of the id -> slot, or MEMO_SMALL */
	struct memo_slot *slots;
	size_t nslots, slots_cap;
	uint64_t **ctrs;   /* the counters building its content can raise */
	size_t nctrs;
	bool ctrs_set;
};

/* A shared value an id made, and what building it added to the counters
 * (deltas[delta .. delta + ndelta)), added again on each use. */
struct memo_slot {
	uint32_t sid;
	uint32_t ndelta;
	size_t delta;
};

#define MEMO_SMALL UINT32_MAX     /* the id's value is too short to share */
#define MEMO_MAX ((size_t)1 << 22) /* ids remembered per node at most */

typedef struct {
	node value;
	char **in;
	int nin;
} skip_rule;

struct tmpl {
	const build_opts *o;
	ktable *tables;
	int ntables;
	uint64_t *missing; /* rows whose id a table lacks */
	node root;
	skip_rule *rules;
	int nrules;
	bool skip_empty;
	char **tkeys; /* "types": a key's type wherever the key is */
	vtype *tvals;
	int ntypes;
	const jvalue *defines;
	arena keep; /* constants, for the whole build */
	uint64_t utf8_fixed;
	char describe[1024];
	/* while compiling */
	char **in_names;
	size_t *in_lens;
	int in_ncols;
	int depth;
	/* the input columns read, which make the key (tmpl_key) */
	int *used;
	int nused;
	bool keyed;
	buf_t key;
	/* shared values (eval_shared) */
	shared *sh;
	buf_t enc;
	struct delta {
		uint64_t *ctr;
		uint64_t add;
	} *deltas;
	size_t ndeltas, deltas_cap;
	uint64_t *snap;
	size_t snap_cap;
};

/* What a node takes from the nodes above it. */
typedef struct {
	int table; /* the row its columns come from: -1 the input */
	bool trim, omit_empty;
	int invalid;
} inherit;

static char *dupn(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	if (n)
		memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static char *dupstr(const char *s) { return dupn(s, strlen(s)); }

static int cbad(const tmpl *t, const jvalue *v, const char *path, const char *fmt, ...) PRINTF_FMT(4, 5);
static int cbad(const tmpl *t, const jvalue *v, const char *path, const char *fmt, ...)
{
	char msg[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	err_set("%s:%d: %s: %s", t->o->record_file ? t->o->record_file : "config", v->line, path, msg);
	return -1;
}

static vtype key_type(const tmpl *t, const char *key, size_t n)
{
	if (!key)
		return VT_NONE;
	for (int i = 0; i < t->ntypes; i++)
		if (strlen(t->tkeys[i]) == n && memcmp(t->tkeys[i], key, n) == 0)
			return t->tvals[i];
	return VT_NONE;
}

/* The column called name (a trailing ? makes it optional) of the input, or of
 * table ti: its index, -1 when optional and absent, -2 (err_set) when missing. */
static int column(tmpl *t, const jvalue *v, const char *path, int ti, const char *name)
{
	size_t n = strlen(name);
	bool optional = n > 1 && name[n - 1] == '?';
	if (optional)
		n--;
	int found = -1;
	if (ti < 0) {
		for (int i = 0; i < t->in_ncols; i++)
			if (t->in_lens[i] == n && memcmp(t->in_names[i], name, n) == 0)
				found = i;
	} else {
		found = ktable_column(&t->tables[ti], name, n);
	}
	if (found >= 0 && ti < 0) {
		bool seen = false;
		for (int i = 0; i < t->nused && !seen; i++)
			seen = t->used[i] == found;
		if (!seen) {
			t->used = xrealloc(t->used, (size_t)(t->nused + 1) * sizeof *t->used);
			t->used[t->nused++] = found;
		}
	}
	if (found >= 0 || optional)
		return found;
	if (ti < 0)
		cbad(t, v, path, "the input %s has no column \"%.*s\" (end the name with ? if it may be missing)", t->o->input,
		     (int)n, name);
	else
		cbad(t, v, path, "the table \"%s\" (%s) has no column \"%.*s\" (end the name with ? if it may be missing)",
		     t->o->tables[ti].name, t->tables[ti].path, (int)n, name);
	return -2;
}

/* a JSON constant as a value; keys of its maps typed by "types" */
static int const_value(tmpl *t, const jvalue *v, const char *path, const char *key, mval *out)
{
	switch (v->type) {
	case J_NULL:
		*out = (mval){.type = T_STRING, .s = (const uint8_t *)"", .n = 0};
		return 0;
	case J_BOOL:
		*out = (mval){.type = T_BOOL, .u = v->b};
		return 0;
	case J_STRING:
		*out = (mval){.type = T_STRING, .s = (const uint8_t *)v->s, .n = (uint32_t)v->n};
		return 0;
	case J_NUMBER: {
		vtype ty = key_type(t, key, key ? strlen(key) : 0);
		char why[200];
		bool omit;
		if (conv_text((const uint8_t *)v->raw, strlen(v->raw), ty == VT_NONE || ty == VT_STRING ? VT_AUTO : ty, ',',
			      false, &t->keep, out, &omit, why, sizeof why) < 0)
			return cbad(t, v, path, "%s", why);
		if (ty == VT_STRING)
			*out = (mval){.type = T_STRING, .s = (const uint8_t *)v->raw, .n = (uint32_t)strlen(v->raw)};
		return 0;
	}
	case J_ARRAY: {
		mval *items = arena_alloc(&t->keep, sizeof *items * (v->len + 1));
		for (size_t i = 0; i < v->len; i++)
			if (const_value(t, &v->items[i], path, key, &items[i]) < 0)
				return -1;
		*out = (mval){.type = T_ARRAY, .n = (uint32_t)v->len, .items = items};
		return 0;
	}
	case J_OBJECT: {
		mval *items = arena_alloc(&t->keep, sizeof *items * (2 * v->len + 2));
		for (size_t i = 0; i < v->len; i++) {
			items[2 * i] = (mval){.type = T_STRING, .s = (const uint8_t *)v->keys[i], .n = (uint32_t)strlen(v->keys[i])};
			if (const_value(t, &v->items[i], path, v->keys[i], &items[2 * i + 1]) < 0)
				return -1;
		}
		*out = (mval){.type = T_MAP, .n = (uint32_t)v->len, .items = items};
		mval_sort_map(out);
		return 0;
	}
	}
	return -1;
}

static int strlist(const tmpl *t, const jvalue *v, const char *path, char ***out, int *n)
{
	if (v->type == J_STRING) {
		*out = xcalloc(2, sizeof **out);
		(*out)[0] = dupn(v->s, v->n);
		*n = 1;
		return 0;
	}
	if (v->type != J_ARRAY)
		return cbad(t, v, path, "must be a string or an array of strings");
	*out = xcalloc(v->len + 1, sizeof **out);
	for (size_t i = 0; i < v->len; i++) {
		if (v->items[i].type != J_STRING)
			return cbad(t, &v->items[i], path, "must be an array of strings");
		(*out)[i] = dupn(v->items[i].s, v->items[i].n);
		*n = (int)i + 1;
	}
	return 0;
}

static int number_pair(const tmpl *t, const jvalue *v, const char *path, double *lo, double *hi)
{
	if (v->type != J_ARRAY || v->len != 2 || v->items[0].type != J_NUMBER || v->items[1].type != J_NUMBER ||
	    v->items[0].num > v->items[1].num)
		return cbad(t, v, path, "must be [min, max]");
	*lo = v->items[0].num;
	*hi = v->items[1].num;
	return 0;
}

static int chars_set(const tmpl *t, const jvalue *v, const char *path, uint8_t set[32])
{
	if (v->type != J_STRING || v->n == 0)
		return cbad(t, v, path, "must name the allowed characters, e.g. \"A-Z0-9\"");
	memset(set, 0, 32);
	const uint8_t *s = (const uint8_t *)v->s;
	for (size_t i = 0; i < v->n; i++) {
		uint8_t lo = s[i], hi = s[i];
		if (i + 2 < v->n && s[i + 1] == '-') {
			hi = s[i + 2];
			i += 2;
		}
		if (lo > hi)
			return cbad(t, v, path, "has the range %c-%c backwards", lo, hi);
		for (unsigned c = lo; c <= hi; c++)
			set[c >> 3] |= (uint8_t)(1u << (c & 7));
	}
	return 0;
}

static int compile(tmpl *t, const jvalue *v, node *n, const char *path, const char *key, inherit in);

static bool directive(const char *k) { return k[0] == '$'; }

static const char *const DIRECTIVES[] = {"$table", "$on", "$missing", "$col", "$const", "$use", "$first", "$array",
					 "$trim", "$case", "$empty_if", "$one_of", "$chars", "$length", "$number",
					 "$ceil", "$strip_prefix", "$parse", "$default", "$type", "$invalid", "$empty",
					 NULL};

static int compile_object(tmpl *t, const jvalue *v, node *n, const char *path, const char *key, inherit in)
{
	for (size_t i = 0; i < v->len; i++) {
		if (!directive(v->keys[i]))
			continue;
		bool ok = false;
		for (const char *const *d = DIRECTIVES; *d && !ok; d++)
			ok = strcmp(*d, v->keys[i]) == 0;
		if (!ok)
			return cbad(t, &v->items[i], n->path, "unknown directive \"%s\"", v->keys[i]);
	}
	const jvalue *x;
	/* what the node and the ones below it inherit */
	if ((x = json_get(v, "$table"))) {
		if (x->type != J_STRING)
			return cbad(t, x, n->path, "$table must name a table");
		n->table = -1;
		for (int i = 0; i < t->ntables; i++)
			if (strcmp(t->o->tables[i].name, x->s) == 0)
				n->table = i;
		if (n->table < 0)
			return cbad(t, x, n->path, "no table \"%s\" in record.tables", x->s);
		const jvalue *on = json_get(v, "$on");
		if (!on || on->type != J_STRING)
			return cbad(t, x, n->path, "$table needs \"$on\": the column holding the id");
		n->on_col = column(t, on, n->path, in.table, on->s);
		if (n->on_col == -2)
			return -1;
		n->missing = MISS_EMPTY;
		if ((x = json_get(v, "$missing"))) {
			if (x->type != J_STRING ||
			    (strcmp(x->s, "empty") != 0 && strcmp(x->s, "omit") != 0 && strcmp(x->s, "skip") != 0))
				return cbad(t, x, n->path, "$missing must be empty, omit or skip");
			n->missing = x->s[0] == 'e' ? MISS_EMPTY : x->s[0] == 'o' ? MISS_OMIT : MISS_SKIP;
		}
		in.table = n->table;
		if (t->o->tables[n->table].trim >= 0)
			in.trim = t->o->tables[n->table].trim;
	} else if ((x = json_get(v, "$on")) || (x = json_get(v, "$missing"))) {
		return cbad(t, x, n->path, "%s goes with $table", json_get(v, "$on") ? "$on" : "$missing");
	}
	if ((x = json_get(v, "$trim"))) {
		if (x->type != J_BOOL)
			return cbad(t, x, n->path, "$trim must be true or false");
		in.trim = x->b;
	}
	if ((x = json_get(v, "$empty"))) {
		if (x->type != J_STRING || (strcmp(x->s, "keep") != 0 && strcmp(x->s, "omit") != 0))
			return cbad(t, x, n->path, "$empty must be keep or omit");
		in.omit_empty = strcmp(x->s, "omit") == 0;
	}
	if ((x = json_get(v, "$invalid"))) {
		if (x->type != J_STRING ||
		    (strcmp(x->s, "omit") != 0 && strcmp(x->s, "skip") != 0 && strcmp(x->s, "bad") != 0))
			return cbad(t, x, n->path, "$invalid must be omit, skip or bad");
		in.invalid = x->s[0] == 'o' ? INV_OMIT : x->s[0] == 's' ? INV_SKIP : INV_BAD;
	}
	n->trim = in.trim;
	n->omit_empty = in.omit_empty;
	n->invalid = in.invalid;
	/* the content: one of these */
	int members = 0;
	for (size_t i = 0; i < v->len; i++)
		members += !directive(v->keys[i]) && v->keys[i][0] != '_';
	const char *kinds[] = {"$col", "$const", "$use", "$first", "$array"};
	int nk = members > 0;
	for (int i = 0; i < 5; i++)
		nk += json_get(v, kinds[i]) != NULL;
	if (nk != 1)
		return cbad(t, v, n->path, nk ? "holds more than one of $col, $const, $use, $first, $array and map keys"
					   : "needs a value: $col, $const, $use, $first, $array or map keys");
	bool leaf = false;
	if ((x = json_get(v, "$col"))) {
		if (x->type != J_STRING || x->n == 0)
			return cbad(t, x, n->path, "$col must name a column");
		n->kind = N_COL;
		if ((n->col = column(t, x, n->path, in.table, x->s)) == -2)
			return -1;
		leaf = true;
	} else if ((x = json_get(v, "$const"))) {
		n->kind = N_CONST;
		if (x->type == J_STRING) {
			n->cstr = true;
			n->cval = (mval){.type = T_STRING, .s = (const uint8_t *)x->s, .n = (uint32_t)x->n};
			leaf = true;
		} else if (const_value(t, x, path, key, &n->cval) < 0) {
			return -1;
		}
	} else if ((x = json_get(v, "$use"))) {
		const jvalue *body = x->type == J_STRING && t->defines ? json_get(t->defines, x->s) : NULL;
		if (!body)
			return cbad(t, x, n->path, "$use names no entry of record.define");
		if (++t->depth > 32)
			return cbad(t, x, n->path, "record.define entries use each other too deeply (a loop?)");
		n->kind = N_USE;
		n->kids = xcalloc(1, sizeof *n->kids);
		n->nkids = 1;
		int rc = compile(t, body, &n->kids[0], path, key, in);
		t->depth--;
		if (rc < 0)
			return -1;
	} else if ((x = json_get(v, "$first")) || (x = json_get(v, "$array"))) {
		bool first = json_get(v, "$first") != NULL;
		if (x->type != J_ARRAY || (first && x->len == 0))
			return cbad(t, x, n->path, "%s must be an array of values", first ? "$first" : "$array");
		n->kind = first ? N_FIRST : N_ARRAY;
		n->kids = xcalloc(x->len + 1, sizeof *n->kids);
		n->nkids = (int)x->len;
		for (size_t i = 0; i < x->len; i++) {
			char p[600];
			snprintf(p, sizeof p, "%s[%zu]", path, i);
			if (compile(t, &x->items[i], &n->kids[i], p, key, in) < 0)
				return -1;
		}
	} else {
		n->kind = N_MAP;
		n->kids = xcalloc((size_t)members + 1, sizeof *n->kids);
		n->keys = xcalloc((size_t)members + 1, sizeof *n->keys);
		for (size_t i = 0; i < v->len; i++) {
			if (directive(v->keys[i]) || v->keys[i][0] == '_')
				continue;
			char p[600];
			snprintf(p, sizeof p, "%s%s%s", path, *path ? "." : "", v->keys[i]);
			n->keys[n->nkids] = dupstr(v->keys[i]);
			if (compile(t, &v->items[i], &n->kids[n->nkids++], p, v->keys[i], in) < 0)
				return -1;
		}
	}
	/* the steps of a value */
	n->type = key_type(t, key, key ? strlen(key) : 0);
	static const char *const STEPS[] = {"$case", "$empty_if", "$one_of", "$chars", "$length", "$number",
					    "$ceil", "$strip_prefix", "$parse", "$default", NULL};
	if (!leaf) {
		for (const char *const *s = STEPS; *s; s++)
			if ((x = json_get(v, *s)))
				return cbad(t, x, n->path, "%s applies to $col and string $const values", *s);
		if ((x = json_get(v, "$type")))
			return cbad(t, x, n->path, "$type applies to $col and string $const values");
	}
	if ((x = json_get(v, "$type")) && (x->type != J_STRING || !vtype_parse(x->s, &n->type)))
		return cbad(t, x, n->path, "$type must be string, uint16, uint32, uint64, uint128, int32, double, float, boolean, array or auto");
	if (!leaf)
		return 0;
	if ((x = json_get(v, "$case"))) {
		if (x->type != J_STRING || (strcmp(x->s, "upper") != 0 && strcmp(x->s, "lower") != 0))
			return cbad(t, x, n->path, "$case must be upper or lower");
		n->casing = x->s[0] == 'u' ? CASE_UPPER : CASE_LOWER;
	}
	if ((x = json_get(v, "$empty_if")) && strlist(t, x, path, &n->empty_if, &n->nempty_if) < 0)
		return -1;
	if ((x = json_get(v, "$one_of")) && strlist(t, x, path, &n->one_of, &n->none_of) < 0)
		return -1;
	if ((x = json_get(v, "$chars"))) {
		if (chars_set(t, x, path, n->chars) < 0)
			return -1;
		n->has_chars = true;
	}
	if ((x = json_get(v, "$length"))) {
		double lo, hi;
		if (number_pair(t, x, path, &lo, &hi) < 0)
			return -1;
		n->has_len = true;
		n->len_min = lo < 0 ? 0 : (size_t)lo;
		n->len_max = (size_t)hi;
	}
	if ((x = json_get(v, "$number"))) {
		if (number_pair(t, x, path, &n->num_min, &n->num_max) < 0)
			return -1;
		n->has_num = true;
	}
	if ((x = json_get(v, "$ceil"))) {
		if (x->type != J_BOOL)
			return cbad(t, x, n->path, "$ceil must be true or false");
		n->ceil = x->b;
	}
	if ((x = json_get(v, "$strip_prefix"))) {
		const jvalue *eq = x->type == J_OBJECT ? json_get(x, "equals") : NULL;
		const jvalue *sep = x->type == J_OBJECT ? json_get(x, "sep") : NULL;
		if (!eq || (sep && (sep->type != J_STRING || sep->n != 1)))
			return cbad(t, x, n->path, "$strip_prefix must be {\"equals\": value, \"sep\": \"-\"}");
		n->strip_sep = sep ? (uint8_t)sep->s[0] : '-';
		n->strip_eq = xcalloc(1, sizeof *n->strip_eq);
		char p[600];
		snprintf(p, sizeof p, "%s.$strip_prefix", path);
		inherit sub = in;
		sub.omit_empty = false;
		sub.invalid = INV_OMIT;
		if (compile(t, eq, n->strip_eq, p, NULL, sub) < 0)
			return -1;
		n->strip_eq->type = VT_STRING;
	}
	if ((x = json_get(v, "$parse"))) {
		if (x->type != J_STRING ||
		    (strcmp(x->s, "literal") != 0 && strcmp(x->s, "json") != 0 && strcmp(x->s, "list") != 0))
			return cbad(t, x, n->path, "$parse must be literal, json or list");
		n->parse = strcmp(x->s, "list") == 0 ? P_LIST : P_LITERAL;
	}
	if ((x = json_get(v, "$default"))) {
		if (x->type != J_STRING)
			return cbad(t, x, n->path, "$default must be a string");
		n->defval = dupn(x->s, x->n);
	}
	return 0;
}

static int compile(tmpl *t, const jvalue *v, node *n, const char *path, const char *key, inherit in)
{
	n->table = -1;
	n->trim = in.trim;
	n->omit_empty = in.omit_empty;
	n->invalid = in.invalid;
	n->path = dupstr(*path ? path : "(record)");
	switch (v->type) {
	case J_STRING:
		if (v->n == 0)
			return cbad(t, v, n->path, "an empty column name (use {\"$const\": \"\"} for an empty string)");
		n->kind = N_COL;
		n->type = key_type(t, key, key ? strlen(key) : 0);
		return (n->col = column(t, v, n->path, in.table, v->s)) == -2 ? -1 : 0;
	case J_NUMBER:
	case J_BOOL:
		n->kind = N_CONST;
		return const_value(t, v, n->path, key, &n->cval);
	case J_ARRAY:
		n->kind = N_ARRAY;
		n->kids = xcalloc(v->len + 1, sizeof *n->kids);
		n->nkids = (int)v->len;
		for (size_t i = 0; i < v->len; i++) {
			char p[600];
			snprintf(p, sizeof p, "%s[%zu]", path, i);
			if (compile(t, &v->items[i], &n->kids[i], p, key, in) < 0)
				return -1;
		}
		return 0;
	case J_OBJECT:
		return compile_object(t, v, n, path, key, in);
	default:
		return cbad(t, v, n->path, "must be a column name, a map, an array or a {\"$...\"} value, not null");
	}
}

static void node_free(node *n)
{
	for (int i = 0; i < n->nkids; i++) {
		node_free(&n->kids[i]);
		if (n->keys)
			free(n->keys[i]);
	}
	free(n->kids);
	free(n->keys);
	free(n->path);
	buf_free(&n->last_id);
	buf_free(&n->snap);
	free(n->snap_ends);
	for (int i = 0; i < n->nempty_if; i++)
		free(n->empty_if[i]);
	free(n->empty_if);
	for (int i = 0; i < n->none_of; i++)
		free(n->one_of[i]);
	free(n->one_of);
	if (n->strip_eq) {
		node_free(n->strip_eq);
		free(n->strip_eq);
	}
	free(n->defval);
	ht16_free(&n->memo);
	free(n->slots);
	free(n->ctrs);
}

/* ---- building a record ---- */

typedef struct {
	tmpl *t;
	arena *a;
	char *why;
	size_t wl;
} ectx;

enum { E_OK = 0, E_SKIP = 1, E_BAD = 2, E_FATAL = -1 };

static void field(const rowv *r, int col, const uint8_t **p, size_t *n)
{
	if (r->blank || col < 0 || (size_t)col >= r->n || !r->p) {
		*p = (const uint8_t *)"";
		*n = 0;
		return;
	}
	size_t start = col ? r->ends[col - 1] : 0;
	*p = r->p + start;
	*n = r->ends[col] - start;
}

/* The row of n's table for id: from the last fetch when the id repeats. */
static int fetch(ectx *e, node *n, const uint8_t *id, size_t idn, rowv *out)
{
	if (!(n->has_last && n->last_id.len == idn && (idn == 0 || memcmp(n->last_id.p, id, idn) == 0))) {
		n->has_last = false;
		ktable *kt = &e->t->tables[n->table];
		/* an empty id finds no row, as Perl's lookups (utils.pm) treat it */
		int found = idn ? ktable_fetch(kt, id, idn) : 0;
		if (found < 0)
			return -1;
		n->last_id.len = 0;
		buf_append(&n->last_id, id, idn);
		n->last_found = found;
		if (found) {
			const csv_reader *r = &kt->rd;
			n->snap.len = 0;
			size_t len = r->nfields ? r->ends[r->nfields - 1] : 0;
			buf_grow(&n->snap, len + 1);
			if (len)
				buf_append(&n->snap, r->rec.p, len);
			if (r->nfields > n->snap_cap) {
				n->snap_cap = r->nfields;
				n->snap_ends = xrealloc(n->snap_ends, n->snap_cap * sizeof *n->snap_ends);
			}
			if (r->nfields)
				memcpy(n->snap_ends, r->ends, r->nfields * sizeof *n->snap_ends);
			n->snap_n = r->nfields;
		}
		n->has_last = true;
	}
	*out = (rowv){.p = n->snap.p, .ends = n->snap_ends, .n = n->snap_n, .blank = !n->last_found};
	return n->last_found;
}

static bool is_ws(uint32_t r)
{
	return (r >= 0x09 && r <= 0x0D) || r == 0x20 || r == 0x85 || r == 0xA0 || r == 0x1680 || (r >= 0x2000 && r <= 0x200A) ||
	       r == 0x2028 || r == 0x2029 || r == 0x202F || r == 0x205F || r == 0x3000;
}

void tmpl_trim(const uint8_t **s, size_t *n)
{
	while (*n) {
		uint32_t r;
		int w = go_decode_rune(*s, *n, &r);
		if (!is_ws(r))
			break;
		*s += w;
		*n -= (size_t)w;
	}
	while (*n) {
		/* the start of the last rune */
		size_t i = *n - 1;
		while (i > 0 && ((*s)[i] & 0xC0) == 0x80 && *n - i < 4)
			i--;
		uint32_t r;
		int w = go_decode_rune(*s + i, *n - i, &r);
		if ((size_t)w != *n - i || !is_ws(r))
			break;
		*n = i;
	}
}

static size_t runes(const uint8_t *s, size_t n)
{
	size_t k = 0;
	for (size_t i = 0; i < n; i++)
		k += (s[i] & 0xC0) != 0x80;
	return k;
}

/* Perl's looks_like_number, finite: 1 with *x, else 0. */
static int number_text(const uint8_t *s, size_t n, double *x)
{
	size_t i = 0;
	if (i < n && (s[i] == '+' || s[i] == '-'))
		i++;
	size_t d0 = i;
	while (i < n && isdigit(s[i]))
		i++;
	size_t whole = i - d0, frac = 0;
	if (i < n && s[i] == '.') {
		i++;
		size_t f0 = i;
		while (i < n && isdigit(s[i]))
			i++;
		frac = i - f0;
	}
	if (whole + frac == 0)
		return 0;
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		size_t e0 = ++i;
		if (i < n && (s[i] == '+' || s[i] == '-'))
			i++;
		size_t g0 = i;
		while (i < n && isdigit(s[i]))
			i++;
		if (i == g0)
			return 0;
		(void)e0;
	}
	if (i != n)
		return 0;
	char tmp[400];
	if (n >= sizeof tmp)
		return 0;
	memcpy(tmp, s, n);
	tmp[n] = 0;
	*x = strtod(tmp, NULL);
	return isfinite(*x) ? 1 : 0;
}

static bool listed(char **list, int n, const uint8_t *v, size_t vn)
{
	for (int i = 0; i < n; i++)
		if (strlen(list[i]) == vn && (vn == 0 || memcmp(list[i], v, vn) == 0))
			return true;
	return false;
}

static int eval(ectx *e, node *n, const rowv *ctx, mval *out, bool *omit);

/* a value that breaks a rule of its node: left out, the row skipped, or bad
 * (why says what is wrong) */
static int invalid_msg(ectx *e, node *n, bool *omit, const char *msg)
{
	if (n->invalid == INV_OMIT) {
		n->invalid_n++;
		*omit = true;
		return E_OK;
	}
	if (n->invalid == INV_SKIP)
		return E_SKIP;
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s: %s", n->path, msg);
	snprintf(e->why, e->wl, "%s", tmp);
	return E_BAD;
}

static int invalid(ectx *e, node *n, bool *omit, const uint8_t *v, size_t vn, const char *what)
{
	char msg[400];
	snprintf(msg, sizeof msg, "\"%.*s\" %s", (int)(vn > 60 ? 60 : vn), (const char *)v, what);
	return invalid_msg(e, n, omit, msg);
}

static bool empty_value(const mval *v)
{
	return (v->type == T_STRING || v->type == T_MAP || v->type == T_ARRAY) && v->n == 0;
}

/* Parsed scalars typed by the key they sit under. */
static int type_parsed(ectx *e, node *n, mval *v, const char *key, size_t kl)
{
	if (v->type == T_MAP) {
		for (uint32_t i = 0; i < v->n; i++)
			if (type_parsed(e, n, &v->items[2 * i + 1], (const char *)v->items[2 * i].s, v->items[2 * i].n) < 0)
				return -1;
		return 0;
	}
	if (v->type == T_ARRAY) {
		for (uint32_t i = 0; i < v->n; i++)
			if (type_parsed(e, n, &v->items[i], key, kl) < 0)
				return -1;
		return 0;
	}
	vtype ty = key == NULL ? n->type : key_type(e->t, key, kl);
	if (ty == VT_NONE || ty == VT_ARRAY || ty == VT_AUTO)
		return 0;
	/* the scalar's text under the type */
	char num[64];
	const uint8_t *s = v->s;
	size_t sn = v->n;
	if (v->type == T_BOOL) {
		s = (const uint8_t *)(v->u ? "1" : "0");
		sn = 1;
	} else if (v->type != T_STRING) {
		/* numbers keep their text in s, n (tmpl_parse_literal) */
		if (!v->s || !v->n) {
			snprintf(num, sizeof num, "%.17g", v->f);
			s = (const uint8_t *)num;
			sn = strlen(num);
		}
	}
	bool om;
	mval r;
	if (conv_text(s, sn, ty, ',', false, e->a, &r, &om, e->why, e->wl) < 0)
		return -1;
	if (ty == VT_STRING || om) {
		uint8_t *c = arena_alloc(e->a, sn + 1);
		memcpy(c, s, sn);
		r = (mval){.type = T_STRING, .s = c, .n = (uint32_t)sn};
	}
	*v = r;
	return 0;
}

static int eval_value(ectx *e, node *n, const rowv *ctx, mval *out, bool *omit)
{
	if (n->kind == N_CONST && !n->cstr) {
		*out = n->cval;
		return E_OK;
	}
	const uint8_t *v;
	size_t vn;
	if (n->kind == N_CONST) {
		v = n->cval.s;
		vn = n->cval.n;
	} else {
		field(ctx, n->col, &v, &vn);
	}
	/* a copy of its own: the steps change it, and the table row it came from
	 * may be read over before the record is done */
	if (!utf8_valid(v, vn)) {
		buf_t b = {0};
		for (size_t i = 0; i < vn;) {
			uint32_t r;
			int w = go_decode_rune(v + i, vn - i, &r);
			if (r == UTF8_RUNE_ERROR && w == 1)
				buf_append(&b, "\xEF\xBF\xBD", 3);
			else
				buf_append(&b, v + i, (size_t)w);
			i += (size_t)w;
		}
		uint8_t *c = arena_alloc(e->a, b.len + 1);
		if (b.len)
			memcpy(c, b.p, b.len);
		v = c;
		vn = b.len;
		buf_free(&b);
		e->t->utf8_fixed++;
	} else {
		uint8_t *c = arena_alloc(e->a, vn + 1);
		if (vn)
			memcpy(c, v, vn);
		v = c;
	}
	if (n->trim)
		tmpl_trim(&v, &vn);
	if (n->casing) {
		uint8_t *c = (uint8_t *)v;
		for (size_t i = 0; i < vn; i++)
			c[i] = (uint8_t)(n->casing == CASE_UPPER ? toupper(c[i]) : tolower(c[i]));
	}
	if (vn && n->nempty_if && listed(n->empty_if, n->nempty_if, v, vn))
		vn = 0;
	if (n->strip_eq && vn) {
		const uint8_t *sep = memchr(v, n->strip_sep, vn);
		if (sep && sep > v && sep < v + vn - 1) {
			mval eq;
			bool eom;
			int rc = eval(e, n->strip_eq, ctx, &eq, &eom);
			if (rc != E_OK)
				return rc;
			size_t pl = (size_t)(sep - v);
			bool same = !eom && eq.type == T_STRING && eq.n == pl && memcmp(eq.s, v, pl) == 0;
			if (same) {
				vn -= pl + 1;
				v = sep + 1;
			} else {
				vn = 0;
			}
		}
	}
	if (vn) {
		if (n->none_of && !listed(n->one_of, n->none_of, v, vn))
			return invalid(e, n, omit, v, vn, "is not one of the allowed values");
		if (n->has_chars)
			for (size_t i = 0; i < vn; i++)
				if (!(n->chars[v[i] >> 3] & (1u << (v[i] & 7))))
					return invalid(e, n, omit, v, vn, "has a character outside $chars");
		if (n->has_len) {
			size_t k = runes(v, vn);
			if (k < n->len_min || k > n->len_max)
				return invalid(e, n, omit, v, vn, "has a length outside $length");
		}
		double x = 0;
		if (n->has_num || n->ceil) {
			if (!number_text(v, vn, &x))
				return invalid(e, n, omit, v, vn, "is not a number");
			if (n->has_num && (x < n->num_min || x > n->num_max))
				return invalid(e, n, omit, v, vn, "is outside the $number range");
		}
		if (n->ceil) {
			char *c = arena_alloc(e->a, 64);
			snprintf(c, 64, "%.0f", ceil(x));
			if (strcmp(c, "-0") == 0)
				strcpy(c, "0");
			v = (const uint8_t *)c;
			vn = strlen(c);
		}
	}
	if (vn == 0 && n->defval) {
		v = (const uint8_t *)n->defval;
		vn = strlen(n->defval);
	}
	*omit = false;
	if (n->parse == P_LIST) {
		tmpl_parse_list(v, vn, e->a, out);
		if (type_parsed(e, n, out, NULL, 0) < 0)
			return invalid_msg(e, n, omit, e->why);
	} else if (n->parse == P_LITERAL) {
		if (vn == 0) {
			*out = (mval){.type = T_ARRAY};
		} else {
			char why[300];
			if (tmpl_parse_literal(v, vn, e->a, out, why, sizeof why) < 0) {
				char msg[400];
				snprintf(msg, sizeof msg, "cannot parse it as a literal: %s", why);
				return invalid_msg(e, n, omit, msg);
			}
			if (type_parsed(e, n, out, NULL, 0) < 0)
				return invalid_msg(e, n, omit, e->why);
		}
	} else {
		char why[300];
		if (conv_text(v, vn, n->type, ',', n->omit_empty, e->a, out, omit, why, sizeof why) < 0)
			return invalid_msg(e, n, omit, why);
	}
	if (!*omit && n->omit_empty && empty_value(out))
		*omit = true;
	return E_OK;
}

static int eval_body(ectx *e, node *n, const rowv *ctx, mval *out, bool *omit);
static int eval_shared(ectx *e, node *n, const rowv *ctx, const uint8_t *id, size_t idn, mval *out, bool *omit);

static int eval(ectx *e, node *n, const rowv *ctx, mval *out, bool *omit)
{
	*omit = false;
	rowv sub;
	if (n->table >= 0) {
		const uint8_t *id;
		size_t idn;
		field(ctx, n->on_col, &id, &idn);
		int found = fetch(e, n, id, idn, &sub);
		if (found < 0)
			return E_FATAL;
		if (!found) {
			e->t->missing[n->table]++;
			if (n->missing == MISS_OMIT) {
				*omit = true;
				return E_OK;
			}
			if (n->missing == MISS_SKIP)
				return E_SKIP;
		}
		ctx = &sub;
		if (found && e->t->sh && (n->kind == N_MAP || n->kind == N_ARRAY) && n != &e->t->root)
			return eval_shared(e, n, ctx, id, idn, out, omit);
	}
	return eval_body(e, n, ctx, out, omit);
}

static void add_ctr(node *n, uint64_t *c)
{
	n->ctrs = xrealloc(n->ctrs, (n->nctrs + 1) * sizeof *n->ctrs);
	n->ctrs[n->nctrs++] = c;
}

static void node_ctrs(node *top, node *n)
{
	add_ctr(top, &n->invalid_n);
	for (int i = 0; i < n->nkids; i++)
		node_ctrs(top, &n->kids[i]);
	if (n->strip_eq)
		node_ctrs(top, n->strip_eq);
}

/* A $table map or array, its table's row in ctx: built once per id. When its
 * encoding takes SHARE_MIN bytes or more it becomes a shared value, which
 * the id's later rows get without building it again (an ASN's routes are
 * then encoded once, not once per record that has the ASN). */
static int eval_shared(ectx *e, node *n, const rowv *ctx, const uint8_t *id, size_t idn, mval *out, bool *omit)
{
	tmpl *t = e->t;
	uint8_t h[16];
	siphash128(id, idn, h);
	uint32_t slot;
	bool known = ht16_get(&n->memo, h, &slot);
	if (known && slot != MEMO_SMALL) {
		const struct memo_slot *m = &n->slots[slot];
		for (uint32_t i = 0; i < m->ndelta; i++)
			*t->deltas[m->delta + i].ctr += t->deltas[m->delta + i].add;
		*omit = false;
		*out = (mval){.type = T_SHARED, .u = m->sid};
		return E_OK;
	}
	if (known)
		return eval_body(e, n, ctx, out, omit);
	if (!n->ctrs_set) {
		add_ctr(n, &t->utf8_fixed);
		for (int i = 0; i < t->ntables; i++)
			add_ctr(n, &t->missing[i]);
		node_ctrs(n, n);
		n->ctrs_set = true;
	}
	if (n->nctrs > t->snap_cap) {
		t->snap_cap = n->nctrs;
		t->snap = xrealloc(t->snap, t->snap_cap * sizeof *t->snap);
	}
	for (size_t i = 0; i < n->nctrs; i++)
		t->snap[i] = *n->ctrs[i];
	int rc = eval_body(e, n, ctx, out, omit);
	if (rc != E_OK || *omit)
		return rc;
	t->enc.len = 0;
	if (mval_canonical(&t->enc, out) < 0)
		return E_OK; /* the record's own encoding fails the same way */
	uint32_t old;
	if (t->enc.len < SHARE_MIN) {
		if (n->memo.len < MEMO_MAX)
			ht16_get_or_put(&n->memo, h, MEMO_SMALL, &old);
		return E_OK;
	}
	uint32_t sid;
	if (shared_add(t->sh, t->enc.p, t->enc.len, &sid) < 0)
		return E_FATAL;
	*out = (mval){.type = T_SHARED, .u = sid};
	if (n->memo.len >= MEMO_MAX)
		return E_OK;
	size_t delta = t->ndeltas;
	for (size_t i = 0; i < n->nctrs; i++) {
		if (*n->ctrs[i] == t->snap[i])
			continue;
		if (t->ndeltas == t->deltas_cap) {
			t->deltas_cap = t->deltas_cap ? 2 * t->deltas_cap : 64;
			t->deltas = xrealloc(t->deltas, t->deltas_cap * sizeof *t->deltas);
		}
		t->deltas[t->ndeltas++] = (struct delta){n->ctrs[i], *n->ctrs[i] - t->snap[i]};
	}
	if (n->nslots == n->slots_cap) {
		n->slots_cap = n->slots_cap ? 2 * n->slots_cap : 64;
		n->slots = xrealloc(n->slots, n->slots_cap * sizeof *n->slots);
	}
	n->slots[n->nslots] = (struct memo_slot){sid, (uint32_t)(t->ndeltas - delta), delta};
	ht16_get_or_put(&n->memo, h, (uint32_t)n->nslots++, &old);
	return E_OK;
}

static int eval_body(ectx *e, node *n, const rowv *ctx, mval *out, bool *omit)
{
	switch (n->kind) {
	case N_COL:
	case N_CONST:
		return eval_value(e, n, ctx, out, omit);
	case N_USE:
		return eval(e, &n->kids[0], ctx, out, omit);
	case N_FIRST: {
		bool have = false;
		for (int i = 0; i < n->nkids; i++) {
			mval v;
			bool om;
			int rc = eval(e, &n->kids[i], ctx, &v, &om);
			if (rc != E_OK)
				return rc;
			if (om)
				continue;
			*out = v;
			have = true;
			if (!empty_value(&v))
				break;
		}
		*omit = !have || (n->omit_empty && empty_value(out));
		return E_OK;
	}
	case N_ARRAY:
	case N_MAP: {
		bool map = n->kind == N_MAP;
		mval *items = arena_alloc(e->a, sizeof *items * (size_t)(2 * n->nkids + 2));
		uint32_t k = 0;
		for (int i = 0; i < n->nkids; i++) {
			mval v;
			bool om;
			int rc = eval(e, &n->kids[i], ctx, &v, &om);
			if (rc != E_OK)
				return rc;
			if (om)
				continue;
			if (map) {
				items[2 * k] = (mval){.type = T_STRING, .s = (const uint8_t *)n->keys[i], .n = (uint32_t)strlen(n->keys[i])};
				items[2 * k + 1] = v;
			} else {
				items[k] = v;
			}
			k++;
		}
		*out = (mval){.type = map ? T_MAP : T_ARRAY, .n = k, .items = items};
		if (map)
			mval_sort_map(out);
		*omit = n->omit_empty && k == 0 && n != &e->t->root;
		return E_OK;
	}
	}
	return E_FATAL;
}

tm_status tmpl_record(tmpl *t, const csv_reader *row, arena *a, mval **out, char *why, size_t wl)
{
	ectx e = {t, a, why, wl};
	rowv in = {.p = row->rec.p, .ends = row->ends, .n = row->nfields};
	for (int i = 0; i < t->nrules; i++) {
		mval v;
		bool om;
		int rc = eval(&e, &t->rules[i].value, &in, &v, &om);
		if (rc != E_OK)
			return rc == E_SKIP ? TM_SKIP : rc == E_BAD ? TM_BAD : TM_FATAL;
		const uint8_t *s = om || v.type != T_STRING ? (const uint8_t *)"" : v.s;
		size_t sn = om || v.type != T_STRING ? 0 : v.n;
		if ((om || v.type == T_STRING) && listed(t->rules[i].in, t->rules[i].nin, s, sn))
			return TM_SKIP;
	}
	mval *rec = arena_alloc(a, sizeof *rec);
	bool om;
	int rc = eval(&e, &t->root, &in, rec, &om);
	if (rc != E_OK)
		return rc == E_SKIP ? TM_SKIP : rc == E_BAD ? TM_BAD : TM_FATAL;
	if (om)
		*rec = (mval){.type = T_MAP};
	if (rec->type != T_MAP) {
		err_set("%s: the record template makes a %s, not a map", t->o->record_file ? t->o->record_file : "config",
			rec->type == T_ARRAY ? "array" : "single value");
		return TM_FATAL;
	}
	if (t->skip_empty && rec->n == 0)
		return TM_SKIP;
	*out = rec;
	return TM_OK;
}

/* ---- literals ---- */

typedef struct {
	const uint8_t *s;
	size_t n, i;
	arena *a;
	char *why;
	size_t wl;
	int depth;
} lit;

static void lit_ws(lit *p)
{
	while (p->i < p->n && isspace(p->s[p->i]))
		p->i++;
}

static int lit_fail(lit *p, const char *what)
{
	snprintf(p->why, p->wl, "%s at offset %zu", what, p->i);
	return -1;
}

static void put_rune(buf_t *b, uint32_t r)
{
	if (r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF))
		r = 0xFFFD;
	if (r < 0x80) {
		buf_push(b, (uint8_t)r);
	} else if (r < 0x800) {
		buf_push(b, (uint8_t)(0xC0 | r >> 6));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	} else if (r < 0x10000) {
		buf_push(b, (uint8_t)(0xE0 | r >> 12));
		buf_push(b, (uint8_t)(0x80 | ((r >> 6) & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	} else {
		buf_push(b, (uint8_t)(0xF0 | r >> 18));
		buf_push(b, (uint8_t)(0x80 | ((r >> 12) & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | ((r >> 6) & 0x3F)));
		buf_push(b, (uint8_t)(0x80 | (r & 0x3F)));
	}
}

static int hexval(const uint8_t *s, size_t n, uint32_t *out)
{
	uint32_t v = 0;
	for (size_t i = 0; i < n; i++) {
		int c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0)
			return -1;
		v = v * 16 + (uint32_t)d;
	}
	*out = v;
	return 0;
}

static int lit_string(lit *p, mval *out)
{
	uint8_t q = p->s[p->i++];
	buf_t b = {0};
	for (;;) {
		if (p->i >= p->n) {
			buf_free(&b);
			return lit_fail(p, "a string without its closing quote");
		}
		uint8_t c = p->s[p->i++];
		if (c == q)
			break;
		if (c != '\\') {
			buf_push(&b, c);
			continue;
		}
		if (p->i >= p->n) {
			buf_free(&b);
			return lit_fail(p, "a backslash at the end");
		}
		c = p->s[p->i++];
		uint32_t r;
		size_t w = c == 'x' ? 2 : c == 'u' ? 4 : c == 'U' ? 8 : 0;
		if (w) {
			if (p->i + w > p->n || hexval(p->s + p->i, w, &r) < 0) {
				buf_free(&b);
				return lit_fail(p, "a bad \\x, \\u or \\U escape");
			}
			p->i += w;
			if (c == 'u' && r >= 0xD800 && r <= 0xDBFF && p->i + 6 <= p->n && p->s[p->i] == '\\' &&
			    p->s[p->i + 1] == 'u') {
				uint32_t lo;
				if (hexval(p->s + p->i + 2, 4, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
					r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
					p->i += 6;
				}
			}
			put_rune(&b, r);
			continue;
		}
		switch (c) {
		case 'n': buf_push(&b, '\n'); break;
		case 't': buf_push(&b, '\t'); break;
		case 'r': buf_push(&b, '\r'); break;
		case 'b': buf_push(&b, '\b'); break;
		case 'f': buf_push(&b, '\f'); break;
		case 'v': buf_push(&b, '\v'); break;
		case 'a': buf_push(&b, '\a'); break;
		case '0': buf_push(&b, 0); break;
		case '\\': case '\'': case '"': case '/': buf_push(&b, c); break;
		case '\n': break; /* a line continuation */
		default:
			/* Python keeps an unknown escape as it is */
			buf_push(&b, '\\');
			buf_push(&b, c);
		}
	}
	uint8_t *s = arena_alloc(p->a, b.len + 1);
	if (b.len)
		memcpy(s, b.p, b.len);
	*out = (mval){.type = T_STRING, .s = s, .n = (uint32_t)b.len};
	buf_free(&b);
	return 0;
}

static bool lit_word(lit *p, const char *w)
{
	size_t n = strlen(w);
	if (p->i + n > p->n || memcmp(p->s + p->i, w, n) != 0)
		return false;
	if (p->i + n < p->n && (isalnum(p->s[p->i + n]) || p->s[p->i + n] == '_'))
		return false;
	p->i += n;
	return true;
}

static int lit_value(lit *p, mval *out);

static int lit_items(lit *p, uint8_t close, bool map, mval *out)
{
	p->i++;
	if (++p->depth > 200)
		return lit_fail(p, "nesting too deep");
	buf_t items = {0};
	size_t k = 0;
	for (;;) {
		lit_ws(p);
		if (p->i < p->n && p->s[p->i] == close) {
			p->i++;
			break;
		}
		mval v[2];
		int nv = map ? 2 : 1;
		if (lit_value(p, &v[0]) < 0) {
			buf_free(&items);
			return -1;
		}
		if (map) {
			if (v[0].type != T_STRING) {
				/* a number key keeps its text */
				if (!v[0].s || !v[0].n) {
					buf_free(&items);
					return lit_fail(p, "a map key that is not a string or a number");
				}
				v[0].type = T_STRING;
			}
			lit_ws(p);
			if (p->i < p->n && p->s[p->i] == ':')
				p->i++;
			else if (p->i + 1 < p->n && p->s[p->i] == '=' && p->s[p->i + 1] == '>')
				p->i += 2;
			else {
				buf_free(&items);
				return lit_fail(p, "a map key without its ':'");
			}
			if (lit_value(p, &v[1]) < 0) {
				buf_free(&items);
				return -1;
			}
		}
		buf_append(&items, v, sizeof(mval) * (size_t)nv);
		k++;
		lit_ws(p);
		if (p->i < p->n && p->s[p->i] == ',') {
			p->i++;
			continue;
		}
		if (p->i < p->n && p->s[p->i] == close) {
			p->i++;
			break;
		}
		buf_free(&items);
		return lit_fail(p, map ? "a map entry without ',' or the closing '}'" : "an item without ',' or the closing bracket");
	}
	p->depth--;
	mval *arr = arena_alloc(p->a, items.len + sizeof(mval));
	if (items.len)
		memcpy(arr, items.p, items.len);
	buf_free(&items);
	*out = (mval){.type = map ? T_MAP : T_ARRAY, .n = (uint32_t)k, .items = arr};
	if (map) {
		/* sorted, and the later of equal keys kept, as in a Python dict */
		mval_sort_map(out);
		uint32_t w = 0;
		for (uint32_t i = 0; i < out->n; i++) {
			if (w > 0 && arr[2 * (w - 1)].n == arr[2 * i].n && memcmp(arr[2 * (w - 1)].s, arr[2 * i].s, arr[2 * i].n) == 0) {
				arr[2 * (w - 1) + 1] = arr[2 * i + 1];
				continue;
			}
			arr[2 * w] = arr[2 * i];
			arr[2 * w + 1] = arr[2 * i + 1];
			w++;
		}
		out->n = w;
	}
	return 0;
}

static int lit_value(lit *p, mval *out)
{
	lit_ws(p);
	if (p->i >= p->n)
		return lit_fail(p, "a value is missing");
	uint8_t c = p->s[p->i];
	if ((c == 'u' || c == 'U') && p->i + 1 < p->n && (p->s[p->i + 1] == '\'' || p->s[p->i + 1] == '"')) {
		p->i++; /* Python 2's u'...' */
		c = p->s[p->i];
	}
	if (c == '\'' || c == '"')
		return lit_string(p, out);
	if (c == '[')
		return lit_items(p, ']', false, out);
	if (c == '(')
		return lit_items(p, ')', false, out);
	if (c == '{')
		return lit_items(p, '}', true, out);
	if (lit_word(p, "True") || lit_word(p, "true")) {
		*out = (mval){.type = T_BOOL, .u = 1};
		return 0;
	}
	if (lit_word(p, "False") || lit_word(p, "false")) {
		*out = (mval){.type = T_BOOL, .u = 0};
		return 0;
	}
	if (lit_word(p, "None") || lit_word(p, "null") || lit_word(p, "undef")) {
		*out = (mval){.type = T_STRING, .s = (const uint8_t *)"", .n = 0};
		return 0;
	}
	size_t start = p->i;
	if (c == '-' || c == '+')
		p->i++;
	while (p->i < p->n && (isalnum(p->s[p->i]) || p->s[p->i] == '.' || p->s[p->i] == '_' ||
			       ((p->s[p->i] == '-' || p->s[p->i] == '+') && (p->s[p->i - 1] == 'e' || p->s[p->i - 1] == 'E'))))
		p->i++;
	size_t len = p->i - start;
	if (len && (p->s[p->i - 1] == 'L' || p->s[p->i - 1] == 'l'))
		len--; /* Python 2's 10L */
	const uint8_t *t = p->s + start;
	if (len && (t[0] == '+'))
		t++, len--;
	char why[200];
	bool om;
	mval v;
	if (len == 0 || conv_text(t, len, VT_AUTO, ',', false, p->a, &v, &om, why, sizeof why) < 0 || v.type == T_STRING ||
	    v.type == T_BOOL) {
		p->i = start;
		return lit_fail(p, "not a string, number, list, map, True, False or None");
	}
	/* the text is kept in s, n (unused by numbers), for a type rule that
	 * asks for it as a string */
	uint8_t *s = arena_alloc(p->a, len + 1);
	memcpy(s, t, len);
	v.s = s;
	v.n = (uint32_t)len;
	*out = v;
	return 0;
}

int tmpl_parse_literal(const uint8_t *s, size_t n, arena *a, mval *out, char *why, size_t wl)
{
	lit p = {.s = s, .n = n, .a = a, .why = why, .wl = wl};
	if (lit_value(&p, out) < 0)
		return -1;
	lit_ws(&p);
	if (p.i != p.n)
		return lit_fail(&p, "more after the value");
	return 0;
}

void tmpl_parse_list(const uint8_t *s, size_t n, arena *a, mval *out)
{
	tmpl_trim(&s, &n);
	if (n && s[0] == '[')
		s++, n--;
	if (n && s[n - 1] == ']')
		n--;
	tmpl_trim(&s, &n);
	*out = (mval){.type = T_ARRAY};
	if (n == 0)
		return;
	size_t count = 1;
	for (size_t i = 0; i < n; i++)
		count += s[i] == ',';
	mval *items = arena_alloc(a, sizeof *items * (count + 1));
	size_t k = 0, start = 0, kept = 0;
	for (size_t i = 0; i <= n; i++) {
		if (i < n && s[i] != ',')
			continue;
		const uint8_t *p = s + start;
		size_t pn = i - start;
		tmpl_trim(&p, &pn);
		/* Perl's split drops trailing fields that are empty as split, before
		 * their quotes come off: [""] is one empty item, "a,," is one item */
		if (pn)
			kept = k + 1;
		if (pn && (p[0] == '"' || p[0] == '\''))
			p++, pn--;
		if (pn && (p[pn - 1] == '"' || p[pn - 1] == '\''))
			pn--;
		items[k++] = (mval){.type = T_STRING, .s = p, .n = (uint32_t)pn};
		start = i + 1;
	}
	k = kept;
	out->n = (uint32_t)k;
	out->items = items;
}

/* ---- setup ---- */

tmpl *tmpl_new(const build_opts *o, char **names, size_t *lens, int ncols)
{
	const jvalue *r = o->record;
	const char *file = o->record_file ? o->record_file : "config";
	/* the rest of the record comes from the template, not from these */
	struct {
		bool set;
		const char *name;
	} clash[] = {{o->lookup_path != NULL, "lookup"},	{o->ninclude > 0, "fields.include"},
		     {o->nexclude > 0, "fields.exclude"},	{o->ntypes > 0, "fields.types"},
		     {o->nrenames > 0, "fields.rename"},	{o->nempties > 0, "fields.empty_if"},
		     {o->nest_keys, "fields.nest_keys"},	{o->infer_types, "fields.infer_types"},
		     {o->no_fields, "fields.no_fields"},	{o->omit_empty, "fields.empty"}};
	for (size_t i = 0; i < sizeof clash / sizeof clash[0]; i++)
		if (clash[i].set) {
			err_set("%s: \"record\" builds the whole record, so %s (or its flag) does not apply; use the template's "
				"own settings",
				file, clash[i].name);
			return NULL;
		}
	tmpl *t = xcalloc(1, sizeof *t);
	t->o = o;
	arena_init(&t->keep);
	t->in_names = names;
	t->in_lens = lens;
	t->in_ncols = ncols;
	t->keyed = !o->network_field && !o->no_fields && !o->fill_gaps;
	static const char *const K[] = {"tables", "define", "shape", "types", "trim", "empty", "invalid", "skip_if", "skip_empty", NULL};
	for (size_t i = 0; i < r->len; i++) {
		if (r->keys[i][0] == '_')
			continue;
		bool ok = false;
		for (const char *const *k = K; *k && !ok; k++)
			ok = strcmp(*k, r->keys[i]) == 0;
		if (!ok) {
			cbad(t, &r->items[i], "record", "unknown key \"%s\" (it takes tables, define, shape, types, trim, empty, "
							"invalid, skip_if, skip_empty)", r->keys[i]);
			goto fail;
		}
	}
	const jvalue *x;
	inherit in = {.table = -1, .trim = false, .omit_empty = false, .invalid = INV_BAD};
	if ((x = json_get(r, "trim"))) {
		if (x->type != J_BOOL) {
			cbad(t, x, "record.trim", "must be true or false");
			goto fail;
		}
		in.trim = x->b;
	}
	if ((x = json_get(r, "empty"))) {
		if (x->type != J_STRING || (strcmp(x->s, "keep") != 0 && strcmp(x->s, "omit") != 0)) {
			cbad(t, x, "record.empty", "must be keep or omit");
			goto fail;
		}
		in.omit_empty = strcmp(x->s, "omit") == 0;
	}
	if ((x = json_get(r, "invalid"))) {
		if (x->type != J_STRING || (strcmp(x->s, "omit") != 0 && strcmp(x->s, "skip") != 0 && strcmp(x->s, "bad") != 0)) {
			cbad(t, x, "record.invalid", "must be omit, skip or bad");
			goto fail;
		}
		in.invalid = x->s[0] == 'o' ? INV_OMIT : x->s[0] == 's' ? INV_SKIP : INV_BAD;
	}
	if ((x = json_get(r, "skip_empty"))) {
		if (x->type != J_BOOL) {
			cbad(t, x, "record.skip_empty", "must be true or false");
			goto fail;
		}
		t->skip_empty = x->b;
	}
	if ((x = json_get(r, "types"))) {
		if (x->type != J_OBJECT) {
			cbad(t, x, "record.types", "must be an object of key: type");
			goto fail;
		}
		t->tkeys = xcalloc(x->len + 1, sizeof *t->tkeys);
		t->tvals = xcalloc(x->len + 1, sizeof *t->tvals);
		for (size_t i = 0; i < x->len; i++) {
			if (x->keys[i][0] == '_')
				continue;
			char w[300];
			snprintf(w, sizeof w, "record.types.%s", x->keys[i]);
			if (x->items[i].type != J_STRING || !vtype_parse(x->items[i].s, &t->tvals[t->ntypes])) {
				cbad(t, &x->items[i], w, "must be string, uint16, uint32, uint64, uint128, int32, double, float, "
							 "boolean, array or auto");
				goto fail;
			}
			t->tkeys[t->ntypes++] = dupstr(x->keys[i]);
		}
	}
	if ((x = json_get(r, "define")) && x->type != J_OBJECT) {
		cbad(t, x, "record.define", "must be an object of name: value");
		goto fail;
	}
	t->defines = x;
	/* the tables */
	t->ntables = o->ntables;
	t->tables = xcalloc((size_t)o->ntables + 1, sizeof *t->tables);
	t->missing = xcalloc((size_t)o->ntables + 1, sizeof *t->missing);
	for (int i = 0; i < o->ntables; i++) {
		char label[300];
		snprintf(label, sizeof label, "table \"%s\"", o->tables[i].name);
		if (ktable_open(&t->tables[i], label, o->tables[i].path, o->tables[i].key, o->tmp_dir, o->lazy_quotes) < 0)
			goto fail;
	}
	if (!(x = json_get(r, "shape"))) {
		cbad(t, r, "record", "needs \"shape\": the record, as a map of key: value");
		goto fail;
	}
	if (x->type != J_OBJECT) {
		cbad(t, x, "record.shape", "must be an object: the record is a map");
		goto fail;
	}
	if (compile(t, x, &t->root, "", NULL, in) < 0)
		goto fail;
	if ((x = json_get(r, "skip_if"))) {
		if (x->type != J_ARRAY) {
			cbad(t, x, "record.skip_if", "must be an array of {\"value\": ..., \"in\": [...]}");
			goto fail;
		}
		t->rules = xcalloc(x->len + 1, sizeof *t->rules);
		for (size_t i = 0; i < x->len; i++) {
			const jvalue *ru = &x->items[i];
			char w[100];
			snprintf(w, sizeof w, "record.skip_if[%zu]", i);
			const jvalue *val = ru->type == J_OBJECT ? json_get(ru, "value") : NULL;
			const jvalue *inl = ru->type == J_OBJECT ? json_get(ru, "in") : NULL;
			if (!val || !inl || ru->len != 2) {
				cbad(t, ru, w, "must be {\"value\": ..., \"in\": [...]}");
				goto fail;
			}
			skip_rule *sr = &t->rules[t->nrules++];
			inherit sub = in;
			sub.omit_empty = false;
			sub.invalid = INV_OMIT;
			if (compile(t, val, &sr->value, w, NULL, sub) < 0 || strlist(t, inl, w, &sr->in, &sr->nin) < 0)
				goto fail;
			sr->value.type = VT_STRING;
		}
	}
	size_t l = (size_t)snprintf(t->describe, sizeof t->describe, "record template");
	for (int i = 0; i < t->ntables && l < sizeof t->describe; i++)
		l += (size_t)snprintf(t->describe + l, sizeof t->describe - l, "%s table \"%s\" (%llu rows)", i ? "," : ";",
				      o->tables[i].name, (unsigned long long)t->tables[i].rows);
	t->in_names = NULL;
	t->in_lens = NULL;
	return t;
fail:
	tmpl_free(t);
	return NULL;
}

const uint8_t *tmpl_key(tmpl *t, const csv_reader *row, size_t *n)
{
	if (!t->keyed)
		return NULL;
	t->key.len = 0;
	rowv in = {.p = row->rec.p, .ends = row->ends, .n = row->nfields};
	for (int i = 0; i < t->nused; i++) {
		const uint8_t *v;
		size_t vn;
		field(&in, t->used[i], &v, &vn);
		uint8_t len[4] = {(uint8_t)(vn >> 24), (uint8_t)(vn >> 16), (uint8_t)(vn >> 8), (uint8_t)vn};
		buf_append(&t->key, len, 4);
		if (vn)
			buf_append(&t->key, v, vn);
	}
	*n = t->key.len;
	return t->key.len ? t->key.p : (const uint8_t *)"";
}

const char *tmpl_describe(const tmpl *t) { return t->describe; }

uint64_t tmpl_utf8_fixed(const tmpl *t) { return t->utf8_fixed; }

void tmpl_set_shared(tmpl *t, shared *sh) { t->sh = sh; }

static void report_invalid(const node *n, buf_t *out, int *shown)
{
	if (n->invalid_n && (*shown)++ < 20)
		buf_printf(out, "note: %s: %llu invalid value%s left out\n", n->path, (unsigned long long)n->invalid_n,
			   n->invalid_n == 1 ? "" : "s");
	for (int i = 0; i < n->nkids; i++)
		report_invalid(&n->kids[i], out, shown);
	if (n->strip_eq)
		report_invalid(n->strip_eq, out, shown);
}

void tmpl_report(const tmpl *t, buf_t *out)
{
	char a1[32];
	for (int i = 0; i < t->ntables; i++)
		if (t->missing[i])
			buf_printf(out, "note: table \"%s\" had no row for the id in %s lookup%s\n", t->o->tables[i].name,
				   fmt_int(t->missing[i], a1), t->missing[i] == 1 ? "" : "s");
	int shown = 0;
	report_invalid(&t->root, out, &shown);
	for (int i = 0; i < t->nrules; i++)
		report_invalid(&t->rules[i].value, out, &shown);
}

void tmpl_free(tmpl *t)
{
	if (!t)
		return;
	node_free(&t->root);
	for (int i = 0; i < t->nrules; i++) {
		node_free(&t->rules[i].value);
		for (int k = 0; k < t->rules[i].nin; k++)
			free(t->rules[i].in[k]);
		free(t->rules[i].in);
	}
	free(t->rules);
	for (int i = 0; i < t->ntables; i++)
		ktable_close(&t->tables[i]);
	free(t->tables);
	free(t->missing);
	for (int i = 0; i < t->ntypes; i++)
		free(t->tkeys[i]);
	free(t->tkeys);
	free(t->tvals);
	free(t->used);
	buf_free(&t->key);
	buf_free(&t->enc);
	free(t->deltas);
	free(t->snap);
	arena_free(&t->keep);
	free(t);
}
