/* Open-addressing hash tables with linear probing. Capacity grows by half
 * when the table is 80% full, so memory stays near 20 bytes per entry for
 * 16-byte keys. */
#ifndef HASHTAB_H
#define HASHTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16-byte keys that are already uniform (SHA-256 prefixes) -> uint32. */
typedef struct {
	uint8_t *keys;
	uint32_t *vals;
	uint64_t *used;
	size_t cap, len;
} ht16;

void ht16_init(ht16 *t);
void ht16_free(ht16 *t);
bool ht16_get(const ht16 *t, const uint8_t key[16], uint32_t *val);
/* Returns true and the existing value if key is present; otherwise inserts
 * val and returns false. */
bool ht16_get_or_put(ht16 *t, const uint8_t key[16], uint32_t val, uint32_t *out);
/* Room for n entries without growing, so a table of known size is
 * allocated once instead of holding old and new arrays while it grows. */
void ht16_reserve(ht16 *t, size_t n);

/* uint64 keys -> uint32. */
typedef struct {
	uint64_t *keys;
	uint32_t *vals;
	uint64_t *used;
	size_t cap, len;
} ht64;

void ht64_init(ht64 *t);
void ht64_free(ht64 *t);
bool ht64_get(const ht64 *t, uint64_t key, uint32_t *val);
bool ht64_get_or_put(ht64 *t, uint64_t key, uint32_t val, uint32_t *out);
/* Room for n entries without growing (a table of known most size is then
 * never held twice while it grows). */
void ht64_reserve(ht64 *t, size_t n);

#endif
