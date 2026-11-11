/* libmmdbio: what the mmdbio command does, for C programs.
 *
 * Open an MMDB file and look addresses up, walk its networks, verify it,
 * shrink it, check that two files answer alike, time lookups, and build one
 * from CSV, TSV, JSON or NDJSON with an import config file.
 *
 * Build it with `make lib` and link with build/libmmdbio.a -lm (and -lpsapi
 * on Windows). Only the mmdbio_ names below are exported.
 *
 * Every function that can fail returns 0, or -1 with mmdbio_error() saying
 * why. The library keeps one error message for the whole process, so call
 * it from one thread at a time. */
#ifndef MMDBIO_H
#define MMDBIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "2.0.0" */
const char *mmdbio_version(void);
/* The message of the last failure. */
const char *mmdbio_error(void);

/* ---- reading ---- */

typedef struct mmdbio_db mmdbio_db;

/* Maps the file; mmdbio_close unmaps it. */
int mmdbio_open(const char *path, mmdbio_db **db);
void mmdbio_close(mmdbio_db *db);

typedef struct {
	const char *database_type; /* valid until mmdbio_close */
	int ip_version;            /* 4 or 6 */
	int record_size;           /* 24, 28 or 32 */
	uint64_t node_count;
	uint64_t build_epoch;
	uint64_t file_size, tree_size, data_size;
} mmdbio_metadata;

int mmdbio_get_metadata(const mmdbio_db *db, mmdbio_metadata *out);

/* Looks ip up (IPv4 or IPv6 text). *found is 0 where the file has no data.
 * Otherwise *json is the record as compact JSON (keys sorted) and *network
 * the network that matched ("8.8.8.0/24"); free() both. Either pointer may
 * be NULL when not wanted. */
int mmdbio_lookup(const mmdbio_db *db, const char *ip, int *found, char **json, char **network);

/* Calls fn for every network with data, in address order (IPv4 networks of
 * an IPv6 file as IPv4, aliases skipped). A non-zero return from fn stops
 * the walk and is returned. The strings are valid only during the call. */
typedef int (*mmdbio_network_fn)(void *ctx, const char *network, const char *json);
int mmdbio_networks(const mmdbio_db *db, mmdbio_network_fn fn, void *ctx);

/* The format's checks: the search tree, the data section, the metadata. */
int mmdbio_verify(const char *path);

/* ---- shrinking ---- */

typedef struct {
	int record_size; /* 24, 28 or 32; 0 keeps the input's */
	int no_compact;  /* copy the data section as it is */
} mmdbio_shrink_options;

typedef struct {
	uint64_t in_bytes, out_bytes;
	uint64_t in_nodes, out_nodes;
	int in_record_size, out_record_size, min_record_size;
	uint64_t in_tree, out_tree, in_data, out_data;
	double seconds;
} mmdbio_shrink_result;

/* Writes the shrunk copy of in to out (out NULL: sizes only). It does not
 * check the result: call mmdbio_check for that. opts may be NULL. */
int mmdbio_shrink(const char *in, const char *out, const mmdbio_shrink_options *opts, mmdbio_shrink_result *res);

typedef struct {
	uint64_t seed;     /* 0 means 1 */
	uint64_t lookups;  /* random addresses per family; 0 means 1,000,000 */
	int all_networks;  /* also compare every network of both files */
} mmdbio_check_options;

enum { MMDBIO_CHECK_OK = 0, MMDBIO_CHECK_LOOKUP = 2, MMDBIO_CHECK_METADATA = 4 };

typedef struct {
	int failed;          /* MMDBIO_CHECK_OK, or the check that failed */
	char detail[1024];   /* what differed */
	uint64_t fixed, random4, random6, networks; /* lookups (networks) compared */
} mmdbio_check_result;

/* Compares two files that should answer alike (an original and its shrunk
 * copy): the metadata but node_count and record_size, edge addresses,
 * random lookups, and optionally every network. Returns 0 with res->failed
 * set; -1 only when a file can't be read. opts may be NULL. */
int mmdbio_check(const char *a, const char *b, const mmdbio_check_options *opts, mmdbio_check_result *res);

/* ---- timing ---- */

typedef struct {
	double ns_per_op, qps;
} mmdbio_bench_result;

/* Random IPv4 lookups, each record decoded. */
int mmdbio_bench(const char *path, uint64_t seed, uint64_t lookups, mmdbio_bench_result *res);

typedef struct {
	int rounds;
	double median[2], min[2], max[2]; /* ns per lookup: a, b */
	double ratio;                      /* median of b / a over the rounds */
} mmdbio_paired_result;

/* Both files in interleaved rounds, each round on its own address stream. */
int mmdbio_bench_paired(const char *a, const char *b, uint64_t seed, uint64_t lookups, int rounds,
			mmdbio_paired_result *res);

/* ---- building ---- */

/* Builds the MMDB an import config file describes (the JSON file of
 * mmdbio import --config; see the README). Progress goes to stderr unless
 * quiet. */
int mmdbio_build(const char *config_path, int quiet);

#ifdef __cplusplus
}
#endif

#endif
