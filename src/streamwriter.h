/* The streaming MMDB writer: a C port of internal/streamwriter. The search
 * tree is written depth-first straight to the file from sorted,
 * non-overlapping input, so memory holds the current root-to-leaf path and
 * the unique records only. Two passes: sw_count assigns record ids and counts
 * nodes, sw_write_file writes the tree, data section and metadata. */
#ifndef STREAMWRITER_H
#define STREAMWRITER_H

#include "common.h"
#include "hashtab.h"
#include "mmdb_enc.h"

/* Records: ids in the counting pass, encoding in the write pass on the first
 * row of a record the finished tree points at. */
typedef int (*sw_value_fn)(void *ctx, mval **out);

/* No data offset reaches it: node_count + 16 + offset must stay below 2^32. */
#define SW_NOT_ENCODED UINT32_MAX

typedef struct {
	ht16 ids;
	uint32_t *offsets; /* data-section offset per id, SW_NOT_ENCODED until encoded */
	size_t n, cap;
	uint64_t *ref; /* bitmap of records the tree uses */
	size_t ref_words;
	datasec data;
	bool frozen, keys_dropped;
} sw_records;

int sw_intern(sw_records *r, const uint8_t *key, size_t keylen, sw_value_fn fn, void *ctx, uint32_t *id);

/* For sources that keep the record ids of the counting pass instead of
 * calling sw_intern on every row of the write pass: sw_pending is true while
 * the write pass still needs id's value (the tree uses it and it is not
 * encoded yet), and sw_supply encodes it from fn. */
bool sw_pending(const sw_records *r, uint32_t id);
int sw_supply(sw_records *r, uint32_t id, sw_value_fn fn, void *ctx);

typedef struct {
	const char *database_type;
	const char *const *description_langs;
	const char *const *description_texts;
	int ndescription;
	const char *const *languages;
	int nlanguages;
	int record_size; /* 24, 28 or 32; 0 means 32 */
	/* 6 (or 0): an IPv6 tree, IPv4 at ::/96. 4: an IPv4 tree of the ::/96
	 * addresses only; segments must stay inside ::/96. */
	int ip_version;
	bool alias_ipv6_to_ipv4;
	bool remove_reserved_networks;
	/* The source applied MaxMind::DB::Writer's alias rules to each input row
	 * itself and hands over pieces of overlapping rows (details.c), so a
	 * segment may end at an aliased network without covering its parent. It
	 * must still cover any aliased network it touches in full. */
	bool alias_rules_in_source;
	/* No two keys the source interns encode the same (the key is the encoded
	 * content), so the data section skips its whole-record dedup cache. */
	bool distinct_encodings;
	/* The source keeps record ids from the counting pass and uses
	 * sw_pending/sw_supply in the write pass, so the key table is freed
	 * after counting. */
	bool ids_kept_by_source;
} sw_options;

typedef struct {
	uint64_t rows, spans;
	size_t records;
	uint64_t nodes;
	int record_size;
	uint64_t data_bytes;
	int64_t file_bytes;
} sw_stats;

/* One input range; is4 says how the address was written, for messages. */
typedef struct {
	u128 start, end;
	bool start4, end4;
	uint32_t record;
} sw_segment;

/* Yields segments in ascending, non-overlapping order; *ok false at the end. */
typedef int (*sw_next_fn)(void *ctx, sw_segment *seg, bool *ok);

typedef struct sw_writer sw_writer;

sw_writer *sw_new(const sw_options *opts);
void sw_free(sw_writer *w);
/* Room for n records, for a source that knows an upper bound: the record
 * tables are then allocated once instead of grown. */
void sw_reserve(sw_writer *w, size_t n);
sw_records *sw_get_records(sw_writer *w);
int sw_count(sw_writer *w, sw_next_fn next, void *ctx);
/* After sw_count: the record size the file is written with (24, 28 or 32).
 * Fails if the counted nodes do not fit it. */
int sw_set_record_size(sw_writer *w, int record_size);
int sw_write_file(sw_writer *w, const char *path, sw_next_fn next, void *ctx);
sw_stats sw_get_stats(const sw_writer *w);
double sw_ceiling_use(const sw_stats *s);

/* Exposed for the unit tests. */
static inline u128 addr_last(u128 a, int depth)
{
	return depth >= 128 ? a : a | (U128_MAX >> depth);
}
static inline u128 addr_right(u128 a, int depth) { return a | ((u128)1 << (127 - depth)); }
static inline u128 addr_parent(u128 a, int depth) { return a & ~((u128)1 << (128 - depth)); }
const char *fmt_network(u128 a, int depth, char out[80]);

#endif
