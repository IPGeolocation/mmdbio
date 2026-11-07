/* export: records to JSON, JSON Lines, CSV or TSV, or the matching networks
 * as a CIDR list. Networks are streamed in address order, so memory stays
 * flat on any database size; records shared by many networks are decoded,
 * matched and rendered once. */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cmds.h"
#include "compat.h"
#include "hashtab.h"
#include "jsonw.h"
#include "mmdb_enc.h"
#include "netaddr.h"
#include "tabular.h"

static flag export_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the first argument)"},
	{.name = "out", .shorthand = 'o', .type = F_STRING,
	 .usage = "Path to output file, or '-' for stdout (or the second argument; default stdout)"},
	{.name = "fields",
	 .type = F_LIST,
	 .usage = "Comma-separated list of fields to extract (e.g. location.country.name,city.names.en)"},
	{.name = "range", .type = F_STRING, .usage = "Optional comma-separated CIDR ranges to filter networks"},
	{.name = "where",
	 .type = F_ARRAY,
	 .usage = "Only export networks whose field matches, e.g. is_vpn=true, location.country.code2=DE,FR or "
		  "threat_score>=80 (repeatable, all must match)"},
	{.name = "format",
	 .shorthand = 'f',
	 .type = F_STRING,
	 .def = "json",
	 .usage = "Output format: json (one object keyed by network), jsonl (a line per network, with \"range\"), csv, "
		  "tsv, or cidr (one network per line); default: from the --out extension, else json"},
	{.name = "no-header", .type = F_BOOL, .usage = "CSV/TSV without the header row"},
	{.name = "flatten",
	 .type = F_BOOL,
	 .usage = "CSV/TSV: a column per nested field (location.city.name) instead of JSON in one cell"},
	{.name = "family", .type = F_INT, .def = "0", .usage = "Only export IPv4 (4) or IPv6 (6) networks"},
	{.name = "aggregate",
	 .type = F_BOOL,
	 .def = "true",
	 .usage = "With --format cidr, merge adjacent networks into the fewest CIDRs (--aggregate=false keeps networks "
		  "as stored)"},
};

static int run_export(command *c, int argc, char **argv);

command cmd_export = {
	.name = "export",
	.use = "export [flags] [db.mmdb] [output]",
	.short_desc = "Export records from an MMDB to JSON, JSON Lines, CSV, TSV or a plain CIDR list (supports field, value, "
		      "and range filtering)",
	.examples = "  mmdbio export city.mmdb city.csv\n"
		    "  mmdbio export --db city.mmdb --out - -f jsonl --where location.country.iso_code=DE\n"
		    "  mmdbio export city.mmdb -f tsv --flatten --fields location.city.name,location.country.iso_code\n"
		    "  mmdbio export --db proxy.mmdb --where is_vpn=true --format cidr --out vpn.txt",
	.flags = export_flags,
	.nflags = (int)(sizeof export_flags / sizeof export_flags[0]),
	.run = run_export,
};

/* ---- --where ---- */

typedef struct {
	char *path;
	char op[3];
	strlist values; /* = and != */
	double number;  /* > >= < <= */
} condition;

static char *dup_trim(const char *s, size_t n)
{
	while (n && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
		s++;
		n--;
	}
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
		n--;
	char *p = xmalloc(n + 1);
	if (n)
		memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static bool parse_condition(const char *s, condition *c, char *why, size_t whylen)
{
	memset(c, 0, sizeof *c);
	size_t i = strcspn(s, "=!<>");
	if (s[i] == 0 || i == 0) {
		snprintf(why, whylen, "expected field<op>value, e.g. is_vpn=true or threat_score>=80");
		return false;
	}
	static const char *const ops[] = {">=", "<=", "!=", "=", ">", "<"};
	const char *rest = s + i;
	for (int k = 0; k < 6; k++) {
		size_t ol = strlen(ops[k]);
		if (strncmp(rest, ops[k], ol) != 0)
			continue;
		char *value = dup_trim(rest + ol, strlen(rest + ol));
		if (!*value) {
			snprintf(why, whylen, "missing value after '%s'", ops[k]);
			free(value);
			return false;
		}
		c->path = dup_trim(s, i);
		memcpy(c->op, ops[k], ol + 1);
		if (k == 2 || k == 3) {
			const char *p = value;
			for (;;) {
				const char *comma = strchr(p, ',');
				size_t n = comma ? (size_t)(comma - p) : strlen(p);
				char *item = dup_trim(p, n);
				strlist_push(&c->values, item);
				free(item);
				if (!comma)
					break;
				p = comma + 1;
			}
			free(value);
			return true;
		}
		char *end;
		c->number = strtod(value, &end);
		if (*end || end == value) {
			snprintf(why, whylen, "'%s' needs a number, got '%s'", ops[k], value);
			free(value);
			free(c->path);
			return false;
		}
		free(value);
		return true;
	}
	snprintf(why, whylen, "unknown operator in '%s'", s);
	return false;
}

static bool value_in(const dnode *v, const strlist *values)
{
	if (v->kind == T_ARRAY) {
		for (uint32_t i = 0; i < v->n; i++)
			if (value_in(&v->items[i], values))
				return true;
		return false;
	}
	buf_t s = {0};
	go_sprint(v, &s);
	bool hit = false;
	for (size_t i = 0; i < values->n && !hit; i++)
		hit = strlen(values->v[i]) == s.len && (s.len == 0 || strncasecmp((const char *)s.p, values->v[i], s.len) == 0);
	buf_free(&s);
	return hit;
}

static bool to_number(const dnode *v, double *out)
{
	switch (v->kind) {
	case T_DOUBLE:
	case T_FLOAT:
		*out = v->f;
		return true;
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
		*out = (double)v->u;
		return true;
	case T_INT32:
		*out = (double)(int64_t)v->u;
		return true;
	case T_UINT128:
		*out = ldexp((double)v->u_hi, 64) + (double)v->u;
		return true;
	case T_STRING: {
		char *t = dup_trim((const char *)v->s, v->n);
		char *end;
		*out = strtod(t, &end);
		bool ok = *t && !*end;
		free(t);
		return ok;
	}
	}
	return false;
}

static bool matches_all(const dnode *rec, const condition *conds, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const condition *c = &conds[i];
		const dnode *v = extract_field(rec, c->path);
		if (!v)
			return false;
		if (strcmp(c->op, "=") == 0) {
			if (!value_in(v, &c->values))
				return false;
		} else if (strcmp(c->op, "!=") == 0) {
			if (value_in(v, &c->values))
				return false;
		} else {
			double x;
			if (!to_number(v, &x))
				return false;
			bool ok = c->op[0] == '>' ? (c->op[1] ? x >= c->number : x > c->number)
						  : (c->op[1] ? x <= c->number : x < c->number);
			if (!ok)
				return false;
		}
	}
	return true;
}

/* ---- networks ---- */

typedef struct {
	u128 start, end;
	bool is4;
} filter_range;

static bool network_is4(const mmdb_reader *r, u128 ip, int bit)
{
	return r->ip_version == 4 || ((ip >> 32) == 0 && bit >= 96);
}

/* the CIDR aggregation of one family: the open range and where lines go */
typedef struct {
	bool open;
	u128 lo, hi;
	FILE *out;
	uint64_t cidrs;
} agg;

static void put_cidr(void *ctx, u128 ip, int bits)
{
	agg *a = ctx;
	char n[80];
	fprintf(a->out, "%s\n", fmt_net(ip, bits, n));
	a->cidrs++;
}

static void agg_flush(agg *a)
{
	if (a->open)
		range_cidrs(a->lo, a->hi, put_cidr, a);
	a->open = false;
}

static void agg_add(agg *a, u128 lo, u128 hi)
{
	if (a->open && a->hi != U128_MAX && a->hi + 1 == lo) {
		a->hi = hi;
		return;
	}
	agg_flush(a);
	a->open = true;
	a->lo = lo;
	a->hi = hi;
}

typedef struct {
	bool matches;
	char *text; /* the rendered record (json, jsonl, csv/tsv cells) */
	size_t len, at; /* jsonl: where the range goes */
} cached;

typedef struct {
	mmdb_reader r;
	out_format fmt;
	strlist *fields;
	condition *conds;
	size_t nconds;
	filter_range *ranges;
	size_t nranges;
	long long family;
	columns cols;
	char delim;
	/* the record cache */
	ht64 cache_ix;
	cached *cache;
	size_t ncache, cache_bytes;
	arena ar;
	buf_t js, one;
	buf_t *cells;
} exporter;

static bool wanted(const exporter *x, u128 ip, int bit, bool is4)
{
	u128 lo = ip, hi = net_last(ip, bit);
	if (x->nranges) {
		bool in = false;
		for (size_t i = 0; i < x->nranges && !in; i++)
			in = x->ranges[i].is4 == is4 && !(hi < x->ranges[i].start) && !(x->ranges[i].end < lo);
		if (!in)
			return false;
	}
	return !x->family || (is4 ? 4 : 6) == x->family;
}

static void render(exporter *x, const dnode *v, cached *ce)
{
	buf_t *js = &x->js, *one = &x->one;
	js->len = 0;
	switch (x->fmt) {
	case OUT_JSON:
		if (x->fields->n == 0) {
			jw_value(js, v, "", 1);
		} else {
			jw_object o = {0};
			for (size_t i = 0; i < x->fields->n; i++) {
				one->len = 0;
				const dnode *f = extract_field(v, x->fields->v[i]);
				if (f)
					jw_value(one, f, "", 1);
				else
					buf_append(one, "null", 4);
				jw_add(&o, x->fields->v[i], strlen(x->fields->v[i]), one);
			}
			one->len = 0;
			jw_write_object(&o, one);
			for (size_t i = 0; i < one->len; i++) {
				buf_push(js, one->p[i]);
				if (one->p[i] == '\n')
					buf_append(js, "  ", 2);
			}
		}
		break;
	case OUT_JSONL:
		/* the network's place among the sorted keys, filled per network */
		json_row(js, v, "range", NULL, x->fields, false, &ce->at);
		break;
	case OUT_CSV:
	case OUT_TSV: {
		/* every cell after the range, each with its delimiter before it */
		size_t n = x->cols.n;
		const char **cp = xcalloc(n + 1, sizeof *cp);
		size_t *cl = xcalloc(n + 1, sizeof *cl);
		cp[0] = "";
		for (size_t i = 0; i < n; i++) {
			x->cells[i].len = 0;
			const dnode *f = column_value(v, x->cols.names[i]);
			if (f)
				cell_text(f, &x->cells[i]);
			cp[i + 1] = x->cells[i].p ? (const char *)x->cells[i].p : "";
			cl[i + 1] = x->cells[i].len;
		}
		js->len = 0;
		row_append(js, x->delim, cp, cl, n + 1);
		free(cp);
		free(cl);
		break;
	}
	default:
		break;
	}
	ce->text = xmalloc(js->len + 1);
	if (js->len)
		memcpy(ce->text, js->p, js->len);
	ce->len = js->len;
}

/* The cached entry of the record at off (decoded, matched and rendered the
 * first time), or NULL when it can't be decoded. */
static cached *entry(exporter *x, uint64_t off, u128 ip, int bit, bool collect)
{
	uint32_t slot;
	if (ht64_get(&x->cache_ix, off, &slot))
		return &x->cache[slot];
	if (x->ncache >= (1u << 20) || x->cache_bytes >= (256u << 20)) {
		for (size_t i = 0; i < x->ncache; i++)
			free(x->cache[i].text);
		x->ncache = x->cache_bytes = 0;
		ht64_free(&x->cache_ix);
		ht64_init(&x->cache_ix);
	}
	dnode *v;
	uint64_t next;
	arena_reset(&x->ar);
	if (mdec_value(&x->r.data, off, &x->ar, &v, &next) < 0) {
		char n[80];
		go_logf("Warning: failed to decode network %s: %s", fmt_net(ip, bit, n), err_msg());
		return NULL;
	}
	x->cache = xrealloc(x->cache, (x->ncache + 1) * sizeof *x->cache);
	cached *ce = &x->cache[x->ncache];
	memset(ce, 0, sizeof *ce);
	ce->matches = matches_all(v, x->conds, x->nconds);
	if (ce->matches) {
		if (collect)
			columns_add_record(&x->cols, v);
		else if (x->fmt != OUT_CIDR)
			render(x, v, ce);
		x->cache_bytes += ce->len;
	}
	uint32_t dummy;
	ht64_get_or_put(&x->cache_ix, off, (uint32_t)x->ncache, &dummy);
	x->ncache++;
	return ce;
}

static void cache_clear(exporter *x)
{
	for (size_t i = 0; i < x->ncache; i++)
		free(x->cache[i].text);
	x->ncache = x->cache_bytes = 0;
	ht64_free(&x->cache_ix);
	ht64_init(&x->cache_ix);
}

static int run_export(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str, *outp = flag_get(c, "out")->str;
	int pos = 0;
	if (!*db && pos < argc)
		db = argv[pos++];
	if (!*outp && pos < argc)
		outp = argv[pos++];
	if (pos < argc)
		go_fatalf("Unexpected argument: %s", argv[pos]);
	if (!*db)
		missing_flag(c, "--db is required (or the database as the first argument)");
	if (!*outp)
		outp = "-";
	exporter x;
	memset(&x, 0, sizeof x);
	flag *ff = flag_get(c, "format");
	if (!out_format_parse(ff->str, &x.fmt) || x.fmt == OUT_JSON_PRETTY)
		go_fatalf("Invalid --format '%s': use json, jsonl, csv, tsv or cidr", ff->str);
	if (!ff->set && strcmp(outp, "-") != 0)
		out_format_from_path(outp, &x.fmt);
	bool cidr = x.fmt == OUT_CIDR;
	x.fields = &flag_get(c, "fields")->list;
	if (cidr && x.fields->n > 0)
		go_fatalf("--fields cannot be used with --format cidr");
	x.family = flag_get(c, "family")->i;
	if (x.family != 0 && x.family != 4 && x.family != 6)
		go_fatalf("Invalid --family %lld: use 4 or 6", x.family);
	bool aggregate = flag_get(c, "aggregate")->b;
	x.delim = x.fmt == OUT_TSV ? '\t' : ',';

	strlist *where = &flag_get(c, "where")->list;
	x.nconds = where->n;
	x.conds = xcalloc(where->n + 1, sizeof *x.conds);
	for (size_t i = 0; i < where->n; i++) {
		char why[512];
		if (!parse_condition(where->v[i], &x.conds[i], why, sizeof why))
			go_fatalf("Invalid --where '%s': %s", where->v[i], why);
	}

	if (mmdb_open(&x.r, db) < 0)
		go_fatalf("Failed to open MMDB: %s", err_msg());

	const char *rs = flag_get(c, "range")->str;
	if (*rs) {
		const char *p = rs;
		for (;;) {
			const char *comma = strchr(p, ',');
			size_t n = comma ? (size_t)(comma - p) : strlen(p);
			char *one = dup_trim(p, n);
			x.ranges = xrealloc(x.ranges, (x.nranges + 1) * sizeof *x.ranges);
			filter_range *f = &x.ranges[x.nranges++];
			if (!strchr(one, '/') || !parse_network(one, strlen(one), &f->start, &f->end, &f->is4)) {
				char raw[256];
				snprintf(raw, sizeof raw, "%.*s", (int)(n < 200 ? n : 200), p);
				go_fatalf("Invalid range '%s': invalid CIDR address: %s", raw, one);
			}
			free(one);
			if (!comma)
				break;
			p = comma + 1;
		}
	}

	ht64_init(&x.cache_ix);
	arena_init(&x.ar);
	columns_init(&x.cols, flag_get(c, "flatten")->b);
	bool tab = x.fmt == OUT_CSV || x.fmt == OUT_TSV;
	net_iter it;
	u128 ip;
	int bit;
	uint64_t off;
	int rc;
	if (tab) {
		if (x.fields->n) {
			for (size_t i = 0; i < x.fields->n; i++)
				columns_add(&x.cols, x.fields->v[i], strlen(x.fields->v[i]));
		} else {
			/* the columns: every field of the records that will be exported */
			net_iter_init(&it, &x.r);
			while ((rc = net_iter_next(&it, &ip, &bit, &off)) == 1) {
				bool is4 = network_is4(&x.r, ip, bit);
				if (wanted(&x, ip, bit, is4))
					entry(&x, off, ip, bit, true);
			}
			if (rc < 0)
				go_fatalf("Error iterating networks: %s", err_msg());
			net_iter_free(&it);
			cache_clear(&x);
			columns_sort(&x.cols);
		}
		x.cells = xcalloc(x.cols.n + 1, sizeof *x.cells);
	}

	FILE *out = stdout;
	bool to_stdout = strcmp(outp, "-") == 0;
	if (!to_stdout) {
		out = fopen(outp, "wb");
		if (!out)
			go_fatalf("Failed to create output file: open %s: %s", outp, go_strerror(errno));
	}
	static char obuf[1 << 20];
	setvbuf(out, obuf, _IOFBF, sizeof obuf);

	agg a4 = {.out = out}, a6 = {.out = NULL};
	if (cidr && aggregate) {
		a6.out = temp_stream();
		if (!a6.out)
			go_fatalf("Failed to aggregate networks: tmpfile: %s", go_strerror(errno));
	}
	if (tab && !flag_get(c, "no-header")->b) {
		const char **cp = xcalloc(x.cols.n + 1, sizeof *cp);
		size_t *cl = xcalloc(x.cols.n + 1, sizeof *cl);
		cp[0] = "range";
		cl[0] = 5;
		for (size_t i = 0; i < x.cols.n; i++) {
			cp[i + 1] = x.cols.names[i];
			cl[i + 1] = strlen(x.cols.names[i]);
		}
		row_write(out, x.delim, cp, cl, x.cols.n + 1);
		free(cp);
		free(cl);
	}

	uint64_t matched = 0, records = 0;
	bool first = true;
	if (x.fmt == OUT_JSON)
		fputc('{', out);
	net_iter_init(&it, &x.r);
	while ((rc = net_iter_next(&it, &ip, &bit, &off)) == 1) {
		bool is4 = network_is4(&x.r, ip, bit);
		if (!wanted(&x, ip, bit, is4))
			continue;
		cached *ce = entry(&x, off, ip, bit, false);
		if (!ce || !ce->matches)
			continue;
		matched++;
		u128 lo = ip, hi = net_last(ip, bit);
		char n[80];
		switch (x.fmt) {
		case OUT_CIDR:
			if (aggregate)
				agg_add(is4 ? &a4 : &a6, lo, hi);
			else
				fprintf(out, "%s\n", fmt_net(ip, bit, n));
			break;
		case OUT_JSON:
			fmt_net(ip, bit, n);
			fputs(first ? "\n  " : ",\n  ", out);
			first = false;
			x.js.len = 0;
			json_string(&x.js, (const uint8_t *)n, strlen(n));
			fwrite(x.js.p, 1, x.js.len, out);
			fputs(": ", out);
			fwrite(ce->text, 1, ce->len, out);
			records++;
			break;
		case OUT_JSONL:
			fmt_net(ip, bit, n);
			x.js.len = 0;
			json_string(&x.js, (const uint8_t *)n, strlen(n));
			fwrite(ce->text, 1, ce->at, out);
			fwrite(x.js.p, 1, x.js.len, out);
			fwrite(ce->text + ce->at, 1, ce->len - ce->at, out);
			fputc('\n', out);
			records++;
			break;
		default: /* csv, tsv */
			fmt_net(ip, bit, n);
			fputs(n, out);
			fwrite(ce->text, 1, ce->len, out);
			records++;
		}
	}
	if (rc < 0)
		go_fatalf("Error iterating networks: %s", err_msg());

	const char *dest = to_stdout ? "stdout" : outp;
	char summary[600];
	if (x.fmt == OUT_JSON) {
		fputs(first ? "}\n" : "\n}\n", out);
		snprintf(summary, sizeof summary, "Exported %llu records to %s", (unsigned long long)records, dest);
	} else if (!cidr) {
		snprintf(summary, sizeof summary, "Exported %llu records to %s", (unsigned long long)records, dest);
	} else if (aggregate) {
		agg_flush(&a4);
		agg_flush(&a6);
		rewind(a6.out);
		char chunk[65536];
		size_t got;
		while ((got = fread(chunk, 1, sizeof chunk, a6.out)) > 0)
			fwrite(chunk, 1, got, out);
		fclose(a6.out);
		snprintf(summary, sizeof summary, "Exported %llu networks as %llu CIDRs to %s", (unsigned long long)matched,
			 (unsigned long long)(a4.cidrs + a6.cidrs), dest);
	} else {
		snprintf(summary, sizeof summary, "Exported %llu CIDRs to %s", (unsigned long long)matched, dest);
	}
	if (fflush(out) != 0 || ferror(out))
		go_fatalf("Failed to write output: %s", go_strerror(errno));
	if (!to_stdout)
		fclose(out);
	if (to_stdout)
		fprintf(stderr, "%s\n", summary);
	else
		printf("%s\n", summary);

	net_iter_free(&it);
	cache_clear(&x);
	free(x.cache);
	ht64_free(&x.cache_ix);
	arena_free(&x.ar);
	buf_free(&x.js);
	buf_free(&x.one);
	for (size_t i = 0; x.cells && i < x.cols.n + 1; i++)
		buf_free(&x.cells[i]);
	free(x.cells);
	columns_free(&x.cols);
	for (size_t i = 0; i < where->n; i++) {
		free(x.conds[i].path);
		strlist_free(&x.conds[i].values);
	}
	free(x.conds);
	free(x.ranges);
	mmdb_close(&x.r);
	return 0;
}
