/* read: look up IP data: one IP, a list, every address of a CIDR or range,
 * or any mix of them given as arguments, as JSON, JSON Lines, CSV or TSV. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cmds.h"
#include "input.h"
#include "jsonw.h"
#include "netaddr.h"
#include "tabular.h"

static flag read_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the last argument)"},
	{.name = "ip", .type = F_STRING, .usage = "IP address to lookup"},
	{.name = "fields",
	 .type = F_LIST,
	 .usage = "Comma-separated list of fields to extract (e.g. location.country.name,city.names.en)"},
	{.name = "input", .type = F_STRING, .usage = "Path to file with IPs (or '-' for stdin)"},
	{.name = "out", .type = F_STRING, .usage = "Optional output file path"},
	{.name = "range", .type = F_STRING, .usage = "CIDR range (e.g. 192.168.1.0/24) to lookup all IPs in range"},
	{.name = "format",
	 .shorthand = 'f',
	 .type = F_STRING,
	 .def = "json",
	 .usage = "Output: json (one object keyed by IP), jsonl (a line per IP, with \"ip\"), json-pretty, csv or tsv"},
	{.name = "no-header", .type = F_BOOL, .usage = "CSV/TSV without the header row"},
	{.name = "flatten",
	 .type = F_BOOL,
	 .usage = "CSV/TSV: a column per nested field (location.city.name) instead of JSON in one cell"},
};

static int run_read(command *c, int argc, char **argv);

command cmd_read = {
	.name = "read",
	.use = "read [flags] [IP | CIDR | range | file | -]... [db.mmdb]",
	.short_desc = "Read IP data from an MMDB file (supports single, batch, or CIDR range mode)",
	.long_desc =
		"Read IP data from an MMDB file.\n\n"
		"Give the addresses as arguments, any mix of: IPs, CIDRs (8.8.8.0/24), ranges (1.1.1.0-1.1.1.9 or\n"
		"1.1.1.0,1.1.1.9), files with one of those per line, and - for stdin. The database is --db or the\n"
		"last argument. Every address of a CIDR or range is looked up (at most 2^32 per range).\n\n"
		"Output formats (--format):\n"
		"  json         one object keyed by IP; a single IP prints its record alone\n"
		"  jsonl        one JSON object per IP, its address under \"ip\"\n"
		"  json-pretty  the same objects, indented\n"
		"  csv, tsv     a row per IP: ip, then the record's fields\n"
		"In jsonl, json-pretty, csv and tsv, IPs without data are left out.",
	.examples = "  mmdbio read 8.8.8.8 city.mmdb\n"
		    "  mmdbio read 8.8.8.0/30 1.1.1.1-1.1.1.3 ips.txt city.mmdb -f csv\n"
		    "  cat ips.txt | mmdbio read - city.mmdb -f jsonl --fields location.country.iso_code\n"
		    "  mmdbio read --db city.mmdb --ip 8.8.8.8",
	.flags = read_flags,
	.nflags = (int)(sizeof read_flags / sizeof read_flags[0]),
	.run = run_read,
};

typedef struct {
	mmdb_reader r;
	arena a;
	strlist *fields;
} reader_ctx;

/* The JSON (at depth 1) of one lookup: the record, the --fields map, null,
 * or the Go error strings. */
static void lookup_json(reader_ctx *x, const char *ip_text, size_t n, buf_t *out, bool *found)
{
	u128 ip;
	bool is4;
	*found = false;
	if (!parse_ip_text(ip_text, n, &ip, &is4)) {
		json_string(out, (const uint8_t *)"invalid_ip", 10);
		return;
	}
	uint64_t off;
	if (mmdb_lookup(&x->r, ip, is4, &off, found) < 0) {
		char msg[600];
		snprintf(msg, sizeof msg, "lookup_error: error looking up '%.*s': %s", (int)n, ip_text, err_msg());
		json_string(out, (const uint8_t *)msg, strlen(msg));
		*found = true;
		return;
	}
	if (!*found) {
		buf_append(out, "null", 4);
		return;
	}
	dnode *v;
	uint64_t next;
	arena_reset(&x->a);
	if (mdec_value(&x->r.data, off, &x->a, &v, &next) < 0) {
		char msg[600];
		snprintf(msg, sizeof msg, "lookup_error: %s", err_msg());
		json_string(out, (const uint8_t *)msg, strlen(msg));
		return;
	}
	if (x->fields->n == 0) {
		jw_value(out, v, "", 1);
		return;
	}
	jw_object o = {0};
	buf_t one = {0};
	for (size_t i = 0; i < x->fields->n; i++) {
		one.len = 0;
		const dnode *f = extract_field(v, x->fields->v[i]);
		if (f)
			jw_value(&one, f, "", 1);
		else
			buf_append(&one, "null", 4);
		jw_add(&o, x->fields->v[i], strlen(x->fields->v[i]), &one);
	}
	buf_free(&one);
	/* the field map sits at depth 1 of the results */
	buf_t inner = {0};
	jw_write_object(&o, &inner);
	for (size_t i = 0; i < inner.len; i++) {
		buf_push(out, inner.p[i]);
		if (inner.p[i] == '\n')
			buf_append(out, "  ", 2);
	}
	buf_free(&inner);
}

static void write_results(command *c, jw_object *res)
{
	buf_t out = {0};
	jw_write_object(res, &out);
	const char *path = flag_get(c, "out")->str;
	if (*path) {
		if (write_file(path, out.p, out.len) < 0)
			go_fatalf("Failed to write output: %s", err_msg());
		printf("Results saved to %s\n", path);
	} else {
		fwrite(out.p, 1, out.len, stdout);
		fputc('\n', stdout);
	}
	buf_free(&out);
}

static void add_lookup(reader_ctx *x, jw_object *res, const char *ip, size_t n)
{
	buf_t v = {0};
	bool found;
	lookup_json(x, ip, n, &v, &found);
	jw_add(res, ip, n, &v);
	buf_free(&v);
}

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t' || **s == '\r' || **s == '\v' || **s == '\f')) {
		(*s)++;
		(*n)--;
	}
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r' || (*s)[*n - 1] == '\v' ||
		      (*s)[*n - 1] == '\f'))
		(*n)--;
}

static void read_all(const char *path, buf_t *all)
{
	instream *s = in_open(path);
	if (!s)
		go_fatalf("Failed to read IPs: %s", err_msg());
	uint8_t chunk[65536];
	for (;;) {
		ssize_t n = in_read(s, chunk, sizeof chunk);
		if (n < 0)
			go_fatalf("Failed to read IPs: %s", err_msg());
		if (n == 0)
			break;
		buf_append(all, chunk, (size_t)n);
	}
	in_close(s);
}

/* The Go mmdbio's three modes: --ip, --input, --range. */
static int classic(command *c, reader_ctx *x)
{
	jw_object res = {0};
	const char *input = flag_get(c, "input")->str;
	if (*input) {
		buf_t all = {0};
		read_all(input, &all);
		size_t i = 0;
		while (i < all.len) {
			size_t j = i;
			while (j < all.len && all.p[j] != '\n')
				j++;
			const char *line = (const char *)all.p + i;
			size_t n = j - i;
			trim(&line, &n);
			if (n)
				add_lookup(x, &res, line, n);
			i = j + 1;
		}
		buf_free(&all);
		write_results(c, &res);
		return 0;
	}

	const char *range = flag_get(c, "range")->str;
	if (*range) {
		u128 start, end;
		bool is4;
		if (!strchr(range, '/') || !parse_network(range, strlen(range), &start, &end, &is4))
			go_fatalf("Invalid CIDR range: invalid CIDR address: %s", range);
		for (u128 ip = start;; ip++) {
			char t[64];
			fmt_ip(ip, is4, t);
			add_lookup(x, &res, t, strlen(t));
			if (ip == end)
				break;
		}
		write_results(c, &res);
		return 0;
	}

	const char *ip = flag_get(c, "ip")->str;
	if (!*ip) {
		printf("Error: either --ip, --range, or --input is required\n");
		exit(1);
	}
	u128 addr;
	bool is4;
	if (!parse_ip_text(ip, strlen(ip), &addr, &is4))
		go_fatalf("Invalid IP address: %s", ip);
	uint64_t off;
	bool found;
	if (mmdb_lookup(&x->r, addr, is4, &off, &found) < 0)
		go_fatalf("Lookup error: error looking up '%s': %s", ip, err_msg());
	if (!found) {
		printf("No data found for that IP.\n");
		return 0;
	}
	if (x->fields->n > 0) {
		add_lookup(x, &res, ip, strlen(ip));
		write_results(c, &res);
		return 0;
	}
	dnode *v;
	uint64_t next;
	if (mdec_value(&x->r.data, off, &x->a, &v, &next) < 0)
		go_fatalf("Lookup error: %s", err_msg());
	buf_t out = {0};
	jw_value(&out, v, "", 0);
	fwrite(out.p, 1, out.len, stdout);
	fputc('\n', stdout);
	buf_free(&out);
	return 0;
}

/* ---- addresses from arguments ---- */

typedef enum { SRC_SPAN, SRC_FILE, SRC_STDIN } src_kind;

typedef struct {
	src_kind kind;
	u128 lo, hi;
	bool is4, single;
	const char *path;
} source;

typedef struct {
	source *v;
	size_t n;
	buf_t stdin_data;
	bool stdin_read;
} sources;

#define MAX_SPAN ((u128)1 << 32)

/* An IP, a CIDR, a range a-b or a,b. */
static bool parse_span(const char *s, size_t n, source *out, char *why, size_t whylen)
{
	char tmp[160];
	if (n == 0 || n >= sizeof tmp)
		return false;
	memcpy(tmp, s, n);
	tmp[n] = 0;
	char *comma = strchr(tmp, ',');
	if (comma)
		*comma = '-';
	*out = (source){.kind = SRC_SPAN};
	if (parse_ip_text(tmp, n, &out->lo, &out->is4)) {
		out->hi = out->lo;
		out->single = true;
		return true;
	}
	if (!parse_network(tmp, n, &out->lo, &out->hi, &out->is4) || out->hi < out->lo)
		return false;
	if (out->hi - out->lo >= MAX_SPAN) {
		snprintf(why, whylen, "%.*s has more than 4,294,967,296 addresses; use export --range for networks that big",
			 (int)n, s);
		return false;
	}
	return true;
}

static bool all_digits(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

static bool is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

static void add_source(sources *L, source s)
{
	L->v = xrealloc(L->v, (L->n + 1) * sizeof *L->v);
	L->v[L->n++] = s;
}

static void add_token(sources *L, const char *tok)
{
	char why[256] = "";
	source s;
	if (strcmp(tok, "-") == 0) {
		add_source(L, (source){.kind = SRC_STDIN});
		return;
	}
	if (!(all_digits(tok) && is_file(tok)) && parse_span(tok, strlen(tok), &s, why, sizeof why)) {
		add_source(L, s);
		return;
	}
	if (*why)
		go_fatalf("%s", why);
	if (is_file(tok)) {
		add_source(L, (source){.kind = SRC_FILE, .path = tok});
		return;
	}
	go_fatalf("Not an IP, CIDR, range or file: %s", tok);
}

typedef int (*addr_fn)(void *ctx, u128 ip, bool is4, const char *text, size_t tn, bool valid);

static int each_in_span(const source *s, addr_fn fn, void *ctx)
{
	for (u128 ip = s->lo;; ip++) {
		char t[64];
		fmt_ip(ip, s->is4, t);
		if (fn(ctx, ip, s->is4, t, strlen(t), true) < 0)
			return -1;
		if (ip == s->hi)
			return 0;
	}
}

static int each_in_text(const buf_t *all, addr_fn fn, void *ctx)
{
	size_t i = 0;
	while (i < all->len) {
		size_t j = i;
		while (j < all->len && all->p[j] != '\n')
			j++;
		const char *line = (const char *)all->p + i;
		size_t n = j - i;
		trim(&line, &n);
		i = j + 1;
		if (!n || line[0] == '#')
			continue;
		source s;
		char why[256] = "";
		if (!parse_span(line, n, &s, why, sizeof why)) {
			if (*why)
				go_fatalf("%s", why);
			if (fn(ctx, 0, false, line, n, false) < 0)
				return -1;
			continue;
		}
		if (each_in_span(&s, fn, ctx) < 0)
			return -1;
	}
	return 0;
}

static int each_address(sources *L, addr_fn fn, void *ctx)
{
	for (size_t i = 0; i < L->n; i++) {
		source *s = &L->v[i];
		if (s->kind == SRC_SPAN) {
			if (each_in_span(s, fn, ctx) < 0)
				return -1;
		} else if (s->kind == SRC_STDIN) {
			if (!L->stdin_read) {
				read_all("-", &L->stdin_data);
				L->stdin_read = true;
			}
			if (each_in_text(&L->stdin_data, fn, ctx) < 0)
				return -1;
		} else {
			buf_t all = {0};
			read_all(s->path, &all);
			int rc = each_in_text(&all, fn, ctx);
			buf_free(&all);
			if (rc < 0)
				return -1;
		}
	}
	return 0;
}

/* ---- the output ---- */

typedef struct {
	reader_ctx *x;
	out_format fmt;
	FILE *out;
	jw_object res;
	columns cols;
	ht64 seen;
	bool pass1;
	buf_t line;
	buf_t *cells;
	const char **cp;
	size_t *cl;
} emit;

/* the record at ip, or NULL (with a note on stderr for the JSON formats) */
static const dnode *lookup_rec(emit *e, u128 ip, bool is4, const char *text, size_t tn, uint64_t *off)
{
	bool found;
	if (mmdb_lookup(&e->x->r, ip, is4, off, &found) < 0) {
		fprintf(stderr, "err: looking up %.*s: %s\n", (int)tn, text, err_msg());
		return NULL;
	}
	if (!found) {
		if (!e->pass1 && (e->fmt == OUT_JSONL || e->fmt == OUT_JSON_PRETTY))
			fprintf(stderr, "err: no data for %.*s\n", (int)tn, text);
		return NULL;
	}
	dnode *v;
	uint64_t next;
	arena_reset(&e->x->a);
	if (mdec_value(&e->x->r.data, *off, &e->x->a, &v, &next) < 0) {
		fprintf(stderr, "err: decoding the record of %.*s: %s\n", (int)tn, text, err_msg());
		return NULL;
	}
	return v;
}

static int emit_one(void *ctx, u128 ip, bool is4, const char *text, size_t tn, bool valid)
{
	emit *e = ctx;
	if (e->fmt == OUT_JSON) {
		add_lookup(e->x, &e->res, text, tn);
		return 0;
	}
	if (!valid) {
		if (!e->pass1)
			fprintf(stderr, "err: invalid IP address %.*s\n", (int)tn, text);
		return 0;
	}
	uint64_t off;
	if (e->pass1) {
		uint32_t had;
		bool found;
		if (mmdb_lookup(&e->x->r, ip, is4, &off, &found) < 0 || !found || ht64_get_or_put(&e->seen, off, 0, &had))
			return 0;
	}
	const dnode *v = lookup_rec(e, ip, is4, text, tn, &off);
	if (!v)
		return 0;
	if (e->pass1) {
		columns_add_record(&e->cols, v);
		return 0;
	}
	char ipt[64];
	snprintf(ipt, sizeof ipt, "%.*s", (int)tn, text);
	if (e->fmt == OUT_JSONL || e->fmt == OUT_JSON_PRETTY) {
		e->line.len = 0;
		json_row(&e->line, v, "ip", ipt, e->x->fields, e->fmt == OUT_JSON_PRETTY, NULL);
		buf_push(&e->line, '\n');
		fwrite(e->line.p, 1, e->line.len, e->out);
		return 0;
	}
	size_t n = e->cols.n + 1;
	e->cp[0] = ipt;
	e->cl[0] = strlen(ipt);
	for (size_t i = 0; i < e->cols.n; i++) {
		e->cells[i].len = 0;
		const dnode *f = column_value(v, e->cols.names[i]);
		if (f)
			cell_text(f, &e->cells[i]);
		e->cp[i + 1] = e->cells[i].p ? (const char *)e->cells[i].p : "";
		e->cl[i + 1] = e->cells[i].len;
	}
	row_write(e->out, e->fmt == OUT_TSV ? '\t' : ',', e->cp, e->cl, n);
	return 0;
}

static int run_read(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str;
	int nsrc = argc;
	if (!*db && argc > 0)
		db = argv[--nsrc];
	if (!*db)
		missing_flag(c, "--db flag is required");
	out_format fmt;
	const char *fs = flag_get(c, "format")->str;
	if (!out_format_parse(fs, &fmt) || fmt == OUT_CIDR)
		go_fatalf("Invalid --format '%s': use json, jsonl, json-pretty, csv or tsv", fs);
	bool flatten = flag_get(c, "flatten")->b;

	reader_ctx x = {.fields = &flag_get(c, "fields")->list};
	arena_init(&x.a);
	if (mmdb_open(&x.r, db) < 0)
		go_fatalf("Failed to open MMDB file: %s", err_msg());

	if (nsrc == 0 && fmt == OUT_JSON) {
		int rc = classic(c, &x);
		arena_free(&x.a);
		mmdb_close(&x.r);
		return rc;
	}

	sources L = {0};
	const char *ip = flag_get(c, "ip")->str, *range = flag_get(c, "range")->str, *input = flag_get(c, "input")->str;
	if (*ip)
		add_token(&L, ip);
	if (*range)
		add_token(&L, range);
	if (*input)
		add_source(&L, strcmp(input, "-") == 0 ? (source){.kind = SRC_STDIN} : (source){.kind = SRC_FILE, .path = input});
	for (int i = 0; i < nsrc; i++)
		add_token(&L, argv[i]);
	if (L.n == 0) {
		printf("Error: nothing to look up: give IPs, CIDRs, ranges or files (or --ip, --range, --input)\n");
		exit(1);
	}

	/* a single IP in JSON: its record alone, as --ip prints it */
	if (fmt == OUT_JSON && L.n == 1 && L.v[0].kind == SRC_SPAN && L.v[0].single) {
		char t[64];
		fmt_ip(L.v[0].lo, L.v[0].is4, t);
		flag_get(c, "ip")->str = t;
		flag_get(c, "input")->str = "";
		flag_get(c, "range")->str = "";
		int rc = classic(c, &x);
		arena_free(&x.a);
		mmdb_close(&x.r);
		free(L.v);
		return rc;
	}

	emit e = {.x = &x, .fmt = fmt, .out = stdout};
	const char *outp = flag_get(c, "out")->str;
	if (fmt != OUT_JSON && *outp) {
		e.out = fopen(outp, "wb");
		if (!e.out)
			go_fatalf("Failed to write output: open %s: %s", outp, go_strerror(errno));
	}
	static char obuf[1 << 16];
	setvbuf(e.out, obuf, _IOFBF, sizeof obuf);
	columns_init(&e.cols, flatten);
	ht64_init(&e.seen);
	if (fmt == OUT_CSV || fmt == OUT_TSV) {
		if (x.fields->n) {
			for (size_t i = 0; i < x.fields->n; i++)
				columns_add(&e.cols, x.fields->v[i], strlen(x.fields->v[i]));
		} else {
			e.pass1 = true;
			each_address(&L, emit_one, &e);
			e.pass1 = false;
			columns_sort(&e.cols);
		}
		e.cells = xcalloc(e.cols.n + 1, sizeof *e.cells);
		e.cp = xcalloc(e.cols.n + 1, sizeof *e.cp);
		e.cl = xcalloc(e.cols.n + 1, sizeof *e.cl);
		if (!flag_get(c, "no-header")->b) {
			e.cp[0] = "ip";
			e.cl[0] = 2;
			for (size_t i = 0; i < e.cols.n; i++) {
				e.cp[i + 1] = e.cols.names[i];
				e.cl[i + 1] = strlen(e.cols.names[i]);
			}
			row_write(e.out, fmt == OUT_TSV ? '\t' : ',', e.cp, e.cl, e.cols.n + 1);
		}
	}
	each_address(&L, emit_one, &e);
	if (fmt == OUT_JSON) {
		write_results(c, &e.res);
	} else {
		if (fflush(e.out) != 0 || ferror(e.out))
			go_fatalf("Failed to write output: %s", go_strerror(errno));
		if (e.out != stdout) {
			fclose(e.out);
			printf("Results saved to %s\n", outp);
		}
	}
	for (size_t i = 0; e.cells && i < e.cols.n + 1; i++)
		buf_free(&e.cells[i]);
	free(e.cells);
	free(e.cp);
	free(e.cl);
	buf_free(&e.line);
	columns_free(&e.cols);
	ht64_free(&e.seen);
	buf_free(&L.stdin_data);
	free(L.v);
	arena_free(&x.a);
	mmdb_close(&x.r);
	return 0;
}
