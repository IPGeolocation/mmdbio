#include "flags.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void strlist_push(strlist *l, const char *s)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 8;
		l->v = xrealloc(l->v, l->cap * sizeof *l->v);
	}
	size_t n = strlen(s) + 1;
	l->v[l->n] = xmalloc(n);
	memcpy(l->v[l->n++], s, n);
}

void strlist_free(strlist *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof *l);
}

static void push_split(strlist *l, const char *s)
{
	const char *p = s;
	for (;;) {
		const char *c = strchr(p, ',');
		size_t n = c ? (size_t)(c - p) : strlen(p);
		char *item = xmalloc(n + 1);
		memcpy(item, p, n);
		item[n] = 0;
		strlist_push(l, item);
		free(item);
		if (!c)
			break;
		p = c + 1;
	}
}

static bool parse_bool_text(const char *s, bool *out)
{
	static const char *const t[] = {"1", "t", "T", "true", "TRUE", "True"};
	static const char *const f[] = {"0", "f", "F", "false", "FALSE", "False"};
	for (int i = 0; i < 6; i++) {
		if (strcmp(s, t[i]) == 0) {
			*out = true;
			return true;
		}
		if (strcmp(s, f[i]) == 0) {
			*out = false;
			return true;
		}
	}
	return false;
}

void flags_reset(command *c)
{
	for (int i = 0; i < c->nflags; i++) {
		flag *f = &c->flags[i];
		f->set = false;
		f->str = f->def ? f->def : "";
		f->b = f->def && strcmp(f->def, "true") == 0;
		f->i = f->def ? strtoll(f->def, NULL, 0) : 0;
		strlist_free(&f->list);
	}
}

flag *flag_get(command *c, const char *name)
{
	for (int i = 0; i < c->nflags; i++)
		if (strcmp(c->flags[i].name, name) == 0)
			return &c->flags[i];
	return NULL;
}

static flag *find_long(command *c, const char *name, size_t n)
{
	for (int i = 0; i < c->nflags; i++)
		if (strlen(c->flags[i].name) == n && memcmp(c->flags[i].name, name, n) == 0)
			return &c->flags[i];
	return NULL;
}

static flag *find_short(command *c, char s)
{
	for (int i = 0; i < c->nflags; i++)
		if (c->flags[i].shorthand && c->flags[i].shorthand == s)
			return &c->flags[i];
	return NULL;
}

/* "mmdbio read" */
static void print_command_path(FILE *out, const command *c)
{
	fprintf(out, "mmdbio %s", c->name);
}

static int bad(const command *c, const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "Error: ");
	fprintf(stderr, fmt, a, b);
	fprintf(stderr, "\nRun '");
	print_command_path(stderr, c);
	fprintf(stderr, " --help' for usage.\n");
	return -1;
}

static const char *type_name(flag_type t)
{
	switch (t) {
	case F_STRING:
		return "string";
	case F_INT:
		return "int";
	case F_LIST:
		return "strings";
	case F_ARRAY:
		return "stringArray";
	default:
		return "";
	}
}

static int set_value(command *c, flag *f, const char *val, const char *shown)
{
	switch (f->type) {
	case F_STRING:
		f->str = val;
		break;
	case F_BOOL:
		if (!parse_bool_text(val, &f->b))
			return bad(c, "invalid argument \"%s\" for \"%s\" flag: strconv.ParseBool: parsing: invalid syntax",
				   val, shown);
		break;
	case F_INT: {
		char *end;
		errno = 0;
		long long v = strtoll(val, &end, 0);
		if (!*val || *end || errno)
			return bad(c, "invalid argument \"%s\" for \"%s\" flag: strconv.ParseInt: parsing: invalid syntax",
				   val, shown);
		f->i = v;
		break;
	}
	case F_LIST:
		if (!f->set)
			strlist_free(&f->list);
		push_split(&f->list, val);
		break;
	case F_ARRAY:
		if (!f->set)
			strlist_free(&f->list);
		strlist_push(&f->list, val);
		break;
	}
	f->set = true;
	return 0;
}

int flags_parse(command *c, int argc, char **argv, int *nargs)
{
	flags_reset(c);
	int npos = 0;
	char **pos = xmalloc(sizeof *pos * (size_t)(argc + 1));
	int rc = 0;
	for (int i = 0; i < argc; i++) {
		char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			for (i++; i < argc; i++)
				pos[npos++] = argv[i];
			break;
		}
		if (a[0] != '-' || a[1] == 0) {
			pos[npos++] = a;
			continue;
		}
		if (a[1] == '-') {
			const char *name = a + 2;
			const char *eq = strchr(name, '=');
			size_t n = eq ? (size_t)(eq - name) : strlen(name);
			if (n == 4 && strncmp(name, "help", 4) == 0) {
				rc = 1;
				goto done;
			}
			flag *f = find_long(c, name, n);
			if (!f) {
				char nm[128];
				snprintf(nm, sizeof nm, "%.*s", (int)(n < 100 ? n : 100), name);
				rc = bad(c, "unknown flag: --%s%s", nm, "");
				goto done;
			}
			char shown[140];
			snprintf(shown, sizeof shown, "--%s", f->name);
			const char *val = eq ? eq + 1 : NULL;
			if (!val) {
				if (f->type == F_BOOL) {
					val = "true";
				} else if (i + 1 < argc) {
					val = argv[++i];
				} else {
					rc = bad(c, "flag needs an argument: %s%s", shown, "");
					goto done;
				}
			}
			if (set_value(c, f, val, shown) < 0) {
				rc = -1;
				goto done;
			}
			continue;
		}
		/* shorthands: -i value, -ivalue, -i=value, -abc for bools */
		for (const char *p = a + 1; *p; p++) {
			if (*p == 'h') {
				rc = 1;
				goto done;
			}
			flag *f = find_short(c, *p);
			if (!f) {
				char s[2] = {*p, 0};
				rc = bad(c, "unknown shorthand flag: '%s' in %s", s, a);
				goto done;
			}
			char shown[8];
			snprintf(shown, sizeof shown, "-%c", *p);
			const char *val = NULL;
			if (p[1] == '=') {
				val = p + 2;
			} else if (f->type == F_BOOL) {
				val = "true";
			} else if (p[1]) {
				val = p + 1;
			} else if (i + 1 < argc) {
				val = argv[++i];
			} else {
				rc = bad(c, "flag needs an argument: %s%s", shown, "");
				goto done;
			}
			if (set_value(c, f, val, shown) < 0) {
				rc = -1;
				goto done;
			}
			if (f->type != F_BOOL || p[1] == '=')
				break;
		}
	}
done:
	for (int k = 0; k < npos; k++)
		argv[k] = pos[k];
	*nargs = npos;
	free(pos);
	return rc;
}

static int cmp_flag(const void *a, const void *b)
{
	return strcmp(((const flag *)a)->name, ((const flag *)b)->name);
}

void command_help(const command *c, FILE *out)
{
	const char *text = c->long_desc ? c->long_desc : c->short_desc;
	fprintf(out, "%s\n\nUsage:\n  mmdbio %s\n", text, c->use);
	if (c->examples)
		fprintf(out, "\nExamples:\n%s\n", c->examples);
	int n = c->nflags;
	flag *sorted = xmalloc(sizeof *sorted * (size_t)(n + 1));
	memcpy(sorted, c->flags, sizeof *sorted * (size_t)n);
	sorted[n] = (flag){.name = "help", .shorthand = 'h', .type = F_BOOL};
	char help_usage[64];
	snprintf(help_usage, sizeof help_usage, "help for %s", c->name);
	sorted[n].usage = help_usage;
	n++;
	qsort(sorted, (size_t)n, sizeof *sorted, cmp_flag);
	size_t width = 0;
	char line[160];
	for (int i = 0; i < n; i++) {
		const flag *f = &sorted[i];
		if (f->shorthand)
			snprintf(line, sizeof line, "  -%c, --%s %s", f->shorthand, f->name, type_name(f->type));
		else
			snprintf(line, sizeof line, "      --%s %s", f->name, type_name(f->type));
		size_t l = strlen(line);
		while (l && line[l - 1] == ' ')
			line[--l] = 0;
		if (l > width)
			width = l;
	}
	fprintf(out, "\nFlags:\n");
	for (int i = 0; i < n; i++) {
		const flag *f = &sorted[i];
		if (f->shorthand)
			snprintf(line, sizeof line, "  -%c, --%s %s", f->shorthand, f->name, type_name(f->type));
		else
			snprintf(line, sizeof line, "      --%s %s", f->name, type_name(f->type));
		size_t l = strlen(line);
		while (l && line[l - 1] == ' ')
			line[--l] = 0;
		fprintf(out, "%s%*s   %s", line, (int)(width - l), "", f->usage);
		if (f->def && *f->def && !(f->type == F_BOOL && strcmp(f->def, "false") == 0) &&
		    !(f->type == F_INT && strcmp(f->def, "0") == 0)) {
			if (f->type == F_STRING)
				fprintf(out, " (default \"%s\")", f->def);
			else
				fprintf(out, " (default %s)", f->def);
		}
		fputc('\n', out);
	}
	free(sorted);
}
