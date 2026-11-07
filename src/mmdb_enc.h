/* MMDB data-section encoding: the value model, control bytes, pointers, and
 * the deduplicating data section of streamwriter/encode.go. */
#ifndef MMDB_ENC_H
#define MMDB_ENC_H

#include "common.h"
#include "hashtab.h"

enum {
	T_EXTENDED = 0,
	T_POINTER = 1,
	T_STRING = 2,
	T_DOUBLE = 3,
	T_BYTES = 4,
	T_UINT16 = 5,
	T_UINT32 = 6,
	T_MAP = 7,
	T_INT32 = 8,
	T_UINT64 = 9,
	T_UINT128 = 10,
	T_ARRAY = 11,
	T_CONTAINER = 12,
	T_END_MARKER = 13,
	T_BOOL = 14,
	T_FLOAT = 15,
};

/* The largest length a control byte can declare. */
#define MMDB_MAX_SIZE (65821u + (1u << 24) - 1)

/* A value to encode. Maps hold 2*n items, key then value, keys T_STRING and
 * sorted bytewise (mval_sort_map), as Go's sortedKeys orders them. */
typedef struct mval {
	uint8_t type;
	uint32_t n;
	const uint8_t *s;
	uint64_t u, u_hi; /* u_hi: the high 64 bits of a T_UINT128 */
	double f;
	struct mval *items;
} mval;

void mval_sort_map(mval *m);
/* The pointer-free encoding of v, appended to b: equal values (with sorted
 * maps) give equal bytes, and the bytes decode back to v. */
int mval_canonical(buf_t *b, const mval *v);

/* Encoders shared with the reader tests. */
int append_control(buf_t *b, int type, size_t size);
void append_uint(buf_t *b, int type, uint64_t v);
int pointer_size(uint32_t off);
void append_pointer(buf_t *b, uint32_t off);

/* 128-bit SipHash-2-4 of the pointer-free encodings in the dedup cache. */
void siphash128(const uint8_t *in, size_t n, uint8_t out[16]);

/* The data section. Bytes go to buf and, when fd >= 0, are flushed to the
 * file at file_off + their offset, so the section never has to fit in RAM. */
typedef struct {
	buf_t buf;
	uint64_t flushed;
	int fd;
	uint64_t file_off;
	ht16 seen; /* hash of pointer-free encoding -> offset of first copy */
	bool dedup;
	bool dedup_records; /* ds_record reuses identical records (default on) */
	buf_t scratch;
} datasec;

void ds_init(datasec *d, bool dedup);
void ds_free(datasec *d);
static inline uint64_t ds_size(const datasec *d) { return d->flushed + d->buf.len; }
/* Appends a top-level record, reusing an identical earlier record. */
int ds_record(datasec *d, const mval *v, uint32_t *off);
/* Appends v itself; with dedup, members may become pointers. */
int ds_inline(datasec *d, const mval *v);
int ds_flush(datasec *d);

int pwrite_all(int fd, const void *p, size_t n, uint64_t off);

#endif
