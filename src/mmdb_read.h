/* An MMDB reader for mmdb-diff: metadata, tree walking in address order,
 * value decoding and the format's checks (verify). */
#ifndef MMDB_READ_H
#define MMDB_READ_H

#include "common.h"
#include "hashtab.h"
#include "mmdb_enc.h"

/* A decoded value. Strings and bytes point into the mapped file; maps hold
 * 2*n items (key, value). Allocated from an arena. */
typedef struct dnode {
	uint32_t kind;
	uint32_t n;
	const uint8_t *s;
	uint64_t u, u_hi;
	double f;
	struct dnode *items;
} dnode;

typedef struct {
	uint8_t *p;
	size_t used, cap;
	struct arena_block *blocks;
} arena;

void arena_init(arena *a);
void *arena_alloc(arena *a, size_t n);
void arena_reset(arena *a);
void arena_free(arena *a);

/* A decoder over one section (the data section or the metadata section);
 * offsets and pointers are relative to buf. */
typedef struct {
	const uint8_t *buf;
	uint64_t len;
} mdec;

/* Decodes the value at off, following pointers, into *out; *next is the
 * offset after the value itself (after the pointer bytes for a pointer). */
int mdec_value(const mdec *d, uint64_t off, arena *a, dnode **out, uint64_t *next);
/* nextValueOffset(off, 1). */
int mdec_skip(const mdec *d, uint64_t off, uint64_t *next);

/* Type-aware, map-key-sorted bytes: equal values give equal bytes. */
void dnode_canonical(const dnode *v, buf_t *out);
/* Go's json.Marshal of the decoded value. */
void dnode_json(const dnode *v, buf_t *out);
/* encoding/json's pieces: an HTML-safe string, the shortest float (32 or
 * 64 bits), a 128-bit integer, base64 bytes. */
void json_string(buf_t *out, const uint8_t *s, size_t n);
void json_float(buf_t *out, double f, int bits);
void json_u128(buf_t *out, uint64_t hi, uint64_t lo);
void json_base64(buf_t *out, const uint8_t *s, size_t n);
/* A map's pair indexes in bytewise key order (malloc'd). */
uint32_t *dnode_sorted_pairs(const dnode *v);
/* The decoded value as an encoder value (strings stay where they are). */
mval *dnode_to_mval(arena *a, const dnode *v);
/* Every string and map key is valid UTF-8. */
bool dnode_utf8_ok(const dnode *v);

typedef struct {
	const char *path;
	const uint8_t *map;
	size_t size;
	uint64_t node_count, record_size, ip_version, major, minor, build_epoch;
	dnode *meta_root; /* the decoded metadata map */
	arena meta_arena;
	const uint8_t *database_type;
	size_t database_type_len;
	uint32_t ndescription, nlanguages;
	dnode *description, *languages;
	uint64_t tree_size, node_bytes;
	mdec data;     /* data section */
	mdec meta;     /* metadata section, after the marker */
	uint64_t ipv4_start;
	int ipv4_start_bit_depth;
} mmdb_reader;

int mmdb_open(mmdb_reader *r, const char *path);
void mmdb_close(mmdb_reader *r);
int mmdb_read_node(const mmdb_reader *r, uint64_t node, uint64_t *left, uint64_t *right);
int mmdb_verify(const mmdb_reader *r);
/* A lookup: an IPv4 address (is4, the value in the low 32
 * bits) goes from the ::/96 node of an IPv6 tree. *found is false where the
 * tree has no data; *offset is then the record's data-section offset. */
int mmdb_lookup(const mmdb_reader *r, u128 ip, bool is4, uint64_t *offset, bool *found);
/* The same, with the length of the matched (or empty) network in the
 * 128-bit space: an IPv4 /24 is 120. */
int mmdb_lookup_net(const mmdb_reader *r, u128 ip, bool is4, uint64_t *offset, bool *found, int *prefix);

/* Networks with data in address order, skipping IPv4 aliases like
 * the Go readers' Networks(). */
typedef struct {
	u128 ip;
	int bit;
	uint64_t pointer;
} net_node;

typedef struct {
	const mmdb_reader *r;
	net_node *stack;
	size_t n, cap;
} net_iter;

void net_iter_init(net_iter *it, const mmdb_reader *r);
void net_iter_free(net_iter *it);
/* 1 with a network, 0 at the end, -1 on error. */
int net_iter_next(net_iter *it, u128 *ip, int *bit, uint64_t *offset);

#endif
