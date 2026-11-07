/* shrink: a smaller MMDB file that answers every lookup the same way, and
 * the checks that prove it.
 *
 * 1. Each search-tree node is interned by its pair of children, bottom up,
 *    so every distinct subtree is stored once; a node whose children are
 *    both empty becomes empty itself. The root stays node 0, and the IPv4
 *    subtree of an IPv6 tree keeps a node of its own, so readers that skip
 *    the IPv4 aliases while listing networks skip nothing else.
 * 2. The data section keeps what the tree still reaches (records, and what
 *    their pointers reach) in the same order, each pointer re-encoded at the
 *    smallest width for its new target.
 * 3. The metadata keeps every key; only node_count and record_size change.
 *
 * The input is mapped, not read into memory. Memory is two bits per input
 * node, plus about 30 bytes per distinct node. */
#ifndef SHRINK_H
#define SHRINK_H

#include "common.h"

typedef struct {
	int record_size; /* 24, 28 or 32; 0 keeps the input's */
	bool no_compact; /* copy the data section as it is */
	bool verbose;    /* each phase and its time on stderr */
} shrink_opts;

typedef struct {
	uint64_t in_bytes, out_bytes;
	uint64_t in_nodes, out_nodes;
	int in_record_size, out_record_size;
	int min_record_size; /* the smallest record size the shrunk tree fits */
	uint64_t in_tree, out_tree, in_data, out_data, meta_bytes;
	double seconds;
} shrink_result;

/* Writes the shrunk copy of in to out, creating or truncating it; with out
 * NULL nothing is written, but every size is still exact. */
int mmdb_shrink(const char *in, const char *out, const shrink_opts *o, shrink_result *res);

/* ---- the checks (shrink_check.c) ---- */

/* Exit codes of a failed check, as IPinfo's mmdbshrink uses them. */
enum { CHECK_LOOKUP = 2, CHECK_METADATA = 4 };

typedef struct {
	uint64_t n, hits;
	double seconds;
} check_stage;

typedef struct {
	bool metadata_ok;
	check_stage fixed, random4, random6, networks;
	int failed;      /* 0, CHECK_LOOKUP or CHECK_METADATA */
	char detail[1024];
} check_report;

typedef struct {
	uint64_t seed;
	uint64_t lookups;   /* random addresses per family */
	bool all_networks;  /* also walk every network of both files */
} check_opts;

/* Compares the original and the shrunk file: the metadata (all but
 * node_count and record_size), edge addresses, random lookups, and
 * optionally every network. Returns 0 with r->failed set by the first check
 * that fails; -1 when a file can't be read. */
int shrink_check(const char *base, const char *shrunk, const check_opts *o, check_report *r);

typedef struct {
	uint64_t lookups;
	double ns_per_op, qps;
} bench_result;

/* Timed random IPv4 lookups, each record decoded. */
int shrink_bench(const char *path, uint64_t seed, uint64_t lookups, bench_result *out);

typedef struct {
	int rounds;
	double median[2], min[2], max[2]; /* ns per lookup, file a and file b */
	double ratio;                      /* the median of b / a over the rounds */
} paired_result;

/* Two files timed in interleaved rounds (a then b, then b then a, ...),
 * each round on its own address stream, so drift and cache warmth fall on
 * both alike; the medians are robust to a noisy round. */
int shrink_bench_paired(const char *a, const char *b, uint64_t seed, uint64_t lookups, int rounds, paired_result *out);

typedef struct {
	uint64_t mapped_resident; /* bytes of the file in memory after the lookups */
	uint64_t peak_rss;
	uint64_t minflt, majflt;
	double ns_per_op;
	uint64_t lookups;
} memory_sample;

/* Runs the lookups in a fresh process (this program, re-executed), so the
 * numbers are that reader's alone. */
int shrink_memory_sample(const char *path, uint64_t seed, uint64_t lookups, memory_sample *out);
/* The child's side: prints the sample as one line on stdout. */
int shrink_memory_probe(const char *path, uint64_t seed, uint64_t lookups);

/* xoshiro256** seeded through splitmix64: the address streams. */
typedef struct {
	uint64_t s[4];
} rng;

void rng_seed(rng *g, uint64_t seed);
uint64_t rng_next(rng *g);

#endif
