/* metadata, stats, inspect and verify. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cmds.h"
#include "compat.h"
#include "jsonw.h"
#include "mmdb_enc.h"
#include "netaddr.h"

static void open_or_die(mmdb_reader *r, const char *path, const char *what)
{
	if (mmdb_open(r, path) < 0)
		go_fatalf("%s: %s", what, err_msg());
}

/* fmt's %v of a []string: [a b c] */
static void print_languages(const mmdb_reader *r)
{
	printf("[");
	for (uint32_t i = 0; i < r->nlanguages; i++)
		printf(i ? " %.*s" : "%.*s", (int)r->languages[i].n, (const char *)r->languages[i].s);
	printf("]");
}

static dnode description_map(const mmdb_reader *r)
{
	return (dnode){.kind = T_MAP, .n = r->ndescription, .items = r->description};
}

/* ---- metadata ---- */

static flag metadata_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the first argument)"},
	{.name = "layout", .shorthand = 'l', .type = F_BOOL,
	 .usage = "Also show the format version, and the size and offset of each section"},
	{.name = "data-types", .type = F_BOOL,
	 .usage = "Also break the data section down by value type (reads the whole section)"},
	{.name = "json", .type = F_BOOL, .usage = "Output in JSON format: every metadata key, and the layout"},
	{.name = "format", .shorthand = 'f', .type = F_STRING, .def = "text", .usage = "text (or pretty, mmdbctl's name), or json (as --json)"},
};

/* ---- the data section by type ---- */

typedef struct {
	uint64_t values[16], bytes[16], entries[16];
} type_stats;

static const char *const TYPE_NAMES[16] = {
	"extended", "pointer", "string", "double", "bytes", "uint16", "uint32", "map",
	"int32",    "uint64",  "uint128", "array", "container", "end_marker", "boolean", "float",
};

/* Counts the value at off and everything in it; *end is just past it. */
static int count_value(const mdec *d, uint64_t off, type_stats *st, int depth, uint64_t *end)
{
	if (depth > 512 || off >= d->len) {
		err_set("invalid data section at offset %llu", (unsigned long long)off);
		return -1;
	}
	uint64_t p = off;
	uint8_t ctrl = d->buf[p++];
	int type = ctrl >> 5;
	if (type == T_POINTER) {
		uint64_t n = 2 + ((ctrl >> 3) & 3);
		st->values[T_POINTER]++;
		st->bytes[T_POINTER] += n;
		*end = off + n;
		return *end <= d->len ? 0 : -1;
	}
	if (type == T_EXTENDED) {
		if (p >= d->len)
			return -1;
		type = 7 + d->buf[p++];
		if (type < 8 || type > 15) {
			err_set("invalid data section: unknown type at offset %llu", (unsigned long long)off);
			return -1;
		}
	}
	uint64_t size = ctrl & 0x1f;
	if (size >= 29) {
		int nb = (int)size - 28;
		if (p + (uint64_t)nb > d->len)
			return -1;
		uint64_t v = 0;
		for (int i = 0; i < nb; i++)
			v = v << 8 | d->buf[p++];
		size = size == 29 ? 29 + v : size == 30 ? 285 + v : 65821 + v;
	}
	st->values[type]++;
	st->bytes[type] += p - off;
	if (type == T_MAP || type == T_ARRAY) {
		st->entries[type] += size;
		for (uint64_t i = 0; i < (type == T_MAP ? 2 * size : size); i++)
			if (count_value(d, p, st, depth + 1, &p) < 0)
				return -1;
	} else if (type != T_BOOL) {
		if (size > d->len - p) {
			err_set("invalid data section at offset %llu", (unsigned long long)off);
			return -1;
		}
		st->bytes[type] += size;
		p += size;
	}
	*end = p;
	return 0;
}

static int count_types(const mmdb_reader *r, type_stats *st)
{
	memset(st, 0, sizeof *st);
	for (uint64_t off = 0; off < r->data.len;)
		if (count_value(&r->data, off, st, 0, &off) < 0)
			return -1;
	return 0;
}

static void human(uint64_t n, char out[32])
{
	static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
	double v = (double)n;
	int u = 0;
	while (v >= 1000 && u < 4) {
		v /= 1000;
		u++;
	}
	if (u == 0)
		snprintf(out, 32, "%llu B", (unsigned long long)n);
	else
		snprintf(out, 32, "%.2f %s", v, units[u]);
}

static int run_metadata(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str;
	if (!*db && argc > 0)
		db = argv[0];
	if (!*db)
		missing_flag(c, "--db flag is required");
	mmdb_reader r;
	open_or_die(&r, db, "Failed to open MMDB");
	bool types = flag_get(c, "data-types")->b, layout = flag_get(c, "layout")->b || types;
	type_stats ts;
	if (types && count_types(&r, &ts) < 0)
		go_fatalf("Failed to read the data section: %s", err_msg());
	uint64_t data_start = r.tree_size + 16, data_end = data_start + r.data.len;
	uint64_t meta_start = (uint64_t)(r.meta.buf - r.map);
	const char *mfmt = flag_get(c, "format")->str;
	if (strcmp(mfmt, "pretty") == 0) /* mmdbctl's name for text */
		mfmt = "text";
	if (strcmp(mfmt, "text") != 0 && strcmp(mfmt, "json") != 0)
		go_fatalf("Invalid --format '%s': use text or json", mfmt);
	if (flag_get(c, "json")->b || strcmp(mfmt, "json") == 0) {
		jw_object o = {0};
		buf_t v = {0};
		const dnode *m = r.meta_root;
		for (uint32_t i = 0; i < m->n; i++) {
			v.len = 0;
			jw_value(&v, &m->items[2 * i + 1], "", 1);
			jw_add(&o, (const char *)m->items[2 * i].s, m->items[2 * i].n, &v);
		}
		v.len = 0;
		buf_printf(&v,
			   "{\n    \"data_section_end\": %llu,\n    \"data_section_size\": %llu,\n"
			   "    \"data_section_start\": %llu,\n    \"file_size\": %llu,\n    \"metadata_size\": %llu,\n"
			   "    \"metadata_start\": %llu,\n    \"tree_size\": %llu\n  }",
			   (unsigned long long)data_end, (unsigned long long)r.data.len, (unsigned long long)data_start,
			   (unsigned long long)r.size, (unsigned long long)r.meta.len, (unsigned long long)meta_start,
			   (unsigned long long)r.tree_size);
		jw_add(&o, "layout", 6, &v);
		if (types) {
			jw_object t = {0};
			buf_t one = {0};
			for (int k = 1; k < 16; k++) {
				if (!ts.values[k])
					continue;
				one.len = 0;
				buf_printf(&one, "{\n    \"bytes\": %llu,\n", (unsigned long long)ts.bytes[k]);
				if (k == T_MAP || k == T_ARRAY)
					buf_printf(&one, "    \"%s\": %llu,\n", k == T_MAP ? "pairs" : "elements",
						   (unsigned long long)ts.entries[k]);
				buf_printf(&one, "    \"values\": %llu\n  }", (unsigned long long)ts.values[k]);
				jw_add(&t, TYPE_NAMES[k], strlen(TYPE_NAMES[k]), &one);
			}
			one.len = 0;
			jw_write_object(&t, &one);
			v.len = 0;
			for (size_t i = 0; i < one.len; i++) {
				buf_push(&v, one.p[i]);
				if (one.p[i] == '\n')
					buf_append(&v, "  ", 2);
			}
			jw_add(&o, "data_types", 10, &v);
			buf_free(&one);
		}
		buf_t out = {0};
		jw_write_object(&o, &out);
		printf("%.*s\n", (int)out.len, (const char *)out.p);
		buf_free(&out);
		buf_free(&v);
		mmdb_close(&r);
		return 0;
	}
	time_t t = (time_t)r.build_epoch;
	struct tm tm;
	localtime_r(&t, &tm);
	char when[64];
	strftime(when, sizeof when, "%a, %d %b %Y %H:%M:%S %Z", &tm); /* time.RFC1123 */
	printf("MMDB Metadata\n");
	printf("-----------------------------\n");
	printf("Database Type: %.*s\n", (int)r.database_type_len, (const char *)r.database_type);
	printf("IP Version:    %llu\n", (unsigned long long)r.ip_version);
	printf("Record Size:   %llu bits\n", (unsigned long long)r.record_size);
	printf("Node Count:    %llu\n", (unsigned long long)r.node_count);
	printf("Build Date:    %s\n", when);
	printf("Languages:     ");
	print_languages(&r);
	printf("\n");
	if (r.ndescription > 0) {
		dnode d = description_map(&r);
		buf_t out = {0};
		jw_value(&out, &d, "  ", 0);
		printf("Description: %.*s\n", (int)out.len, (const char *)out.p);
		buf_free(&out);
	}
	if (layout) {
		char a[32], b[32], h[32];
		printf("Format:        %llu.%llu\n", (unsigned long long)r.major, (unsigned long long)r.minor);
		human(r.size, h);
		printf("File Size:     %s bytes (%s)\n", fmt_int(r.size, a), h);
		human(r.tree_size, h);
		printf("Tree:          %s bytes (%s) at 0\n", fmt_int(r.tree_size, a), h);
		human(r.data.len, h);
		printf("Data Section:  %s bytes (%s) at %s\n", fmt_int(r.data.len, a), h, fmt_int(data_start, b));
		printf("Metadata:      %s bytes at %s\n", fmt_int(r.meta.len, a), fmt_int(meta_start, b));
	}
	if (types) {
		char a[32], b[32], e[32];
		printf("Data Types:\n  %-11s %15s %17s %15s\n", "type", "values", "bytes", "pairs/elements");
		for (int k = 1; k < 16; k++) {
			if (!ts.values[k])
				continue;
			printf("  %-11s %15s %17s %15s\n", TYPE_NAMES[k], fmt_int(ts.values[k], a), fmt_int(ts.bytes[k], b),
			       k == T_MAP || k == T_ARRAY ? fmt_int(ts.entries[k], e) : "");
		}
	}
	mmdb_close(&r);
	return 0;
}

command cmd_metadata = {
	.name = "metadata",
	.use = "metadata [flags] [db.mmdb]",
	.short_desc = "Show metadata information from an MMDB file",
	.flags = metadata_flags,
	.nflags = (int)(sizeof metadata_flags / sizeof metadata_flags[0]),
	.run = run_metadata,
};

/* ---- stats ---- */

static flag stats_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the first argument)"},
	{.name = "json", .type = F_BOOL, .usage = "Output in JSON format"},
};

static int run_stats(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str;
	if (!*db && argc > 0)
		db = argv[0];
	if (!*db)
		missing_flag(c, "--db flag is required");
	mmdb_reader r;
	open_or_die(&r, db, "Failed to open MMDB file");
	uint64_t v4 = 0, v6 = 0;
	net_iter it;
	net_iter_init(&it, &r);
	u128 ip;
	int bit;
	uint64_t off;
	int rc;
	while ((rc = net_iter_next(&it, &ip, &bit, &off)) == 1) {
		if ((ip >> 32) == 0 && bit >= 96)
			v4++;
		else
			v6++;
	}
	if (rc < 0)
		printf("Error during scan: %s\n", err_msg());
	net_iter_free(&it);
	time_t t = (time_t)r.build_epoch;
	struct tm tm;
	gmtime_r(&t, &tm);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm); /* time.RFC3339 in UTC */

	if (flag_get(c, "json")->b) {
		buf_t out = {0};
		buf_t s = {0};
		jw_object o = {0};
#define ADD(k, ...)                                \
	do {                                       \
		s.len = 0;                         \
		buf_printf(&s, __VA_ARGS__);       \
		jw_add(&o, k, strlen(k), &s);      \
	} while (0)
		s.len = 0;
		json_string(&s, (const uint8_t *)when, strlen(when));
		jw_add(&o, "build_epoch", 11, &s);
		s.len = 0;
		json_string(&s, r.database_type, r.database_type_len);
		jw_add(&o, "database_type", 13, &s);
		s.len = 0;
		dnode d = description_map(&r);
		jw_value(&s, &d, "", 1);
		jw_add(&o, "description", 11, &s);
		ADD("ip_version", "%llu", (unsigned long long)r.ip_version);
		ADD("ipv4_count", "%llu", (unsigned long long)v4);
		ADD("ipv6_count", "%llu", (unsigned long long)v6);
		s.len = 0;
		dnode langs = {.kind = T_ARRAY, .n = r.nlanguages, .items = r.languages};
		jw_value(&s, &langs, "", 1);
		jw_add(&o, "languages", 9, &s);
		ADD("networks", "%llu", (unsigned long long)(v4 + v6));
		ADD("node_count", "%llu", (unsigned long long)r.node_count);
		ADD("record_size", "%llu", (unsigned long long)r.record_size);
#undef ADD
		jw_write_object(&o, &out);
		printf("%.*s\n", (int)out.len, (const char *)out.p);
		buf_free(&out);
		buf_free(&s);
		mmdb_close(&r);
		return 0;
	}
	static const char rule[] = "────────────────────────────────────────────";
	printf("=====MMDB Statistics========\n%s\n", rule);
	printf("Database Type: %.*s\n", (int)r.database_type_len, (const char *)r.database_type);
	for (uint32_t i = 0; i < r.ndescription; i++) {
		const dnode *k = &r.description[2 * i], *v = &r.description[2 * i + 1];
		if (k->n == 2 && memcmp(k->s, "en", 2) == 0)
			printf("Description:   %.*s\n", (int)v->n, (const char *)v->s);
	}
	printf("IP Version:    %llu\n", (unsigned long long)r.ip_version);
	printf("Record Size:   %llu bits\n", (unsigned long long)r.record_size);
	printf("Node Count:    %llu\n", (unsigned long long)r.node_count);
	printf("Build Epoch:   %s\n", when);
	printf("Languages:     ");
	print_languages(&r);
	printf("\n%s\n", rule);
	printf("IPv4 Count:    %llu\n", (unsigned long long)v4);
	printf("IPv6 Count:    %llu\n", (unsigned long long)v6);
	printf("Networks:      %llu\n", (unsigned long long)(v4 + v6));
	printf("%s\n", rule);
	mmdb_close(&r);
	return 0;
}

command cmd_stats = {
	.name = "stats",
	.use = "stats [flags] [db.mmdb]",
	.short_desc = "Display statistics and metadata of an MMDB file",
	.flags = stats_flags,
	.nflags = 2,
	.run = run_stats,
};

/* ---- inspect ---- */

static flag inspect_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the first argument)"},
	{.name = "sample-ip", .type = F_STRING, .usage = "Sample IP to inspect structure"},
	{.name = "out", .type = F_STRING, .usage = "Optional path to export schema as JSON"},
};

typedef struct {
	jw_object o;
	jw_object sorted_print;
} schema;

static void explore(schema *s, buf_t *path, const dnode *v)
{
	size_t keep = path->len;
	if (v->kind == T_MAP) {
		for (uint32_t i = 0; i < v->n; i++) {
			const dnode *k = &v->items[2 * i];
			if (path->len)
				buf_push(path, '.');
			buf_append(path, k->s, k->n);
			explore(s, path, &v->items[2 * i + 1]);
			path->len = keep;
		}
		return;
	}
	if (v->kind == T_ARRAY) {
		for (uint32_t i = 0; i < v->n; i++) {
			buf_printf(path, "[%u]", i);
			explore(s, path, &v->items[i]);
			path->len = keep;
		}
		return;
	}
	buf_t t = {0};
	const char *name = go_type_name(v);
	json_string(&t, (const uint8_t *)name, strlen(name));
	jw_add(&s->o, (const char *)path->p, path->len, &t);
	t.len = 0;
	buf_append(&t, name, strlen(name));
	jw_add(&s->sorted_print, (const char *)path->p, path->len, &t);
	buf_free(&t);
}

static int cmp_member(const void *a, const void *b)
{
	const jw_member *x = a, *y = b;
	size_t n = x->klen < y->klen ? x->klen : y->klen;
	int c = memcmp(x->key, y->key, n);
	return c ? c : (x->klen < y->klen ? -1 : x->klen > y->klen);
}

static int run_inspect(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str;
	if (!*db && argc > 0)
		db = argv[0];
	if (!*db)
		missing_flag(c, "--db flag is required");
	mmdb_reader r;
	open_or_die(&r, db, "Failed to open MMDB");
	const char *sample = flag_get(c, "sample-ip")->str;
	u128 ip;
	bool is4;
	if (!parse_ip_text(sample, strlen(sample), &ip, &is4)) {
		ip = (u128)((4u << 24) | (7u << 16) | (229u << 8)); /* 4.7.229.0 */
		is4 = true;
	}
	uint64_t off;
	bool found;
	if (mmdb_lookup(&r, ip, is4, &off, &found) < 0)
		go_fatalf("Lookup failed: %s", err_msg());
	if (!found) {
		printf("No record found for this IP.\n");
		mmdb_close(&r);
		return 0;
	}
	arena a;
	arena_init(&a);
	dnode *v;
	uint64_t next;
	if (mdec_value(&r.data, off, &a, &v, &next) < 0)
		go_fatalf("Lookup failed: %s", err_msg());
	const char *base = strrchr(db, '/');
	printf("📂 Structure for MMDB: %s\n", base ? base + 1 : db);
	printf("────────────────────────────────────────────\n");
	schema s = {0};
	buf_t path = {0};
	if (v->kind != T_MAP && v->kind != T_ARRAY)
		buf_append(&path, "", 0);
	explore(&s, &path, v);
	buf_free(&path);
	if (s.sorted_print.n > 1)
		qsort(s.sorted_print.m, s.sorted_print.n, sizeof *s.sorted_print.m, cmp_member);
	for (size_t i = 0; i < s.sorted_print.n; i++)
		printf("%-50s : %s\n", s.sorted_print.m[i].key, s.sorted_print.m[i].json);
	for (size_t i = 0; i < s.sorted_print.n; i++) {
		free(s.sorted_print.m[i].key);
		free(s.sorted_print.m[i].json);
	}
	free(s.sorted_print.m);
	const char *out = flag_get(c, "out")->str;
	if (*out) {
		buf_t j = {0};
		jw_write_object(&s.o, &j);
		make_parent_dirs(out);
		if (write_file(out, j.p, j.len) < 0)
			go_fatalf("Failed to write schema file: %s", err_msg());
		printf("\n✅ Schema exported to: %s\n", out);
		buf_free(&j);
	} else {
		jw_object empty = s.o;
		buf_t j = {0};
		jw_write_object(&empty, &j);
		buf_free(&j);
	}
	arena_free(&a);
	mmdb_close(&r);
	return 0;
}

command cmd_inspect = {
	.name = "inspect",
	.use = "inspect [flags] [db.mmdb]",
	.short_desc = "Inspect MMDB structure and optionally export schema to JSON",
	.flags = inspect_flags,
	.nflags = 3,
	.run = run_inspect,
};

/* ---- verify ---- */

static flag verify_flags[] = {{.name = "db", .type = F_STRING, .usage = "Path to the .mmdb file (or the first argument)"}};

static int run_verify(command *c, int argc, char **argv)
{
	const char *db = flag_get(c, "db")->str;
	if (!*db && argc > 0)
		db = argv[0];
	if (!*db)
		missing_flag(c, "--db flag is required");
	mmdb_reader r;
	if (mmdb_open(&r, db) < 0 || mmdb_verify(&r) < 0) {
		printf("invalid: %s\n", err_msg());
		mmdb_close(&r);
		return 1;
	}
	printf("valid\n");
	mmdb_close(&r);
	return 0;
}

command cmd_verify = {
	.name = "verify",
	.use = "verify [flags] [db.mmdb]",
	.short_desc = "Verify that an MMDB file is valid",
	.long_desc = "Verify that an MMDB file is well-formed and readable: the search tree, the data section and the "
		     "metadata.\nPrints \"valid\" and exits 0, or \"invalid: <reason>\" and exits 1.",
	.flags = verify_flags,
	.nflags = 1,
	.run = run_verify,
};
