/* Shared values: a map or array that many records repeat, like a lookup
 * table's row with an ASN's routes, is kept once in a temporary file and
 * stands in the records, their keys and the record store as a T_SHARED value
 * (its id), until the data section writes it once and points at it. Equal
 * values get the same id, so equal records still get equal keys. */
#ifndef SHARED_H
#define SHARED_H

#include "common.h"
#include "mmdb_enc.h"
#include "mmdb_read.h"

/* Values whose encoding is shorter stay in the records themselves. */
#define SHARE_MIN 1024

typedef struct shared shared;

/* The file goes in tmp_dir when the first values are written out. */
shared *shared_new(const char *tmp_dir);
void shared_free(shared *s);
/* The value whose canonical encoding (mval_canonical) is enc: its id. */
int shared_add(shared *s, const uint8_t *enc, size_t n, uint32_t *id);
/* The value of id, allocated in a; the shared values in it stay T_SHARED. */
int shared_get(shared *s, uint32_t id, arena *a, mval **out);
/* Whether the value whose canonical encoding is enc is shared: *id. */
bool shared_find(const shared *s, const uint8_t *enc, size_t n, uint32_t *id);
size_t shared_count(const shared *s);
/* The bytes of every shared value once. */
uint64_t shared_bytes(const shared *s);

#endif
