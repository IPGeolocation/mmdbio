/* build_opts: the defaults and the config file. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#include "build.h"
#include "json.h"

static char *dupstr(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

void opts_set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = v ? dupstr(v) : NULL;
}

static void free_strs(char **v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

void build_opts_init(build_opts *o)
{
	memset(o, 0, sizeof *o);
	o->header = -1;
	o->ip_version = 6;
	o->include_reserved = true;
	o->array_sep = ',';
	o->memory = (size_t)512 << 20;
}

void opts_set_rename(build_opts *o, const char *column, const char *key)
{
	for (int i = 0; i < o->nrenames; i++)
		if (strcmp(o->renames[i].column, column) == 0) {
			opts_set_str(&o->renames[i].key, key);
			return;
		}
	o->renames = xrealloc(o->renames, (size_t)(o->nrenames + 1) * sizeof *o->renames);
	o->renames[o->nrenames] = (rename_rule){NULL, NULL};
	opts_set_str(&o->renames[o->nrenames].column, column);
	opts_set_str(&o->renames[o->nrenames].key, key);
	o->nrenames++;
}

void opts_add_empty(build_opts *o, const char *column, const char *value)
{
	o->empties = xrealloc(o->empties, (size_t)(o->nempties + 1) * sizeof *o->empties);
	o->empties[o->nempties] = (empty_rule){NULL, NULL};
	opts_set_str(&o->empties[o->nempties].column, column);
	opts_set_str(&o->empties[o->nempties].value, value);
	o->nempties++;
}

void build_opts_free(build_opts *o)
{
	free(o->input);
	free(o->output);
	free(o->network_column);
	free(o->start_column);
	free(o->end_column);
	free(o->database_type);
	free_strs(o->desc_langs, o->ndesc);
	free_strs(o->desc_texts, o->ndesc);
	free_strs(o->languages, o->nlanguages);
	free_strs(o->include, o->ninclude);
	free_strs(o->exclude, o->nexclude);
	for (int i = 0; i < o->ntypes; i++)
		free(o->types[i].path);
	free(o->types);
	free(o->tmp_dir);
	free_strs(o->columns, o->ncolumns);
	free(o->lookup_path);
	free(o->lookup_key);
	free(o->lookup_on);
	for (int i = 0; i < o->nrenames; i++) {
		free(o->renames[i].column);
		free(o->renames[i].key);
	}
	free(o->renames);
	for (int i = 0; i < o->nempties; i++) {
		free(o->empties[i].column);
		free(o->empties[i].value);
	}
	free(o->empties);
	memset(o, 0, sizeof *o);
}

static const char *const VT_NAMES[] = {"", "string", "uint16", "uint32", "uint64", "uint128", "int32", "double",
				       "float", "boolean", "array", "auto"};

bool vtype_parse(const char *name, vtype *out)
{
	for (int i = 1; i < (int)(sizeof VT_NAMES / sizeof VT_NAMES[0]); i++) {
		if (strcmp(name, VT_NAMES[i]) == 0) {
			*out = (vtype)i;
			return true;
		}
	}
	if (strcmp(name, "bool") == 0) {
		*out = VT_BOOL;
		return true;
	}
	if (strcmp(name, "utf8_string") == 0) {
		*out = VT_STRING;
		return true;
	}
	return false;
}

const char *vtype_name(vtype t) { return VT_NAMES[t]; }

void opts_set_type(build_opts *o, const char *path, vtype t)
{
	for (int i = 0; i < o->ntypes; i++) {
		if (strcmp(o->types[i].path, path) == 0) {
			o->types[i].type = t;
			return;
		}
	}
	o->types = xrealloc(o->types, sizeof *o->types * (size_t)(o->ntypes + 1));
	o->types[o->ntypes++] = (type_rule){dupstr(path), t};
}

/* ---- the config file ---- */

typedef struct {
	const char *file;
	buf_t dir;
} cctx;

static int bad(const cctx *c, const jvalue *v, const char *where, const char *fmt, ...)
    PRINTF_FMT(4, 5);
static int bad(const cctx *c, const jvalue *v, const char *where, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	err_set("%s:%d: %s: %s", c->file, v->line, where, msg);
	return -1;
}

static int known(const cctx *c, const jvalue *o, const char *where, const char *const *keys)
{
	if (o->type != J_OBJECT)
		return bad(c, o, where, "must be an object, not %s", json_type_name(o->type));
	for (size_t i = 0; i < o->len; i++) {
		if (o->keys[i][0] == '_')
			continue;
		bool ok = false;
		for (const char *const *k = keys; *k && !ok; k++)
			ok = strcmp(*k, o->keys[i]) == 0;
		if (!ok) {
			buf_t list = {0};
			for (const char *const *k = keys; *k; k++)
				buf_printf(&list, "%s%s", k == keys ? "" : ", ", *k);
			bad(c, &o->items[i], where, "unknown key \"%s\" (it takes %s)", o->keys[i], buf_cstr(&list));
			buf_free(&list);
			return -1;
		}
	}
	return 0;
}

static int str(const cctx *c, const jvalue *v, const char *where, bool empty_ok, char **out)
{
	if (v->type != J_STRING)
		return bad(c, v, where, "must be a string, not %s", json_type_name(v->type));
	if (v->has_nul)
		return bad(c, v, where, "must not hold \\u0000");
	if (!empty_ok && v->n == 0)
		return bad(c, v, where, "must not be empty");
	opts_set_str(out, v->s);
	return 0;
}

static int boolean(const cctx *c, const jvalue *v, const char *where, bool *out)
{
	if (v->type != J_BOOL)
		return bad(c, v, where, "must be true or false, not %s", json_type_name(v->type));
	*out = v->b;
	return 0;
}

static int path_value(const cctx *c, const jvalue *v, const char *where, char **out)
{
	char *raw = NULL;
	if (str(c, v, where, false, &raw) < 0)
		return -1;
	buf_t p = {0};
	if (raw[0] == '~' && (raw[1] == '/' || raw[1] == 0) && getenv("HOME")) {
		buf_printf(&p, "%s%s", getenv("HOME"), raw + 1);
	} else if (raw[0] == '/' || strcmp(raw, "-") == 0) {
		buf_printf(&p, "%s", raw);
	} else {
		const char *parts[] = {(const char *)c->dir.p, raw};
		path_join(&p, parts, 2);
	}
	opts_set_str(out, buf_cstr(&p));
	buf_free(&p);
	free(raw);
	return 0;
}

static int strings(const cctx *c, const jvalue *v, const char *where, char ***out, int *n)
{
	if (v->type != J_ARRAY)
		return bad(c, v, where, "must be an array of strings, not %s", json_type_name(v->type));
	free_strs(*out, *n);
	*out = xcalloc(v->len + 1, sizeof **out);
	*n = 0;
	for (size_t i = 0; i < v->len; i++) {
		char w[200];
		snprintf(w, sizeof w, "%s[%zu]", where, i);
		char *s = NULL;
		if (str(c, &v->items[i], w, false, &s) < 0)
			return -1;
		(*out)[(*n)++] = s;
	}
	return 0;
}

static int delimiter(const cctx *c, const jvalue *v, char *out)
{
	char *s = NULL;
	if (str(c, v, "input.delimiter", false, &s) < 0)
		return -1;
	static const struct {
		const char *name;
		char d;
	} names[] = {{"comma", ','}, {"tab", '\t'}, {"\\t", '\t'}, {"semicolon", ';'}, {"pipe", '|'}, {"space", ' '}};
	int rc = 0;
	*out = 0;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strcmp(s, names[i].name) == 0)
			*out = names[i].d;
	if (!*out) {
		if (strlen(s) == 1 && s[0] != '"' && s[0] != '\n' && s[0] != '\r')
			*out = s[0];
		else
			rc = bad(c, v, "input.delimiter", "must be one character (or comma, tab, semicolon, pipe, space)");
	}
	free(s);
	return rc;
}

static int load(build_opts *o, const cctx *c, const jvalue *root)
{
	static const char *const TOP[] = {"input", "output", "network", "metadata", "tree", "merge",
					  "on_error", "fields", "temp_dir", "memory_mb", "shrink", "lookup", NULL};
	if (known(c, root, "config", TOP) < 0)
		return -1;
	const jvalue *v;
	if ((v = json_get(root, "input"))) {
		if (v->type == J_STRING) {
			if (path_value(c, v, "input", &o->input) < 0)
				return -1;
		} else {
			static const char *const K[] = {"path", "format", "delimiter", "header", "lazy_quotes", "columns", NULL};
			if (known(c, v, "input", K) < 0)
				return -1;
			const jvalue *x;
			if ((x = json_get(v, "path")) && path_value(c, x, "input.path", &o->input) < 0)
				return -1;
			if ((x = json_get(v, "format"))) {
				char *f = NULL;
				if (str(c, x, "input.format", false, &f) < 0)
					return -1;
				int rc = 0;
				if (strcmp(f, "auto") == 0)
					o->format = FMT_AUTO;
				else if (strcmp(f, "csv") == 0 || strcmp(f, "tsv") == 0)
					o->format = FMT_CSV;
				else if (strcmp(f, "json") == 0)
					o->format = FMT_JSON;
				else if (strcmp(f, "jsonl") == 0 || strcmp(f, "ndjson") == 0)
					o->format = FMT_JSONL;
				else
					rc = bad(c, x, "input.format", "must be auto, csv, json or jsonl, not \"%s\"", f);
				if (strcmp(f, "tsv") == 0 && !o->delimiter)
					o->delimiter = '\t';
				free(f);
				if (rc < 0)
					return -1;
			}
			if ((x = json_get(v, "delimiter")) && delimiter(c, x, &o->delimiter) < 0)
				return -1;
			if ((x = json_get(v, "header"))) {
				if (x->type == J_STRING && strcmp(x->s, "auto") == 0) {
					o->header = -1;
				} else {
					bool h = false;
					if (boolean(c, x, "input.header", &h) < 0)
						return -1;
					o->header = h;
				}
			}
			if ((x = json_get(v, "columns")) && strings(c, x, "input.columns", &o->columns, &o->ncolumns) < 0)
				return -1;
			if ((x = json_get(v, "lazy_quotes")) && boolean(c, x, "input.lazy_quotes", &o->lazy_quotes) < 0)
				return -1;
		}
	}
	if ((v = json_get(root, "output")) && path_value(c, v, "output", &o->output) < 0)
		return -1;
	if ((v = json_get(root, "network"))) {
		if (v->type == J_STRING) {
			if (str(c, v, "network", false, &o->network_column) < 0)
				return -1;
		} else {
			static const char *const K[] = {"column", "start_column", "end_column", NULL};
			if (known(c, v, "network", K) < 0)
				return -1;
			const jvalue *x;
			if ((x = json_get(v, "column")) && str(c, x, "network.column", false, &o->network_column) < 0)
				return -1;
			if ((x = json_get(v, "start_column")) && str(c, x, "network.start_column", false, &o->start_column) < 0)
				return -1;
			if ((x = json_get(v, "end_column")) && str(c, x, "network.end_column", false, &o->end_column) < 0)
				return -1;
			if (!o->start_column != !o->end_column)
				return bad(c, v, "network", "start_column and end_column go together");
		}
	}
	if ((v = json_get(root, "metadata"))) {
		static const char *const K[] = {"database_type", "description", "languages", "ip_version", "record_size", NULL};
		if (known(c, v, "metadata", K) < 0)
			return -1;
		const jvalue *x;
		if ((x = json_get(v, "database_type")) && str(c, x, "metadata.database_type", false, &o->database_type) < 0)
			return -1;
		if ((x = json_get(v, "description"))) {
			free_strs(o->desc_langs, o->ndesc);
			free_strs(o->desc_texts, o->ndesc);
			o->desc_langs = o->desc_texts = NULL;
			o->ndesc = 0;
			if (x->type == J_STRING) {
				o->desc_langs = xcalloc(2, sizeof *o->desc_langs);
				o->desc_texts = xcalloc(2, sizeof *o->desc_texts);
				opts_set_str(&o->desc_langs[0], "en");
				if (str(c, x, "metadata.description", true, &o->desc_texts[0]) < 0)
					return -1;
				o->ndesc = 1;
			} else if (x->type == J_OBJECT && x->len > 0) {
				o->desc_langs = xcalloc(x->len + 1, sizeof *o->desc_langs);
				o->desc_texts = xcalloc(x->len + 1, sizeof *o->desc_texts);
				for (size_t i = 0; i < x->len; i++) {
					char w[200];
					snprintf(w, sizeof w, "metadata.description.%s", x->keys[i]);
					opts_set_str(&o->desc_langs[i], x->keys[i]);
					o->ndesc = (int)i + 1;
					if (str(c, &x->items[i], w, true, &o->desc_texts[i]) < 0)
						return -1;
				}
			} else {
				return bad(c, x, "metadata.description",
					   "must be a string or a non-empty object of language: text");
			}
		}
		if ((x = json_get(v, "languages"))) {
			if (strings(c, x, "metadata.languages", &o->languages, &o->nlanguages) < 0)
				return -1;
			o->languages_set = true;
		}
		if ((x = json_get(v, "ip_version"))) {
			if (x->type != J_NUMBER || (x->num != 4 && x->num != 6))
				return bad(c, x, "metadata.ip_version", "must be 4 or 6");
			o->ip_version = (int)x->num;
		}
		if ((x = json_get(v, "record_size"))) {
			if (x->type == J_STRING && strcmp(x->s, "auto") == 0)
				o->record_size = 0;
			else if (x->type == J_NUMBER && (x->num == 24 || x->num == 28 || x->num == 32))
				o->record_size = (int)x->num;
			else
				return bad(c, x, "metadata.record_size", "must be 24, 28, 32 or \"auto\"");
		}
	}
	if ((v = json_get(root, "tree"))) {
		static const char *const K[] = {"alias_ipv4", "include_reserved", "fill_gaps", NULL};
		if (known(c, v, "tree", K) < 0)
			return -1;
		const jvalue *x;
		if ((x = json_get(v, "alias_ipv4")) && boolean(c, x, "tree.alias_ipv4", &o->alias_ipv4) < 0)
			return -1;
		if ((x = json_get(v, "fill_gaps")) && boolean(c, x, "tree.fill_gaps", &o->fill_gaps) < 0)
			return -1;
		if ((x = json_get(v, "include_reserved")) && boolean(c, x, "tree.include_reserved", &o->include_reserved) < 0)
			return -1;
	}
	if ((v = json_get(root, "merge"))) {
		char *m = NULL;
		if (str(c, v, "merge", false, &m) < 0)
			return -1;
		int rc = 0;
		if (strcmp(m, "replace") == 0 || strcmp(m, "none") == 0)
			o->merge = MERGE_REPLACE;
		else if (strcmp(m, "keep") == 0)
			o->merge = MERGE_KEEP;
		else if (strcmp(m, "toplevel") == 0)
			o->merge = MERGE_TOPLEVEL;
		else if (strcmp(m, "recurse") == 0)
			o->merge = MERGE_RECURSE;
		else
			rc = bad(c, v, "merge", "must be replace, keep, toplevel or recurse, not \"%s\"", m);
		free(m);
		if (rc < 0)
			return -1;
	}
	if ((v = json_get(root, "on_error"))) {
		if (v->type != J_STRING || (strcmp(v->s, "skip") != 0 && strcmp(v->s, "fail") != 0))
			return bad(c, v, "on_error", "must be skip or fail");
		o->strict = strcmp(v->s, "fail") == 0;
	}
	if ((v = json_get(root, "fields"))) {
		static const char *const K[] = {"include",	 "exclude",   "types",	  "infer_types", "nest_keys",
						"empty",	 "array_separator", "network_field", "no_fields",
						"rename",	 "empty_if",  NULL};
		if (known(c, v, "fields", K) < 0)
			return -1;
		const jvalue *x;
		if ((x = json_get(v, "include")) && strings(c, x, "fields.include", &o->include, &o->ninclude) < 0)
			return -1;
		if ((x = json_get(v, "exclude")) && strings(c, x, "fields.exclude", &o->exclude, &o->nexclude) < 0)
			return -1;
		if ((x = json_get(v, "types"))) {
			if (x->type != J_OBJECT)
				return bad(c, x, "fields.types", "must be an object of field: type");
			for (size_t i = 0; i < x->len; i++) {
				char w[300];
				snprintf(w, sizeof w, "fields.types.%s", x->keys[i]);
				vtype t;
				if (x->items[i].type != J_STRING || !vtype_parse(x->items[i].s, &t))
					return bad(c, &x->items[i], w,
						   "must be string, uint16, uint32, uint64, uint128, int32, double, float, "
						   "boolean, array or auto");
				opts_set_type(o, x->keys[i], t);
			}
		}
		if ((x = json_get(v, "infer_types")) && boolean(c, x, "fields.infer_types", &o->infer_types) < 0)
			return -1;
		if ((x = json_get(v, "nest_keys")) && boolean(c, x, "fields.nest_keys", &o->nest_keys) < 0)
			return -1;
		if ((x = json_get(v, "network_field")) && boolean(c, x, "fields.network_field", &o->network_field) < 0)
			return -1;
		if ((x = json_get(v, "no_fields")) && boolean(c, x, "fields.no_fields", &o->no_fields) < 0)
			return -1;
		if ((x = json_get(v, "rename"))) {
			if (x->type != J_OBJECT)
				return bad(c, x, "fields.rename", "must be an object of column: new name");
			for (size_t i = 0; i < x->len; i++) {
				char w[300];
				snprintf(w, sizeof w, "fields.rename.%s", x->keys[i]);
				char *key = NULL;
				if (str(c, &x->items[i], w, false, &key) < 0)
					return -1;
				opts_set_rename(o, x->keys[i], key);
				free(key);
			}
		}
		if ((x = json_get(v, "empty_if"))) {
			if (x->type != J_OBJECT)
				return bad(c, x, "fields.empty_if", "must be an object of column: value (or [values])");
			for (size_t i = 0; i < x->len; i++) {
				char w[300];
				snprintf(w, sizeof w, "fields.empty_if.%s", x->keys[i]);
				const jvalue *e = &x->items[i];
				if (e->type == J_STRING) {
					opts_add_empty(o, x->keys[i], e->s);
					continue;
				}
				char **vals = NULL;
				int nv = 0;
				if (strings(c, e, w, &vals, &nv) < 0)
					return -1;
				for (int k = 0; k < nv; k++)
					opts_add_empty(o, x->keys[i], vals[k]);
				free_strs(vals, nv);
			}
		}
		if ((x = json_get(v, "empty"))) {
			if (x->type != J_STRING || (strcmp(x->s, "keep") != 0 && strcmp(x->s, "omit") != 0))
				return bad(c, x, "fields.empty", "must be keep or omit");
			o->omit_empty = strcmp(x->s, "omit") == 0;
		}
		if ((x = json_get(v, "array_separator"))) {
			if (x->type != J_STRING || x->n != 1)
				return bad(c, x, "fields.array_separator", "must be one character");
			o->array_sep = x->s[0];
		}
	}
	if ((v = json_get(root, "temp_dir")) && path_value(c, v, "temp_dir", &o->tmp_dir) < 0)
		return -1;
	if ((v = json_get(root, "shrink")) && boolean(c, v, "shrink", &o->shrink) < 0)
		return -1;
	if ((v = json_get(root, "lookup"))) {
		if (v->type == J_STRING) {
			if (path_value(c, v, "lookup", &o->lookup_path) < 0)
				return -1;
		} else {
			static const char *const K[] = {"path", "key_column", "input_column", NULL};
			if (known(c, v, "lookup", K) < 0)
				return -1;
			const jvalue *x;
			if (!(x = json_get(v, "path")))
				return bad(c, v, "lookup", "needs \"path\"");
			if (path_value(c, x, "lookup.path", &o->lookup_path) < 0)
				return -1;
			if ((x = json_get(v, "key_column")) && str(c, x, "lookup.key_column", false, &o->lookup_key) < 0)
				return -1;
			if ((x = json_get(v, "input_column")) && str(c, x, "lookup.input_column", false, &o->lookup_on) < 0)
				return -1;
		}
	}
	if ((v = json_get(root, "memory_mb"))) {
		if (v->type != J_NUMBER || v->num < 16 || v->num > 1e7)
			return bad(c, v, "memory_mb", "must be a number of megabytes, at least 16");
		o->memory = (size_t)v->num << 20;
	}
	return 0;
}

int build_opts_load_config(build_opts *o, const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err_set("open %s: %s", path, go_strerror(errno));
		return -1;
	}
	buf_t text = {0};
	for (;;) {
		buf_grow(&text, 1 << 16);
		ssize_t n = read(fd, text.p + text.len, text.cap - text.len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err_set("read %s: %s", path, go_strerror(errno));
			close(fd);
			buf_free(&text);
			return -1;
		}
		if (n == 0)
			break;
		text.len += (size_t)n;
	}
	close(fd);
	cctx c = {.file = path};
	path_dir(&c.dir, path);
	buf_cstr(&c.dir);
	jvalue root;
	int rc = json_parse(text.p, text.len, &root);
	buf_free(&text);
	if (rc < 0) {
		err_prefix("%s", path);
		buf_free(&c.dir);
		return -1;
	}
	rc = load(o, &c, &root);
	json_free(&root);
	buf_free(&c.dir);
	return rc;
}
