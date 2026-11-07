#include "hashtab.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"

#define MIN_CAP 1024

static inline size_t slot_of(uint64_t h, size_t cap) { return (size_t)(((u128)h * cap) >> 64); }
static inline bool is_used(const uint64_t *used, size_t i) { return used[i >> 6] >> (i & 63) & 1; }
static inline void set_used(uint64_t *used, size_t i) { used[i >> 6] |= (uint64_t)1 << (i & 63); }
static inline size_t next_slot(size_t i, size_t cap) { return i + 1 == cap ? 0 : i + 1; }

static uint64_t key16_hash(const uint8_t *k)
{
	uint64_t h;
	memcpy(&h, k, 8);
	return h;
}

static uint64_t mix64(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

static size_t grown(size_t cap) { return cap < MIN_CAP ? MIN_CAP : cap + cap / 2; }

/* ---- ht16 ---- */

static void ht16_alloc(ht16 *t, size_t cap)
{
	t->cap = cap;
	t->len = 0;
	t->keys = xmalloc(cap * 16);
	t->vals = xmalloc(cap * sizeof *t->vals);
	t->used = xcalloc((cap + 63) / 64, sizeof *t->used);
}

void ht16_init(ht16 *t) { memset(t, 0, sizeof *t); }

void ht16_free(ht16 *t)
{
	free(t->keys);
	free(t->vals);
	free(t->used);
	memset(t, 0, sizeof *t);
}

static size_t ht16_find(const ht16 *t, const uint8_t *key, bool *found)
{
	size_t i = slot_of(key16_hash(key), t->cap);
	for (;;) {
		if (!is_used(t->used, i)) {
			*found = false;
			return i;
		}
		if (memcmp(t->keys + i * 16, key, 16) == 0) {
			*found = true;
			return i;
		}
		i = next_slot(i, t->cap);
	}
}

static void ht16_rehash_to(ht16 *t, size_t cap)
{
	ht16 old = *t;
	ht16_alloc(t, cap);
	for (size_t i = 0; i < old.cap; i++) {
		if (!is_used(old.used, i))
			continue;
		bool found;
		size_t j = ht16_find(t, old.keys + i * 16, &found);
		memcpy(t->keys + j * 16, old.keys + i * 16, 16);
		t->vals[j] = old.vals[i];
		set_used(t->used, j);
		t->len++;
	}
	free(old.keys);
	free(old.vals);
	free(old.used);
}

bool ht16_get(const ht16 *t, const uint8_t key[16], uint32_t *val)
{
	if (!t->cap)
		return false;
	bool found;
	size_t i = ht16_find(t, key, &found);
	if (found)
		*val = t->vals[i];
	return found;
}

bool ht16_get_or_put(ht16 *t, const uint8_t key[16], uint32_t val, uint32_t *out)
{
	if (!t->cap)
		ht16_alloc(t, MIN_CAP);
	bool found;
	size_t i = ht16_find(t, key, &found);
	if (found) {
		*out = t->vals[i];
		return true;
	}
	memcpy(t->keys + i * 16, key, 16);
	t->vals[i] = val;
	set_used(t->used, i);
	t->len++;
	*out = val;
	if (t->len * 5 > t->cap * 4)
		ht16_rehash_to(t, grown(t->cap));
	return false;
}

void ht16_reserve(ht16 *t, size_t n)
{
	size_t cap = n + n / 4 + 1;
	if (cap > t->cap)
		ht16_rehash_to(t, cap < MIN_CAP ? MIN_CAP : cap);
}

/* ---- ht64 ---- */

static void ht64_alloc(ht64 *t, size_t cap)
{
	t->cap = cap;
	t->len = 0;
	t->keys = xmalloc(cap * sizeof *t->keys);
	t->vals = xmalloc(cap * sizeof *t->vals);
	t->used = xcalloc((cap + 63) / 64, sizeof *t->used);
}

void ht64_init(ht64 *t) { memset(t, 0, sizeof *t); }

void ht64_free(ht64 *t)
{
	free(t->keys);
	free(t->vals);
	free(t->used);
	memset(t, 0, sizeof *t);
}

static size_t ht64_find(const ht64 *t, uint64_t key, bool *found)
{
	size_t i = slot_of(mix64(key), t->cap);
	for (;;) {
		if (!is_used(t->used, i)) {
			*found = false;
			return i;
		}
		if (t->keys[i] == key) {
			*found = true;
			return i;
		}
		i = next_slot(i, t->cap);
	}
}

static void ht64_rehash_to(ht64 *t, size_t cap)
{
	ht64 old = *t;
	ht64_alloc(t, cap);
	for (size_t i = 0; i < old.cap; i++) {
		if (!is_used(old.used, i))
			continue;
		bool found;
		size_t j = ht64_find(t, old.keys[i], &found);
		t->keys[j] = old.keys[i];
		t->vals[j] = old.vals[i];
		set_used(t->used, j);
		t->len++;
	}
	free(old.keys);
	free(old.vals);
	free(old.used);
}

static void ht64_rehash(ht64 *t) { ht64_rehash_to(t, grown(t->cap)); }

void ht64_reserve(ht64 *t, size_t n)
{
	size_t cap = n + n / 4 + 1;
	if (cap > t->cap)
		ht64_rehash_to(t, cap < MIN_CAP ? MIN_CAP : cap);
}

bool ht64_get(const ht64 *t, uint64_t key, uint32_t *val)
{
	if (!t->cap)
		return false;
	bool found;
	size_t i = ht64_find(t, key, &found);
	if (found)
		*val = t->vals[i];
	return found;
}

bool ht64_get_or_put(ht64 *t, uint64_t key, uint32_t val, uint32_t *out)
{
	if (!t->cap)
		ht64_alloc(t, MIN_CAP);
	bool found;
	size_t i = ht64_find(t, key, &found);
	if (found) {
		*out = t->vals[i];
		return true;
	}
	t->keys[i] = key;
	t->vals[i] = val;
	set_used(t->used, i);
	t->len++;
	*out = val;
	if (t->len * 5 > t->cap * 4)
		ht64_rehash(t);
	return false;
}
