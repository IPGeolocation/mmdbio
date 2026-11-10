/* Unit tests: address parsing and formatting, Go's number formatting, the
 * flag parser, field types, record templates' literals, lists and trimming,
 * the JSON writer, CSV/TSV cells and rows, the prefix a lookup reports, and
 * the random address stream. Prints failures and a "N passed, M failed"
 * line; exits 1 on any failure. */
#include <stdlib.h>
#include <string.h>

#include "cmds.h"
#include "flags.h"
#include "jsonw.h"
#include "mmdb_enc.h"
#include "netaddr.h"
#include "rows.h"
#include "shrink.h"
#include "tabular.h"
#include "tmpl.h"

static int passed, failed;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		if (cond) {                                           \
			passed++;                                     \
		} else {                                              \
			failed++;                                     \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                 \
			fputc('\n', stderr);                          \
		}                                                     \
	} while (0)

static const char *net_text(const char *s)
{
	static char out[200];
	u128 a, b;
	bool four;
	if (!parse_network(s, strlen(s), &a, &b, &four))
		return "invalid";
	char x[64], y[64];
	snprintf(out, sizeof out, "%s-%s%s", fmt_ip(a, four, x), fmt_ip(b, four, y), four ? " v4" : "");
	return out;
}

static void test_networks(void)
{
	struct {
		const char *in, *want;
	} cases[] = {
		{"1.2.3.0/24", "1.2.3.0-1.2.3.255 v4"},
		{"1.2.3.77/24", "1.2.3.0-1.2.3.255 v4"}, /* host bits cleared, as net.ParseCIDR does */
		{" 8.8.8.8 ", "8.8.8.8-8.8.8.8 v4"},
		{"10.0.0.1 - 10.0.0.9", "10.0.0.1-10.0.0.9 v4"},
		{"2001:db8::/32", "2001:db8::-2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"},
		{"::ffff:1.2.3.0/120", "1.2.3.0-1.2.3.255 v4"},
		{"::ffff:1.2.3.4", "1.2.3.4-1.2.3.4 v4"},
		{"16777216", "1.0.0.0-1.0.0.0 v4"},
		{"16777216-16777471", "1.0.0.0-1.0.0.255 v4"},
		{"4294967296", "::1:0:0-::1:0:0"},
		{"0.0.0.0/0", "0.0.0.0-255.255.255.255 v4"},
		{"::/0", "::-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"},
		{"01.2.3.4", "invalid"},
		{"1.2.3.0/33", "invalid"},
		{"1.2.3", "invalid"},
		{"fe80::1%eth0", "invalid"},
		{"", "invalid"},
		{"1.2.3.4/", "invalid"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const char *got = net_text(cases[i].in);
		CHECK(strcmp(got, cases[i].want) == 0, "parse_network(%s) = %s, want %s", cases[i].in, got, cases[i].want);
	}
	char n[80];
	CHECK(strcmp(fmt_net((u128)0x01020300, 120, n), "1.2.3.0/24") == 0, "fmt_net v4: %s", n);
	CHECK(strcmp(fmt_net((u128)0x20010db8 << 96, 32, n), "2001:db8::/32") == 0, "fmt_net v6: %s", n);
	CHECK(strcmp(fmt_net(0, 64, n), "::/64") == 0, "fmt_net above ::/96: %s", n);
}

typedef struct {
	char out[4096];
	size_t len;
} cidr_list;

static void collect(void *ctx, u128 ip, int bits)
{
	cidr_list *l = ctx;
	char n[80];
	l->len += (size_t)snprintf(l->out + l->len, sizeof l->out - l->len, "%s ", fmt_net(ip, bits, n));
}

static void test_cidrs(void)
{
	struct {
		const char *range, *want;
	} cases[] = {
		{"1.2.3.0-1.2.3.255", "1.2.3.0/24 "},
		{"1.2.3.1-1.2.3.6", "1.2.3.1/32 1.2.3.2/31 1.2.3.4/31 1.2.3.6/32 "},
		{"0.0.0.0-255.255.255.255", "0.0.0.0/0 "},
		{"::-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "::/0 "},
		{"10.0.0.255-10.0.1.0", "10.0.0.255/32 10.0.1.0/32 "},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		u128 a, b;
		bool four;
		parse_network(cases[i].range, strlen(cases[i].range), &a, &b, &four);
		cidr_list l = {0};
		range_cidrs(a, b, collect, &l);
		CHECK(strcmp(l.out, cases[i].want) == 0, "range_cidrs(%s) = %s", cases[i].range, l.out);
	}
}

static void test_numbers(void)
{
	struct {
		double f;
		const char *sprint, *json;
	} cases[] = {
		{37.4056, "37.4056", "37.4056"},
		{-122.0775, "-122.0775", "-122.0775"},
		{1e6, "1e+06", "1000000"},
		{123456, "123456", "123456"},
		{1234567.5, "1.2345675e+06", "1234567.5"},
		{0.0001, "0.0001", "0.0001"},
		{0.00001, "1e-05", "0.00001"},
		{1e-7, "1e-07", "1e-7"},
		{1e21, "1e+21", "1e+21"},
		{1e20, "1e+20", "100000000000000000000"},
		{0, "0", "0"},
		{-2.5, "-2.5", "-2.5"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		dnode d = {.kind = T_DOUBLE, .f = cases[i].f};
		buf_t s = {0}, j = {0};
		go_sprint(&d, &s);
		json_float(&j, cases[i].f, 64);
		CHECK(strcmp(buf_cstr(&s), cases[i].sprint) == 0, "fmt.Sprint(%g) = %s, want %s", cases[i].f, s.p,
		      cases[i].sprint);
		CHECK(strcmp(buf_cstr(&j), cases[i].json) == 0, "json(%g) = %s, want %s", cases[i].f, j.p, cases[i].json);
		buf_free(&s);
		buf_free(&j);
	}
	dnode f = {.kind = T_FLOAT, .f = (double)0.1f};
	buf_t s = {0};
	go_sprint(&f, &s);
	CHECK(strcmp(buf_cstr(&s), "0.1") == 0, "float32 0.1 = %s", s.p);
	buf_free(&s);
}

static void test_flags(void)
{
	flag fl[] = {
		{.name = "db", .type = F_STRING},
		{.name = "in", .shorthand = 'i', .type = F_STRING},
		{.name = "json", .type = F_BOOL},
		{.name = "aggregate", .type = F_BOOL, .def = "true"},
		{.name = "fields", .type = F_LIST},
		{.name = "where", .type = F_ARRAY},
		{.name = "family", .type = F_INT, .def = "0"},
	};
	command c = {.name = "t", .use = "t", .short_desc = "t", .flags = fl, .nflags = 7};
	char *argv[] = {"--db=x.mmdb", "-i", "in.csv", "--json", "--aggregate=false", "--fields", "a,b",
			"--fields=c",  "--where", "x=1,2", "--where", "y>3", "--family", "4", "pos", "--", "--db"};
	int n;
	int rc = flags_parse(&c, 17, argv, &n);
	CHECK(rc == 0, "parse rc %d", rc);
	CHECK(strcmp(flag_get(&c, "db")->str, "x.mmdb") == 0, "db");
	CHECK(strcmp(flag_get(&c, "in")->str, "in.csv") == 0, "shorthand");
	CHECK(flag_get(&c, "json")->b && !flag_get(&c, "aggregate")->b, "bools");
	strlist *f = &flag_get(&c, "fields")->list;
	CHECK(f->n == 3 && strcmp(f->v[2], "c") == 0, "list split and repeated: %zu", f->n);
	strlist *w = &flag_get(&c, "where")->list;
	CHECK(w->n == 2 && strcmp(w->v[0], "x=1,2") == 0, "array keeps commas");
	CHECK(flag_get(&c, "family")->i == 4, "int");
	CHECK(n == 2 && strcmp(argv[0], "pos") == 0 && strcmp(argv[1], "--db") == 0, "positional after --");
	char *bad[] = {"--nope"};
	fflush(stderr);
	CHECK(flags_parse(&c, 1, bad, &n) < 0, "unknown flag fails");
	char *help[] = {"-h"};
	CHECK(flags_parse(&c, 1, help, &n) == 1, "-h asks for help");
	char *missing[] = {"--db"};
	CHECK(flags_parse(&c, 1, missing, &n) < 0, "a string flag needs a value");
	char *fresh[] = {"--db", "y"};
	flags_parse(&c, 2, fresh, &n);
	CHECK(flag_get(&c, "aggregate")->b && !flag_get(&c, "fields")->set, "defaults come back on each parse");
	for (int i = 0; i < 7; i++)
		strlist_free(&fl[i].list);
}

static const char *conv(const char *s, vtype t)
{
	static char out[200];
	arena a;
	arena_init(&a);
	mval v;
	bool omit;
	char why[200];
	if (conv_text((const uint8_t *)s, strlen(s), t, ',', false, &a, &v, &omit, why, sizeof why) < 0) {
		snprintf(out, sizeof out, "error");
	} else if (omit) {
		snprintf(out, sizeof out, "omit");
	} else {
		switch (v.type) {
		case T_STRING:
			snprintf(out, sizeof out, "string:%.*s", (int)v.n, (const char *)v.s);
			break;
		case T_UINT16:
		case T_UINT32:
		case T_UINT64:
			snprintf(out, sizeof out, "u%d:%llu", v.type == T_UINT16 ? 16 : v.type == T_UINT32 ? 32 : 64,
				 (unsigned long long)v.u);
			break;
		case T_UINT128:
			snprintf(out, sizeof out, "u128:%llu:%llu", (unsigned long long)v.u_hi, (unsigned long long)v.u);
			break;
		case T_INT32:
			snprintf(out, sizeof out, "i32:%d", (int32_t)v.u);
			break;
		case T_DOUBLE:
			snprintf(out, sizeof out, "double:%g", v.f);
			break;
		case T_FLOAT:
			snprintf(out, sizeof out, "float:%g", v.f);
			break;
		case T_BOOL:
			snprintf(out, sizeof out, "bool:%d", (int)v.u);
			break;
		case T_ARRAY: {
			size_t k = (size_t)snprintf(out, sizeof out, "array:");
			for (uint32_t i = 0; i < v.n; i++)
				k += (size_t)snprintf(out + k, sizeof out - k, "%s%.*s", i ? "|" : "", (int)v.items[i].n,
						      (const char *)v.items[i].s);
			break;
		}
		default:
			snprintf(out, sizeof out, "type %d", v.type);
		}
	}
	arena_free(&a);
	return out;
}

static void test_types(void)
{
	struct {
		const char *in;
		vtype t;
		const char *want;
	} cases[] = {
		{"abc", VT_NONE, "string:abc"},
		{"", VT_STRING, "string:"},
		{"65535", VT_UINT16, "u16:65535"},
		{"65536", VT_UINT16, "error"},
		{" 42 ", VT_UINT32, "u32:42"},
		{"-1", VT_UINT32, "error"},
		{"", VT_UINT32, "omit"},
		{"18446744073709551615", VT_UINT64, "u64:18446744073709551615"},
		{"18446744073709551616", VT_UINT128, "u128:1:0"},
		{"-2147483648", VT_INT32, "i32:-2147483648"},
		{"2147483648", VT_INT32, "error"},
		{"1.5e3", VT_DOUBLE, "double:1500"},
		{"x", VT_DOUBLE, "error"},
		{"0.25", VT_FLOAT, "float:0.25"},
		{"Yes", VT_BOOL, "bool:1"},
		{"0", VT_BOOL, "bool:0"},
		{"maybe", VT_BOOL, "error"},
		{"[a, 'b', \"c\",]", VT_ARRAY, "array:a|b|c"},
		{"", VT_ARRAY, "array:"},
		{"12", VT_AUTO, "u32:12"},
		{"-7", VT_AUTO, "i32:-7"},
		{"007", VT_AUTO, "string:007"},
		{"3.25", VT_AUTO, "double:3.25"},
		{"TRUE", VT_AUTO, "bool:1"},
		{"1.2.3.4", VT_AUTO, "string:1.2.3.4"},
		{"4294967296", VT_AUTO, "u64:4294967296"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const char *got = conv(cases[i].in, cases[i].t);
		CHECK(strcmp(got, cases[i].want) == 0, "conv(\"%s\", %s) = %s, want %s", cases[i].in, vtype_name(cases[i].t),
		      got, cases[i].want);
	}
}

/* a value as compact text: strings bare, maps {k=v,...}, arrays [a|b] */
static void show(buf_t *b, const mval *v)
{
	switch (v->type) {
	case T_STRING:
		buf_printf(b, "%.*s", (int)v->n, (const char *)v->s);
		break;
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
		buf_printf(b, "u:%llu", (unsigned long long)v->u);
		break;
	case T_INT32:
		buf_printf(b, "i:%d", (int32_t)v->u);
		break;
	case T_DOUBLE:
		buf_printf(b, "d:%g", v->f);
		break;
	case T_BOOL:
		buf_printf(b, "b:%d", (int)v->u);
		break;
	case T_ARRAY:
		buf_printf(b, "[");
		for (uint32_t i = 0; i < v->n; i++) {
			if (i)
				buf_printf(b, "|");
			show(b, &v->items[i]);
		}
		buf_printf(b, "]");
		break;
	case T_MAP:
		buf_printf(b, "{");
		for (uint32_t i = 0; i < v->n; i++) {
			buf_printf(b, "%s%.*s=", i ? "," : "", (int)v->items[2 * i].n, (const char *)v->items[2 * i].s);
			show(b, &v->items[2 * i + 1]);
		}
		buf_printf(b, "}");
		break;
	default:
		buf_printf(b, "type %d", v->type);
	}
}

static void test_template_values(void)
{
	struct {
		const char *in, *want;
	} lits[] = {
		{"['1.2.3.0/24', \"2001:db8::/32\"]", "[1.2.3.0/24|2001:db8::/32]"},
		{"[]", "[]"},
		{" [1, -2, 2.5, True, False, None,] ", "[u:1|i:-2|d:2.5|b:1|b:0|]"},
		{"({'b': 'x', 'a': 'y'},)", "[{a=y,b=x}]"},
		{"{'k': 1, 'k': 2}", "{k=u:2}"},
		{"{'as': 'AS1', 'name': 'Caf\\xe9 \\u00e9\\n'}", "{as=AS1,name=Caf\xc3\xa9 \xc3\xa9\n}"},
		{"[u'py2', 10L]", "[py2|u:10]"},
		{"{\"json\": [true, null]}", "{json=[b:1|]}"},
		{"{'perl' => 'hash'}", "{perl=hash}"},
		{"'it''s'", "error"},
		{"['a'", "error"},
		{"[1 2]", "error"},
		{"{1: }", "error"},
		{"abc", "error"},
		{"['a'] x", "error"},
	};
	for (size_t i = 0; i < sizeof lits / sizeof lits[0]; i++) {
		arena a;
		arena_init(&a);
		mval v;
		char why[200];
		buf_t b = {0};
		if (tmpl_parse_literal((const uint8_t *)lits[i].in, strlen(lits[i].in), &a, &v, why, sizeof why) < 0)
			buf_printf(&b, "error");
		else
			show(&b, &v);
		CHECK(strcmp(buf_cstr(&b), lits[i].want) == 0, "literal %s = %s, want %s", lits[i].in, b.p, lits[i].want);
		buf_free(&b);
		arena_free(&a);
	}
	struct {
		const char *in, *want;
	} lists[] = {
		{"[A,B,C]", "[A|B|C]"},
		{" [ A , 'B' ,\"C\" ] ", "[A|B|C]"},
		{"[\"a,b\",c]", "[a|b|c]"}, /* Perl splits inside quotes too */
		{"[A,,B]", "[A||B]"},
		{"[A,B,,]", "[A|B]"},
		{"[\"\"]", "[]"},
		{"[]", "[]"},
		{"", "[]"},
		{"A", "[A]"},
		{"[A", "[A]"},
		{"'x\"", "[x]"},
	};
	for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
		arena a;
		arena_init(&a);
		mval v;
		buf_t b = {0};
		tmpl_parse_list((const uint8_t *)lists[i].in, strlen(lists[i].in), &a, &v);
		show(&b, &v);
		/* an empty item shows as nothing between bars; [""] is one empty item */
		const char *want = strcmp(lists[i].in, "[\"\"]") == 0 ? "[]" : lists[i].want;
		CHECK(strcmp(buf_cstr(&b), want) == 0 && (strcmp(lists[i].in, "[\"\"]") != 0 || v.n == 1),
		      "list %s = %s (%u items), want %s", lists[i].in, b.p, v.n, want);
		buf_free(&b);
		arena_free(&a);
	}
	struct {
		const char *in, *want;
	} trims[] = {
		{"  a b \t", "a b"},
		{"\xC2\xA0x\xE3\x80\x80", "x"},
		{"\xE2\x80\x83 \r\n", ""},
		{"\xC3\xA9 ", "\xC3\xA9"},
		{"\xC2\x85y\xC2\x85", "y"},
		{"\xC2\xADz", "\xC2\xADz"}, /* a soft hyphen is not white space */
	};
	for (size_t i = 0; i < sizeof trims / sizeof trims[0]; i++) {
		const uint8_t *s = (const uint8_t *)trims[i].in;
		size_t n = strlen(trims[i].in);
		tmpl_trim(&s, &n);
		CHECK(n == strlen(trims[i].want) && memcmp(s, trims[i].want, n) == 0, "trim case %zu = \"%.*s\"", i, (int)n,
		      (const char *)s);
	}
}

static void test_json_writer(void)
{
	dnode leaf = {.kind = T_STRING, .s = (const uint8_t *)"<a&b>", .n = 5};
	dnode num = {.kind = T_UINT32, .u = 7};
	dnode empty_map = {.kind = T_MAP, .n = 0}, empty_arr = {.kind = T_ARRAY, .n = 0};
	dnode arr_items[2] = {num, leaf};
	dnode arr = {.kind = T_ARRAY, .n = 2, .items = arr_items};
	dnode items[8] = {
		{.kind = T_STRING, .s = (const uint8_t *)"z", .n = 1}, arr,
		{.kind = T_STRING, .s = (const uint8_t *)"a", .n = 1}, empty_map,
		{.kind = T_STRING, .s = (const uint8_t *)"m", .n = 1}, empty_arr,
		{.kind = T_STRING, .s = (const uint8_t *)"\xff", .n = 1}, num,
	};
	dnode m = {.kind = T_MAP, .n = 4, .items = items};
	buf_t out = {0};
	jw_value(&out, &m, "", 0);
	const char *want = "{\n  \"a\": {},\n  \"m\": [],\n  \"z\": [\n    7,\n    \"\\u003ca\\u0026b\\u003e\"\n  ],\n"
			   "  \"\\ufffd\": 7\n}";
	CHECK(strcmp(buf_cstr(&out), want) == 0, "MarshalIndent:\n%s", out.p);
	buf_free(&out);
}

static const char *csv_row(char delim, const char *a, const char *b)
{
	static buf_t out;
	out.len = 0;
	const char *cells[2] = {a, b};
	size_t lens[2] = {strlen(a), strlen(b)};
	row_append(&out, delim, cells, lens, 2);
	return buf_cstr(&out);
}

static const char *cell(dnode v)
{
	static buf_t out;
	out.len = 0;
	cell_text(&v, &out);
	return buf_cstr(&out);
}

static void test_tabular(void)
{
	CHECK(strcmp(csv_row(',', "a", "b"), "a,b\n") == 0, "plain");
	CHECK(strcmp(csv_row(',', "a,b", "c"), "\"a,b\",c\n") == 0, "comma");
	CHECK(strcmp(csv_row(',', "say \"hi\"", ""), "\"say \"\"hi\"\"\",\n") == 0, "quotes: %s", csv_row(',', "say \"hi\"", ""));
	CHECK(strcmp(csv_row(',', "two\nlines", " lead"), "\"two\nlines\",\" lead\"\n") == 0, "newline, leading space");
	CHECK(strcmp(csv_row('\t', "a,b", "c\td"), "a,b\t\"c\td\"\n") == 0, "tsv: %s", csv_row('\t', "a,b", "c\td"));
	CHECK(strcmp(cell((dnode){.kind = T_STRING, .s = (const uint8_t *)"x y", .n = 3}), "x y") == 0, "string");
	CHECK(strcmp(cell((dnode){.kind = T_UINT64, .u = 18446744073709551615ull}), "18446744073709551615") == 0, "uint64");
	CHECK(strcmp(cell((dnode){.kind = T_INT32, .u = (uint64_t)(int64_t)-5}), "-5") == 0, "int32");
	CHECK(strcmp(cell((dnode){.kind = T_DOUBLE, .f = 38.00881}), "38.00881") == 0, "double");
	CHECK(strcmp(cell((dnode){.kind = T_DOUBLE, .f = 0.0000001}), "1e-7") == 0, "small double: %s",
	      cell((dnode){.kind = T_DOUBLE, .f = 0.0000001}));
	CHECK(strcmp(cell((dnode){.kind = T_BOOL, .u = 1}), "true") == 0, "bool");
	CHECK(strcmp(cell((dnode){.kind = T_BYTES, .s = (const uint8_t *)"hi!", .n = 3}), "aGkh") == 0, "bytes: %s",
	      cell((dnode){.kind = T_BYTES, .s = (const uint8_t *)"hi!", .n = 3}));
	dnode items[4] = {{.kind = T_STRING, .s = (const uint8_t *)"b", .n = 1}, {.kind = T_UINT16, .u = 2},
			  {.kind = T_STRING, .s = (const uint8_t *)"a", .n = 1}, {.kind = T_BOOL, .u = 0}};
	CHECK(strcmp(cell((dnode){.kind = T_MAP, .n = 2, .items = items}), "{\"a\":false,\"b\":2}") == 0, "map");
	out_format f;
	CHECK(out_format_from_path("x.CSV", &f) && f == OUT_CSV, "csv by name");
	CHECK(out_format_from_path("dir/x.ndjson", &f) && f == OUT_JSONL, "ndjson by name");
	CHECK(!out_format_from_path("x.txt", &f), "txt has no format");
	CHECK(out_format_parse("json-compact", &f) && f == OUT_JSONL, "json-compact is jsonl");
	columns c;
	columns_init(&c, true);
	dnode inner[2] = {{.kind = T_STRING, .s = (const uint8_t *)"city", .n = 4}, {.kind = T_STRING}};
	dnode outer[4] = {{.kind = T_STRING, .s = (const uint8_t *)"loc", .n = 3}, {.kind = T_MAP, .n = 1, .items = inner},
			  {.kind = T_STRING, .s = (const uint8_t *)"asn", .n = 3}, {.kind = T_UINT32, .u = 1}};
	dnode rec = {.kind = T_MAP, .n = 2, .items = outer};
	columns_add_record(&c, &rec);
	columns_add_record(&c, &rec);
	columns_sort(&c);
	CHECK(c.n == 2 && strcmp(c.names[0], "asn") == 0 && strcmp(c.names[1], "loc.city") == 0, "flattened columns");
	columns_free(&c);
}

static void test_rng(void)
{
	rng a, b;
	rng_seed(&a, 1);
	rng_seed(&b, 1);
	bool same = true;
	uint64_t x = 0;
	for (int i = 0; i < 1000; i++) {
		uint64_t v = rng_next(&a);
		same = same && v == rng_next(&b);
		x ^= v;
	}
	CHECK(same && x != 0, "the same seed gives the same stream");
	rng_seed(&b, 2);
	CHECK(rng_next(&a) != rng_next(&b), "another seed, another stream");
}

int main(void)
{
	test_networks();
	test_cidrs();
	test_numbers();
	test_flags();
	test_types();
	test_template_values();
	test_json_writer();
	test_tabular();
	test_rng();
	printf("%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
