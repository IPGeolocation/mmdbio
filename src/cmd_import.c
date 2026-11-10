/* import: build an MMDB from CSV, JSON or NDJSON, gzipped or not, with or
 * without a config file. Settings come from the defaults, then the config,
 * then the flags given on the command line. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "build.h"
#include "cmds.h"
#include "compat.h"

static flag import_flags[] = {
	{.name = "in", .shorthand = 'i', .type = F_STRING,
	 .usage = "Input file: CSV, JSON or NDJSON, optionally gzipped ('-' for stdin; or the first argument)"},
	{.name = "out", .shorthand = 'o', .type = F_STRING,
	 .usage = "Output .mmdb file path, or '-' for stdout (or the second argument; default: the input path with .mmdb)"},
	{.name = "config", .shorthand = 'c', .type = F_STRING, .usage = "JSON config file with the build settings; flags override it"},
	{.name = "var", .type = F_ARRAY,
	 .usage = "name=value for ${name} in the config's paths, over its \"vars\" (repeatable)"},
	{.name = "order", .type = F_STRING, .def = "file",
	 .usage = "Which overlapping row is later: file (file order) or ipv6-first (every IPv4 row after every IPv6 row)"},
	{.name = "ip", .type = F_INT, .def = "6", .usage = "IP version (4 or 6)"},
	{.name = "size", .shorthand = 's', .type = F_STRING, .def = "auto",
	 .usage = "Record size (24, 28, 32, or auto for the smallest that fits)"},
	{.name = "merge",
	 .shorthand = 'm',
	 .type = F_STRING,
	 .def = "none",
	 .usage = "Overlapping ranges: none (the later row wins), keep (the earlier row wins), toplevel or recurse (merge "
		  "the records)"},
	{.name = "alias-6to4", .type = F_BOOL, .usage = "Enable IPv6 to IPv4 aliasing"},
	{.name = "disallow-reserved", .type = F_BOOL, .usage = "Disallow inserting reserved IP ranges"},
	{.name = "title", .shorthand = 't', .type = F_STRING, .usage = "Title (database_type) for the .mmdb file (default: the input file's name)"},
	{.name = "description",
	 .shorthand = 'd',
	 .type = F_STRING,
	 .usage = "Description for the .mmdb file (default \"Custom IP Intelligence Database\")"},
	{.name = "languages", .type = F_LIST, .usage = "Languages for the metadata (default en)"},
	{.name = "format", .type = F_STRING, .def = "auto", .usage = "Input format: auto, csv, json or jsonl"},
	{.name = "delimiter", .type = F_STRING, .usage = "CSV delimiter: one character, or comma, tab, semicolon, pipe (default: detected)"},
	{.name = "header", .type = F_STRING, .def = "auto", .usage = "Whether the CSV's first row names the columns: auto, true or false"},
	{.name = "lazy-quotes", .type = F_BOOL, .usage = "Accept stray quotes in CSV fields"},
	{.name = "network-column", .type = F_STRING, .usage = "Column holding the network (CIDR, IP or range) (default: detected)"},
	{.name = "start-column", .type = F_STRING, .usage = "Column holding the first IP of the range (with --end-column)"},
	{.name = "end-column", .type = F_STRING, .usage = "Column holding the last IP of the range (with --start-column)"},
	{.name = "type",
	 .type = F_ARRAY,
	 .usage = "Field type, e.g. threat_score=uint16 (repeatable; string, uint16, uint32, uint64, uint128, int32, double, "
		  "float, boolean, array, auto)"},
	{.name = "infer-types", .type = F_BOOL, .usage = "Type CSV values by their look: integers, doubles, booleans, else strings"},
	{.name = "include", .type = F_LIST, .usage = "Only these columns (or top-level JSON keys) become fields"},
	{.name = "exclude", .type = F_LIST, .usage = "Columns (or top-level JSON keys) to leave out"},
	{.name = "nest-keys", .type = F_BOOL, .usage = "Split dotted column names (location.city.name) into nested maps"},
	{.name = "omit-empty", .type = F_BOOL, .usage = "Leave empty values out instead of storing \"\""},
	{.name = "array-separator", .type = F_STRING, .usage = "Separator inside array fields (default \",\")"},
	{.name = "strict", .type = F_BOOL, .usage = "Stop at the first bad row instead of skipping it"},
	{.name = "tmp-dir", .type = F_STRING, .usage = "Directory for temporary files (default $TMPDIR or /tmp)"},
	{.name = "memory", .type = F_INT, .def = "512", .usage = "Megabytes for sorting rows in memory before using disk"},
	{.name = "quiet", .shorthand = 'q', .type = F_BOOL, .usage = "Print only errors and the final line"},
	{.name = "columns", .type = F_LIST, .usage = "Names for the CSV's columns, in order (a header row is still skipped)"},
	{.name = "network-field", .type = F_BOOL,
	 .usage = "Add \"network\" to each record: the row's CIDR, or its range as start-end"},
	{.name = "no-fields", .type = F_BOOL,
	 .usage = "Store no columns: every network gets an empty record (only \"network\" with --network-field)"},
	{.name = "fill-gaps", .type = F_BOOL,
	 .usage = "Give addresses no row covers a record with every field set to \"\" (rows always win)"},
	{.name = "shrink", .type = F_BOOL, .usage = "Shrink the result losslessly, checked, as mmdbio shrink does"},
	{.name = "lookup", .type = F_STRING,
	 .usage = "CSV the records come from: each input row's id (--lookup-on) picks the row with that --lookup-key"},
	{.name = "lookup-key", .type = F_STRING, .usage = "The lookup's id column (default: its first column)"},
	{.name = "lookup-on", .type = F_STRING, .usage = "The input's id column (default: the lookup's id column name)"},
	{.name = "rename", .type = F_ARRAY,
	 .usage = "column=key: store a column (or top-level JSON key) under another name (repeatable)"},
	{.name = "empty-if", .type = F_ARRAY,
	 .usage = "column=value: read that value of the column as empty, e.g. country=- (repeatable)"},
	/* mmdbctl import's flags, so its command lines work as they are */
	{.name = "csv", .type = F_BOOL, .usage = "mmdbctl's --csv: the input is CSV (--format csv)"},
	{.name = "tsv", .type = F_BOOL, .usage = "mmdbctl's --tsv: the input is TSV (--format tsv)"},
	{.name = "json", .shorthand = 'j', .type = F_BOOL, .usage = "mmdbctl's --json: the input is JSON (--format json)"},
	{.name = "fields", .shorthand = 'f', .type = F_LIST,
	 .usage = "mmdbctl's --fields: names for the columns after the range column(s), for a CSV without a header"},
	{.name = "fields-from-header", .type = F_BOOL, .usage = "mmdbctl's --fields-from-header: the first row names the columns (--header true)"},
	{.name = "range-multicol", .type = F_BOOL, .usage = "mmdbctl's --range-multicol: the first two columns are the range's first and last IP"},
	{.name = "joinkey-col", .type = F_BOOL, .usage = "mmdbctl's --joinkey-col: --range-multicol, and the third column (join_key) is left out"},
	{.name = "no-network", .type = F_BOOL, .usage = "mmdbctl's --no-network: records get no network field, which is the default here"},
	{.name = "ignore-empty-values", .type = F_BOOL, .usage = "mmdbctl's name for --fill-gaps"},
	{.name = "disable-metadata-pointers", .type = F_BOOL, .def = "true",
	 .usage = "mmdbctl's flag, accepted: mmdbio never puts pointers in the metadata"},
};

static int run_import(command *c, int argc, char **argv);

command cmd_import = {
	.name = "import",
	.use = "import [flags] [input] [output.mmdb]",
	.short_desc = "Build an MMDB file from CSV, JSON or NDJSON (gzipped or not)",
	.long_desc =
		"Build an MMDB database from a CSV, JSON or NDJSON file, gzipped or not.\n\n"
		"Only the input is required. The format, the CSV delimiter and header, and the IP columns are\n"
		"detected; every other column becomes a field (CSV values are strings unless typed; JSON keeps\n"
		"its own types). Rows may come in any order and overlap: the later row wins (see --merge).\n\n"
		"Settings are taken from the defaults, then the --config file, then the flags.\n\n"
		"Inputs:\n"
		"  CSV/TSV   a network column (1.2.3.0/24, 8.8.8.8, 1.2.3.0-1.2.3.255) or start/end IP columns\n"
		"  JSON      {\"1.2.3.0/24\": {...}, ...}  (mmdbio export's format)\n"
		"            [{\"network\": \"1.2.3.0/24\", ...}, ...]\n"
		"  NDJSON    one {\"network\": ..., ...} object per line",
	.examples = "  mmdbio import data.csv.gz\n"
		    "  mmdbio import ranges.tsv ranges.mmdb --shrink\n"
		    "  cat ips.txt | mmdbio import - --columns ip --no-fields --out - > blocklist.mmdb\n"
		    "  mmdbio import --in ranges.csv --out ranges.mmdb --type threat_score=uint16 --type is_vpn=boolean\n"
		    "  mmdbio import --in city_data.json --out ip-to-city.mmdb --ip 4\n"
		    "  mmdbio import --config build.json",
	.flags = import_flags,
	.nflags = (int)(sizeof import_flags / sizeof import_flags[0]),
	.run = run_import,
};

static bool strip_suffix(char *s, const char *suf)
{
	size_t n = strlen(s), m = strlen(suf);
	if (n > m && strcasecmp(s + n - m, suf) == 0) {
		s[n - m] = 0;
		return true;
	}
	return false;
}

static void strip_input_suffixes(char *s)
{
	if (!strip_suffix(s, ".gz"))
		strip_suffix(s, ".gzip");
	static const char *const exts[] = {".csv", ".tsv", ".txt", ".json", ".jsonl", ".ndjson"};
	for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
		if (strip_suffix(s, exts[i]))
			break;
}

static int fail(const char *fmt, const char *a)
{
	fprintf(stderr, "Error: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\nRun 'mmdbio import --help' for usage.\n");
	return 1;
}

static int copy_to_stdout(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "Error: open %s: %s\n", path, go_strerror(errno));
		return -1;
	}
	char chunk[1 << 16];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		if (fwrite(chunk, 1, n, stdout) != n)
			break;
	fclose(f);
	if (fflush(stdout) != 0 || ferror(stdout)) {
		fprintf(stderr, "Error: write stdout: %s\n", go_strerror(errno));
		return -1;
	}
	return 0;
}

static int run_import(command *c, int argc, char **argv)
{
	build_opts o;
	build_opts_init(&o);
	int status = 1;
	flag *f;
	f = flag_get(c, "var");
	for (size_t i = 0; i < f->list.n; i++) {
		char *eq = strchr(f->list.v[i], '=');
		if (!eq || eq == f->list.v[i]) {
			status = fail("--var needs name=value, not \"%s\"", f->list.v[i]);
			goto done;
		}
		*eq = 0;
		opts_set_var(&o, f->list.v[i], eq + 1, true);
		*eq = '=';
	}
	const char *cfg = flag_get(c, "config")->str;
	if (*cfg && build_opts_load_config(&o, cfg) < 0) {
		fprintf(stderr, "Error: %s\n", err_msg());
		goto done;
	}
	if ((f = flag_get(c, "in"))->set)
		opts_set_str(&o.input, f->str);
	if ((f = flag_get(c, "out"))->set)
		opts_set_str(&o.output, f->str);
	{
		int pos = 0;
		if (!flag_get(c, "in")->set && pos < argc)
			opts_set_str(&o.input, argv[pos++]);
		if (!flag_get(c, "out")->set && pos < argc)
			opts_set_str(&o.output, argv[pos++]);
		if (pos < argc) {
			status = fail("unexpected argument \"%s\"", argv[pos]);
			goto done;
		}
	}
	if ((f = flag_get(c, "ip"))->set) {
		if (f->i != 4 && f->i != 6) {
			status = fail("--ip must be 4 or 6%s", "");
			goto done;
		}
		o.ip_version = (int)f->i;
	}
	if ((f = flag_get(c, "size"))->set) {
		if (strcmp(f->str, "auto") == 0)
			o.record_size = 0;
		else if (strcmp(f->str, "24") == 0 || strcmp(f->str, "28") == 0 || strcmp(f->str, "32") == 0)
			o.record_size = atoi(f->str);
		else {
			status = fail("--size must be 24, 28, 32 or auto%s", "");
			goto done;
		}
	}
	if ((f = flag_get(c, "merge"))->set) {
		if (strcmp(f->str, "none") == 0 || strcmp(f->str, "replace") == 0)
			o.merge = MERGE_REPLACE;
		else if (strcmp(f->str, "keep") == 0)
			o.merge = MERGE_KEEP;
		else if (strcmp(f->str, "toplevel") == 0)
			o.merge = MERGE_TOPLEVEL;
		else if (strcmp(f->str, "recurse") == 0)
			o.merge = MERGE_RECURSE;
		else {
			status = fail("--merge must be one of: none, keep, toplevel, recurse%s", "");
			goto done;
		}
	}
	if ((f = flag_get(c, "order"))->set) {
		if (strcmp(f->str, "file") != 0 && strcmp(f->str, "ipv6-first") != 0) {
			status = fail("--order must be file or ipv6-first, not \"%s\"", f->str);
			goto done;
		}
		o.ipv6_first = strcmp(f->str, "ipv6-first") == 0;
	}
	if ((f = flag_get(c, "alias-6to4"))->set)
		o.alias_ipv4 = f->b;
	if ((f = flag_get(c, "disallow-reserved"))->set)
		o.include_reserved = !f->b;
	if ((f = flag_get(c, "title"))->set)
		opts_set_str(&o.database_type, f->str);
	if ((f = flag_get(c, "description"))->set) {
		for (int i = 0; i < o.ndesc; i++) {
			free(o.desc_langs[i]);
			free(o.desc_texts[i]);
		}
		free(o.desc_langs);
		free(o.desc_texts);
		o.desc_langs = xcalloc(2, sizeof *o.desc_langs);
		o.desc_texts = xcalloc(2, sizeof *o.desc_texts);
		opts_set_str(&o.desc_langs[0], "en");
		opts_set_str(&o.desc_texts[0], f->str);
		o.ndesc = 1;
	}
	if ((f = flag_get(c, "languages"))->set) {
		o.languages_set = true;
		for (int i = 0; i < o.nlanguages; i++)
			free(o.languages[i]);
		free(o.languages);
		o.languages = xcalloc(f->list.n + 1, sizeof *o.languages);
		o.nlanguages = 0;
		for (size_t i = 0; i < f->list.n; i++)
			if (*f->list.v[i])
				opts_set_str(&o.languages[o.nlanguages++], f->list.v[i]);
	}
	if ((f = flag_get(c, "format"))->set) {
		if (strcmp(f->str, "auto") == 0)
			o.format = FMT_AUTO;
		else if (strcmp(f->str, "csv") == 0)
			o.format = FMT_CSV;
		else if (strcmp(f->str, "tsv") == 0) {
			o.format = FMT_CSV;
			o.delimiter = '\t';
		} else if (strcmp(f->str, "json") == 0)
			o.format = FMT_JSON;
		else if (strcmp(f->str, "jsonl") == 0 || strcmp(f->str, "ndjson") == 0)
			o.format = FMT_JSONL;
		else {
			status = fail("--format must be auto, csv, json or jsonl%s", "");
			goto done;
		}
	}
	if ((f = flag_get(c, "delimiter"))->set) {
		const char *d = f->str;
		if (strcmp(d, "comma") == 0)
			o.delimiter = ',';
		else if (strcmp(d, "tab") == 0 || strcmp(d, "\\t") == 0)
			o.delimiter = '\t';
		else if (strcmp(d, "semicolon") == 0)
			o.delimiter = ';';
		else if (strcmp(d, "pipe") == 0)
			o.delimiter = '|';
		else if (strcmp(d, "space") == 0)
			o.delimiter = ' ';
		else if (strlen(d) == 1 && d[0] != '"' && d[0] != '\n' && d[0] != '\r')
			o.delimiter = d[0];
		else {
			status = fail("--delimiter must be one character, or comma, tab, semicolon, pipe or space%s", "");
			goto done;
		}
	}
	if ((f = flag_get(c, "header"))->set) {
		if (strcmp(f->str, "auto") == 0)
			o.header = -1;
		else if (strcmp(f->str, "true") == 0 || strcmp(f->str, "yes") == 0)
			o.header = 1;
		else if (strcmp(f->str, "false") == 0 || strcmp(f->str, "no") == 0)
			o.header = 0;
		else {
			status = fail("--header must be auto, true or false%s", "");
			goto done;
		}
	}
	if ((f = flag_get(c, "lazy-quotes"))->set)
		o.lazy_quotes = f->b;
	if ((f = flag_get(c, "network-column"))->set) {
		opts_set_str(&o.network_column, f->str);
		opts_set_str(&o.start_column, NULL);
		opts_set_str(&o.end_column, NULL);
	}
	flag *fs = flag_get(c, "start-column"), *fe = flag_get(c, "end-column");
	if (fs->set != fe->set) {
		status = fail("--start-column and --end-column go together%s", "");
		goto done;
	}
	if (fs->set) {
		opts_set_str(&o.start_column, fs->str);
		opts_set_str(&o.end_column, fe->str);
		opts_set_str(&o.network_column, NULL);
	}
	f = flag_get(c, "type");
	for (size_t i = 0; i < f->list.n; i++) {
		char *eq = strrchr(f->list.v[i], '=');
		vtype t;
		if (!eq || eq == f->list.v[i] || !vtype_parse(eq + 1, &t)) {
			status = fail("--type takes field=type, with type string, uint16, uint32, uint64, uint128, int32, "
				      "double, float, boolean, array or auto: \"%s\"",
				      f->list.v[i]);
			goto done;
		}
		*eq = 0;
		opts_set_type(&o, f->list.v[i], t);
		*eq = '=';
	}
	if ((f = flag_get(c, "infer-types"))->set)
		o.infer_types = f->b;
	if ((f = flag_get(c, "include"))->set) {
		for (int i = 0; i < o.ninclude; i++)
			free(o.include[i]);
		free(o.include);
		o.ninclude = 0;
		o.include = xcalloc(f->list.n + 1, sizeof *o.include);
		for (size_t i = 0; i < f->list.n; i++)
			opts_set_str(&o.include[o.ninclude++], f->list.v[i]);
	}
	if ((f = flag_get(c, "exclude"))->set) {
		o.exclude = xrealloc(o.exclude, (size_t)(o.nexclude + (int)f->list.n + 1) * sizeof *o.exclude);
		for (size_t i = 0; i < f->list.n; i++) {
			o.exclude[o.nexclude] = NULL;
			opts_set_str(&o.exclude[o.nexclude++], f->list.v[i]);
		}
	}
	if ((f = flag_get(c, "nest-keys"))->set)
		o.nest_keys = f->b;
	if ((f = flag_get(c, "omit-empty"))->set)
		o.omit_empty = f->b;
	if ((f = flag_get(c, "array-separator"))->set) {
		if (strlen(f->str) != 1) {
			status = fail("--array-separator must be one character%s", "");
			goto done;
		}
		o.array_sep = f->str[0];
	}
	if ((f = flag_get(c, "strict"))->set)
		o.strict = f->b;
	if ((f = flag_get(c, "tmp-dir"))->set)
		opts_set_str(&o.tmp_dir, f->str);
	if ((f = flag_get(c, "memory"))->set) {
		if (f->i < 16) {
			status = fail("--memory must be at least 16 (megabytes)%s", "");
			goto done;
		}
		o.memory = (size_t)f->i << 20;
	}
	if ((f = flag_get(c, "quiet"))->set)
		o.quiet = f->b;
	if ((f = flag_get(c, "columns"))->set) {
		for (int i = 0; i < o.ncolumns; i++)
			free(o.columns[i]);
		free(o.columns);
		o.columns = xcalloc(f->list.n + 1, sizeof *o.columns);
		o.ncolumns = 0;
		for (size_t i = 0; i < f->list.n; i++)
			opts_set_str(&o.columns[o.ncolumns++], f->list.v[i]);
	}
	if ((f = flag_get(c, "network-field"))->set)
		o.network_field = f->b;
	if ((f = flag_get(c, "no-fields"))->set)
		o.no_fields = f->b;
	if ((f = flag_get(c, "fill-gaps"))->set)
		o.fill_gaps = f->b;
	if ((f = flag_get(c, "shrink"))->set)
		o.shrink = f->b;
	if ((f = flag_get(c, "lookup"))->set)
		opts_set_str(&o.lookup_path, f->str);
	if ((f = flag_get(c, "lookup-key"))->set)
		opts_set_str(&o.lookup_key, f->str);
	if ((f = flag_get(c, "lookup-on"))->set)
		opts_set_str(&o.lookup_on, f->str);
	const char *pairs[2] = {"rename", "empty-if"};
	for (int k = 0; k < 2; k++) {
		f = flag_get(c, pairs[k]);
		for (size_t i = 0; i < f->list.n; i++) {
			char *eq = strchr(f->list.v[i], '=');
			if (!eq || eq == f->list.v[i] || (k == 0 && !eq[1])) {
				status = fail(k == 0 ? "--rename takes column=key: \"%s\"" : "--empty-if takes column=value: \"%s\"",
					      f->list.v[i]);
				goto done;
			}
			*eq = 0;
			if (k == 0)
				opts_set_rename(&o, f->list.v[i], eq + 1);
			else
				opts_add_empty(&o, f->list.v[i], eq + 1);
			*eq = '=';
		}
	}
	/* mmdbctl's flags */
	if (flag_get(c, "csv")->b)
		o.format = FMT_CSV;
	if (flag_get(c, "tsv")->b) {
		o.format = FMT_CSV;
		o.delimiter = '\t';
	}
	if (flag_get(c, "json")->b)
		o.format = FMT_JSON;
	if (flag_get(c, "fields-from-header")->b)
		o.header = 1;
	if (flag_get(c, "range-multicol")->b || flag_get(c, "joinkey-col")->b)
		o.range_first2 = true;
	if (flag_get(c, "joinkey-col")->b)
		o.joinkey_col = true;
	if (flag_get(c, "no-network")->b)
		o.network_field = false;
	if (flag_get(c, "ignore-empty-values")->b)
		o.fill_gaps = true;
	if ((f = flag_get(c, "fields"))->set) {
		/* they name the columns after the range ones */
		strlist cols = {0};
		if (o.range_first2) {
			strlist_push(&cols, "start_ip");
			strlist_push(&cols, "end_ip");
			if (o.joinkey_col)
				strlist_push(&cols, "join_key");
		} else {
			strlist_push(&cols, "network");
		}
		for (size_t i = 0; i < f->list.n; i++)
			strlist_push(&cols, f->list.v[i]);
		for (int i = 0; i < o.ncolumns; i++)
			free(o.columns[i]);
		free(o.columns);
		o.columns = xcalloc(cols.n + 1, sizeof *o.columns);
		o.ncolumns = 0;
		for (size_t i = 0; i < cols.n; i++)
			opts_set_str(&o.columns[o.ncolumns++], cols.v[i]);
		strlist_free(&cols);
	}
	if ((o.lookup_key || o.lookup_on) && !o.lookup_path) {
		status = fail("--lookup-key and --lookup-on need --lookup%s", "");
		goto done;
	}

	if (!o.input || !*o.input) {
		status = fail("--in is required (or \"input\" in the config)%s", "");
		goto done;
	}
	if (!o.output || !*o.output) {
		if (strcmp(o.input, "-") == 0) {
			status = fail("--out is required when the input is stdin (--out - writes to stdout)%s", "");
			goto done;
		}
		buf_t p = {0};
		buf_printf(&p, "%s", o.input);
		char *s = (char *)buf_cstr(&p);
		strip_input_suffixes(s);
		size_t n = strlen(s);
		p.len = n;
		buf_printf(&p, ".mmdb");
		opts_set_str(&o.output, buf_cstr(&p));
		buf_free(&p);
	}
	if (strcmp(o.output, o.input) == 0 && strcmp(o.output, "-") != 0) {
		status = fail("the output would overwrite the input (%s)", o.input);
		goto done;
	}
	if (!o.database_type || !*o.database_type) {
		if (strcmp(o.input, "-") == 0) {
			opts_set_str(&o.database_type, "Custom-ip-database");
		} else {
			const char *base = strrchr(o.input, '/');
			char *t = strdup(base ? base + 1 : o.input);
			strip_input_suffixes(t);
			opts_set_str(&o.database_type, *t ? t : "Custom-ip-database");
			free(t);
		}
	}
	/* stdout, and the unshrunk file, are built in a temporary file first */
	bool to_stdout = strcmp(o.output, "-") == 0;
	if (to_stdout && isatty(1)) {
		status = fail("not writing an MMDB file to a terminal: redirect stdout, or give --out a path%s", "");
		goto done;
	}
	char *final = strdup(o.output);
	buf_t built = {0}, shrunk = {0};
	const char *tmp = o.tmp_dir && *o.tmp_dir ? o.tmp_dir : temp_dir();
	if (to_stdout) {
		buf_printf(&built, "%s/mmdbio-out-%ld.mmdb", tmp, (long)getpid());
		o.label = "stdout";
	} else if (o.shrink) {
		buf_printf(&built, "%s.unshrunk-%ld", final, (long)getpid());
		o.label = final;
	} else {
		buf_printf(&built, "%s", final);
	}
	opts_set_str(&o.output, buf_cstr(&built));
	if (build_mmdb(&o) < 0) {
		fprintf(stderr, "Error: %s\n", err_msg());
		if (strcmp(buf_cstr(&built), final) != 0)
			unlink(buf_cstr(&built));
		free(final);
		buf_free(&built);
		goto done;
	}
	status = 0;
	const char *result = buf_cstr(&built);
	if (o.shrink) {
		if (to_stdout)
			buf_printf(&shrunk, "%s/mmdbio-out-%ld.shrunk.mmdb", tmp, (long)getpid());
		else
			buf_printf(&shrunk, "%s", final);
		shrink_opts so = {0};
		check_opts co = {.seed = 1, .lookups = 1000000};
		status = shrink_and_check(result, buf_cstr(&shrunk), &so, &co, true, false, false, true);
		unlink(result);
		result = buf_cstr(&shrunk);
	}
	if (status == 0 && to_stdout && copy_to_stdout(result) < 0)
		status = 1;
	if (to_stdout)
		unlink(result);
	free(final);
	buf_free(&built);
	buf_free(&shrunk);
done:
	build_opts_free(&o);
	return status;
}
