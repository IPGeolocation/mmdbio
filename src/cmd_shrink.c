/* shrink: a smaller MMDB with the same lookups (see shrink.h), checked
 * against the original before it takes the output's name. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "compat.h"
#include "color.h"

static flag shrink_flags[] = {
	{.name = "db", .type = F_STRING, .usage = "The .mmdb file to shrink (or the first argument)"},
	{.name = "out", .type = F_STRING, .usage = "Output file (or the second argument; default: <input>.shrunk.mmdb)"},
	{.name = "overwrite", .type = F_BOOL, .usage = "Replace the output file if it exists"},
	{.name = "dry-run", .type = F_BOOL, .usage = "Report how much the file would shrink, writing nothing"},
	{.name = "record-size",
	 .type = F_INT,
	 .def = "0",
	 .usage = "Record size of the output's tree: 24, 28 or 32 (0 keeps the input's)"},
	{.name = "no-compact", .type = F_BOOL, .usage = "Copy the data section as it is, unreached records included"},
	{.name = "check", .type = F_BOOL, .def = "true", .usage = "Check the result against the input before keeping it"},
	{.name = "full-check", .type = F_BOOL, .usage = "Also compare every network of both files (slower, exhaustive)"},
	{.name = "lookups", .type = F_INT, .def = "1000000", .usage = "Random addresses checked per address family"},
	{.name = "seed", .type = F_INT, .def = "1", .usage = "Seed of the random addresses (the same seed checks the same ones)"},
	{.name = "bench", .type = F_BOOL, .def = "true", .usage = "Time the same random lookups on both files"},
	{.name = "bench-rounds", .type = F_INT, .def = "1",
	 .usage = "Interleaved benchmark rounds (input, output, then the other way round); medians over 3 or more ride out noise"},
	{.name = "full-report",
	 .type = F_BOOL,
	 .usage = "Also measure a reader's memory on both files, each in a fresh process (Linux, macOS)"},
	{.name = "verbose", .shorthand = 'v', .type = F_BOOL, .usage = "Print each phase and how long it took"},
};

static int run_shrink(command *c, int argc, char **argv);

command cmd_shrink = {
	.name = "shrink",
	.use = "shrink [flags] <input.mmdb> [output.mmdb]",
	.short_desc = "Shrink an MMDB file losslessly by storing identical parts of its tree once",
	.long_desc =
		"Shrink an MMDB file without changing any lookup. Identical subtrees of the search tree are\n"
		"stored once and empty ones dropped; the data section keeps what the tree reaches, with its\n"
		"pointers re-encoded at the smallest width. The result is a standard MMDB file any reader opens\n"
		"as it is.\n\n"
		"Every shrink is checked before it is kept: the metadata, edge addresses and 1,000,000 random\n"
		"addresses per family must answer the same in both files. The exit code is 0 when the file was\n"
		"shrunk and checked, 2 when a lookup differs, 4 when the metadata differs, and 1 on any other\n"
		"error; on a failed check nothing is written.",
	.examples = "  mmdbio shrink city.mmdb\n"
		    "  mmdbio shrink city.mmdb small/city.mmdb --record-size 28\n"
		    "  mmdbio shrink --dry-run city.mmdb\n"
		    "  mmdbio shrink --full-check --full-report city.mmdb",
	.flags = shrink_flags,
	.nflags = (int)(sizeof shrink_flags / sizeof shrink_flags[0]),
	.run = run_shrink,
};

/* 1510000000 -> "1.51 GB" */
static const char *dec_bytes(uint64_t n, char out[32])
{
	static const char *const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
	if (n < 1000) {
		snprintf(out, 32, "%llu B", (unsigned long long)n);
		return out;
	}
	double v = (double)n;
	int u = 0;
	while (v >= 1000 && u < 5) {
		v /= 1000;
		u++;
	}
	snprintf(out, 32, "%.2f %s", v, units[u]);
	return out;
}

static const char *mib(uint64_t n, char out[32])
{
	snprintf(out, 32, "%.2f MiB", (double)n / (1 << 20));
	return out;
}

static const char *secs(double s, char out[32])
{
	if (s < 1)
		snprintf(out, 32, "%.0fms", s * 1000);
	else
		snprintf(out, 32, "%.3fs", s);
	return out;
}

static double pct(uint64_t from, uint64_t to) { return from ? 100.0 * ((double)to - (double)from) / (double)from : 0; }

void shrink_default_output(const char *in, buf_t *out)
{
	out->len = 0;
	size_t n = strlen(in);
	if (n > 5 && strcasecmp(in + n - 5, ".mmdb") == 0)
		n -= 5;
	buf_append(out, in, n);
	buf_printf(out, ".shrunk.mmdb");
}

static void stage_line(FILE *o, const char *name, const check_stage *st, bool rate)
{
	char a[32], b[32];
	fprintf(o, "  %s[OK]%s %-15s n=%s  hits=%s", col(o, C_GREEN), col_end(o), name, fmt_int(st->n, a), fmt_int(st->hits, b));
	if (rate && st->n)
		fprintf(o, " (%.2f%%)", 100.0 * (double)st->hits / (double)st->n);
	fprintf(o, "  (%s)\n", secs(st->seconds, a));
}

/* Shrinks in into a temporary file next to out, checks it, then renames it
 * to out. Returns the exit code. */
int bench_rounds = 1;

int shrink_and_check(const char *in, const char *out, const shrink_opts *so, const check_opts *co, bool check,
		     bool bench, bool full_report, bool quiet)
{
	FILE *o = quiet ? stderr : stdout;
	buf_t tmp = {0};
	if (out) {
		make_parent_dirs(out);
		buf_printf(&tmp, "%s.tmp-%ld", out, (long)getpid());
	}
	shrink_result r;
	if (mmdb_shrink(in, out ? buf_cstr(&tmp) : NULL, so, &r) < 0) {
		fprintf(stderr, "Error: %s\n", err_msg());
		if (out)
			unlink(buf_cstr(&tmp));
		buf_free(&tmp);
		return 1;
	}
	char a[32], b[32], d[32], e[32];
	if (!quiet) {
		fprintf(o, "\ninput:   %s\noutput:  %s\n\n", in, out ? out : "(dry run, nothing written)");
		fprintf(o, "  size:   %-12s ->  %-12s (saved %s, %+.2f%%)\n", dec_bytes(r.in_bytes, a), dec_bytes(r.out_bytes, b),
			dec_bytes(r.in_bytes > r.out_bytes ? r.in_bytes - r.out_bytes : 0, d), pct(r.in_bytes, r.out_bytes));
		fprintf(o, "  nodes:  %-12s ->  %-12s (%+.2f%%)\n", fmt_int(r.in_nodes, a), fmt_int(r.out_nodes, b),
			pct(r.in_nodes, r.out_nodes));
		fprintf(o, "  tree:   %-12s ->  %s\n", dec_bytes(r.in_tree, a), dec_bytes(r.out_tree, b));
		fprintf(o, "  data:   %-12s ->  %-12s%s\n", dec_bytes(r.in_data, a), dec_bytes(r.out_data, b),
			r.in_data == r.out_data ? " (unchanged)" : "");
		if (r.in_record_size == r.out_record_size)
			fprintf(o, "  record size: %d bits (the smallest that fits: %d)\n", r.out_record_size, r.min_record_size);
		else
			fprintf(o, "  record size: %d -> %d bits\n", r.in_record_size, r.out_record_size);
		fprintf(o, "\nelapsed: %s\n", secs(r.seconds, a));
	}
	if (!out) {
		buf_free(&tmp);
		return 0;
	}
	const char *t = buf_cstr(&tmp);
	int status = 0;
	memory_sample m[2];
	bool have_mem = false;
	if (full_report) {
		uint64_t n = 200000;
		if (shrink_memory_sample(in, co->seed, n, &m[0]) == 0 && shrink_memory_sample(t, co->seed, n, &m[1]) == 0)
			have_mem = true;
		else
			fprintf(stderr, "warn: memory not measured: %s\n", err_msg());
	}
	if (check) {
		check_report rep;
		if (shrink_check(in, t, co, &rep) < 0) {
			fprintf(stderr, "Error: checking the shrunk file: %s\n", err_msg());
			status = 1;
			goto done;
		}
		if (!quiet)
			fprintf(o, "\nchecks (seed %llu):\n", (unsigned long long)co->seed);
		if (rep.metadata_ok && !quiet) {
			fprintf(o, "  %s[OK]%s %-15s node_count %s -> %s, record_size %d -> %d\n", col(o, C_GREEN), col_end(o),
				"metadata", fmt_int(r.in_nodes, a), fmt_int(r.out_nodes, b), r.in_record_size, r.out_record_size);
		}
		if (!quiet && rep.fixed.n)
			stage_line(o, "edge addresses", &rep.fixed, false);
		if (!quiet && rep.random4.n && (rep.failed == 0 || rep.random6.n))
			stage_line(o, "random IPv4", &rep.random4, true);
		if (!quiet && rep.random6.n && rep.failed == 0)
			stage_line(o, "random IPv6", &rep.random6, true);
		if (!quiet && rep.networks.n && rep.failed == 0) {
			fprintf(o, "  %s[OK]%s %-15s n=%s  (%s)\n", col(o, C_GREEN), col_end(o), "all networks",
				fmt_int(rep.networks.n, a), secs(rep.networks.seconds, b));
		}
		if (rep.failed) {
			fprintf(stderr, "  %s[FAIL]%s %s\n%s\n", col(stderr, C_RED), col_end(stderr),
				rep.failed == CHECK_METADATA ? "metadata" : "lookups", rep.detail);
			fprintf(stderr, "Error: the shrunk file answers differently from the input; nothing was written\n");
			status = rep.failed;
			goto done;
		}
	}
	if (bench && !quiet && bench_rounds > 1) {
		paired_result pr;
		if (shrink_bench_paired(in, t, co->seed, co->lookups, bench_rounds, &pr) == 0) {
			fprintf(o, "\nbench (%d interleaved rounds of %s random IPv4 lookups):\n", pr.rounds, fmt_int(co->lookups, a));
			for (int f = 0; f < 2; f++)
				fprintf(o, "  %s  size=%-11s per_op median=%.0fns (min %.0f, max %.0f)  qps=%s\n", f ? "output:" : "input: ",
					dec_bytes(f ? r.out_bytes : r.in_bytes, a), pr.median[f], pr.min[f], pr.max[f],
					fmt_int(pr.median[f] > 0 ? (uint64_t)(1e9 / pr.median[f]) : 0, b));
			fprintf(o, "  output/input: %.3f (the median of the rounds)\n", pr.ratio);
		}
	} else if (bench && !quiet) {
		bench_result b1, b2;
		if (shrink_bench(in, co->seed, co->lookups, &b1) == 0 && shrink_bench(t, co->seed, co->lookups, &b2) == 0) {
			fprintf(o, "\nbench (%s random IPv4 lookups each):\n", fmt_int(co->lookups, a));
			fprintf(o, "  input:   size=%-11s per_op=%.0fns  qps=%s\n", dec_bytes(r.in_bytes, a), b1.ns_per_op,
				fmt_int((uint64_t)b1.qps, b));
			fprintf(o, "  output:  size=%-11s per_op=%.0fns  qps=%s\n", dec_bytes(r.out_bytes, a), b2.ns_per_op,
				fmt_int((uint64_t)b2.qps, b));
		}
	}
	if (have_mem && !quiet) {
		fprintf(o, "\nmemory (200,000 lookups each, separate processes):\n");
		for (int i = 0; i < 2; i++)
			fprintf(o, "  %s  mapped=%s  peak_rss=%s  per_op=%.0fns  minflt=%s  majflt=%s\n",
				i ? "output:" : "input: ", mib(m[i].mapped_resident, a), mib(m[i].peak_rss, b), m[i].ns_per_op,
				fmt_int(m[i].minflt, d), fmt_int(m[i].majflt, e));
	}
	if (replace_file(t, out) < 0) {
		fprintf(stderr, "Error: rename %s to %s: %s\n", t, out, go_strerror(errno));
		status = 1;
	} else if (quiet) {
		fprintf(stderr, "shrink: %s -> %s (%+.2f%%), %s -> %s nodes%s\n", dec_bytes(r.in_bytes, a),
			dec_bytes(r.out_bytes, b), pct(r.in_bytes, r.out_bytes), fmt_int(r.in_nodes, d), fmt_int(r.out_nodes, e),
			check ? ", checked" : "");
	}
done:
	if (status)
		unlink(t);
	buf_free(&tmp);
	return status;
}

static int run_shrink(command *c, int argc, char **argv)
{
	const char *in = flag_get(c, "db")->str, *out = flag_get(c, "out")->str;
	int pos = 0;
	if (!*in && pos < argc)
		in = argv[pos++];
	if (!*out && pos < argc)
		out = argv[pos++];
	if (pos < argc) {
		fprintf(stderr, "Error: unexpected argument \"%s\"\n", argv[pos]);
		return 1;
	}
	if (!*in)
		missing_flag(c, "the input file is required (the first argument, or --db)");
	long long rs = flag_get(c, "record-size")->i;
	if (rs != 0 && rs != 24 && rs != 28 && rs != 32) {
		fprintf(stderr, "Error: --record-size must be 24, 28 or 32\n");
		return 1;
	}
	long long lookups = flag_get(c, "lookups")->i, seed = flag_get(c, "seed")->i;
	if (lookups < 0) {
		fprintf(stderr, "Error: --lookups can't be negative\n");
		return 1;
	}
	buf_t def = {0};
	bool dry = flag_get(c, "dry-run")->b;
	if (!dry && !*out) {
		shrink_default_output(in, &def);
		out = buf_cstr(&def);
	}
	struct stat st;
	if (!dry && stat(out, &st) == 0 && !flag_get(c, "overwrite")->b) {
		fprintf(stderr, "Error: %s already exists (use --overwrite to replace it)\n", out);
		buf_free(&def);
		return 1;
	}
	shrink_opts so = {.record_size = (int)rs,
			  .no_compact = flag_get(c, "no-compact")->b,
			  .verbose = flag_get(c, "verbose")->b};
	check_opts co = {.seed = (uint64_t)seed, .lookups = (uint64_t)lookups, .all_networks = flag_get(c, "full-check")->b};
	long long rounds = flag_get(c, "bench-rounds")->i;
	if (rounds < 1 || rounds > 1000) {
		fprintf(stderr, "Error: --bench-rounds must be 1 to 1000\n");
		buf_free(&def);
		return 1;
	}
	bench_rounds = (int)rounds;
	int rc = shrink_and_check(in, dry ? NULL : out, &so, &co, flag_get(c, "check")->b, flag_get(c, "bench")->b,
				  flag_get(c, "full-report")->b, false);
	buf_free(&def);
	return rc;
}
