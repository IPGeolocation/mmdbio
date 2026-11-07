#include "mmdb_enc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#define FLUSH_AT (8u << 20)

static int key_cmp(const mval *a, const mval *b)
{
	size_t n = a->n < b->n ? a->n : b->n;
	int c = n ? memcmp(a->s, b->s, n) : 0;
	if (c)
		return c;
	return a->n < b->n ? -1 : a->n > b->n;
}

void mval_sort_map(mval *m)
{
	/* insertion sort of (key, value) pairs: maps here are small */
	for (uint32_t i = 1; i < m->n; i++) {
		mval k = m->items[2 * i], v = m->items[2 * i + 1];
		uint32_t j = i;
		while (j > 0 && key_cmp(&m->items[2 * (j - 1)], &k) > 0) {
			m->items[2 * j] = m->items[2 * (j - 1)];
			m->items[2 * j + 1] = m->items[2 * (j - 1) + 1];
			j--;
		}
		m->items[2 * j] = k;
		m->items[2 * j + 1] = v;
	}
}

int append_control(buf_t *b, int type, size_t size)
{
	uint8_t first = type <= 7 ? (uint8_t)(type << 5) : 0;
	uint8_t extra[3];
	int nextra = 0;
	if (size < 29) {
		first |= (uint8_t)size;
	} else if (size < 285) {
		first |= 29;
		extra[nextra++] = (uint8_t)(size - 29);
	} else if (size < 65821) {
		first |= 30;
		size_t s = size - 285;
		extra[nextra++] = (uint8_t)(s >> 8);
		extra[nextra++] = (uint8_t)s;
	} else if (size <= MMDB_MAX_SIZE) {
		first |= 31;
		size_t s = size - 65821;
		extra[nextra++] = (uint8_t)(s >> 16);
		extra[nextra++] = (uint8_t)(s >> 8);
		extra[nextra++] = (uint8_t)s;
	} else {
		err_set("cannot store %zu bytes or items in one MMDB value (max %u)", size, MMDB_MAX_SIZE);
		return -1;
	}
	buf_push(b, first);
	if (type > 7)
		buf_push(b, (uint8_t)(type - 7));
	buf_append(b, extra, (size_t)nextra);
	return 0;
}

void append_uint(buf_t *b, int type, uint64_t v)
{
	int n = 0;
	for (uint64_t x = v; x; x >>= 8)
		n++;
	append_control(b, type, (size_t)n);
	for (int i = n - 1; i >= 0; i--)
		buf_push(b, (uint8_t)(v >> (8 * i)));
}

int pointer_size(uint32_t off)
{
	if (off < (1u << 11))
		return 2;
	if (off < (1u << 19) + 2048)
		return 3;
	if (off < (1u << 27) + 526336)
		return 4;
	return 5;
}

void append_pointer(buf_t *b, uint32_t off)
{
	const uint8_t p = T_POINTER << 5;
	uint8_t out[5];
	int n;
	if (off < (1u << 11)) {
		out[0] = p | (uint8_t)(off >> 8);
		out[1] = (uint8_t)off;
		n = 2;
	} else if (off < (1u << 19) + 2048) {
		uint32_t v = off - 2048;
		out[0] = p | 1 << 3 | (uint8_t)(v >> 16);
		out[1] = (uint8_t)(v >> 8);
		out[2] = (uint8_t)v;
		n = 3;
	} else if (off < (1u << 27) + 526336) {
		uint32_t v = off - 526336;
		out[0] = p | 2 << 3 | (uint8_t)(v >> 24);
		out[1] = (uint8_t)(v >> 16);
		out[2] = (uint8_t)(v >> 8);
		out[3] = (uint8_t)v;
		n = 4;
	} else {
		out[0] = p | 3 << 3;
		out[1] = (uint8_t)(off >> 24);
		out[2] = (uint8_t)(off >> 16);
		out[3] = (uint8_t)(off >> 8);
		out[4] = (uint8_t)off;
		n = 5;
	}
	buf_append(b, out, (size_t)n);
}

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                                                   \
	do {                                                                                       \
		v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);                           \
		v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                                              \
		v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                                              \
		v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);                           \
	} while (0)

void siphash128(const uint8_t *in, size_t n, uint8_t out[16])
{
	const uint64_t k0 = 0x0706050403020100ULL, k1 = 0x0f0e0d0c0b0a0908ULL;
	uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
	v1 ^= 0xee;
	size_t full = n & ~(size_t)7;
	for (size_t i = 0; i < full; i += 8) {
		uint64_t m;
		memcpy(&m, in + i, 8);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
	uint64_t b = (uint64_t)n << 56;
	for (size_t i = 0; i < (n & 7); i++)
		b |= (uint64_t)in[full + i] << (8 * i);
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xee;
	SIPROUND; SIPROUND; SIPROUND; SIPROUND;
	uint64_t h0 = v0 ^ v1 ^ v2 ^ v3;
	v1 ^= 0xdd;
	SIPROUND; SIPROUND; SIPROUND; SIPROUND;
	uint64_t h1 = v0 ^ v1 ^ v2 ^ v3;
	memcpy(out, &h0, 8);
	memcpy(out + 8, &h1, 8);
}

static bool cacheable(const mval *v)
{
	return v->type == T_MAP || v->type == T_ARRAY || v->type == T_STRING || v->type == T_BYTES;
}

static int append_scalar(buf_t *b, const mval *v)
{
	uint8_t raw[8];
	switch (v->type) {
	case T_STRING:
	case T_BYTES:
		if (append_control(b, v->type, v->n) < 0)
			return -1;
		buf_append(b, v->s, v->n);
		return 0;
	case T_UINT16:
	case T_UINT32:
	case T_UINT64:
		append_uint(b, v->type, v->u);
		return 0;
	case T_UINT128: {
		if (v->u_hi == 0) {
			append_uint(b, T_UINT128, v->u);
			return 0;
		}
		int n = 8;
		for (uint64_t x = v->u_hi; x; x >>= 8)
			n++;
		append_control(b, T_UINT128, (size_t)n);
		for (int i = n - 9; i >= 0; i--)
			buf_push(b, (uint8_t)(v->u_hi >> (8 * i)));
		for (int i = 7; i >= 0; i--)
			buf_push(b, (uint8_t)(v->u >> (8 * i)));
		return 0;
	}
	case T_INT32:
		if ((int32_t)v->u < 0) {
			append_control(b, T_INT32, 4);
			uint32_t x = (uint32_t)v->u;
			for (int i = 0; i < 4; i++)
				raw[i] = (uint8_t)(x >> (24 - 8 * i));
			buf_append(b, raw, 4);
			return 0;
		}
		append_uint(b, T_INT32, (uint32_t)v->u);
		return 0;
	case T_BOOL:
		return append_control(b, T_BOOL, v->u ? 1 : 0);
	case T_FLOAT: {
		float f = (float)v->f;
		uint32_t x;
		memcpy(&x, &f, 4);
		append_control(b, T_FLOAT, 4);
		for (int i = 0; i < 4; i++)
			raw[i] = (uint8_t)(x >> (24 - 8 * i));
		buf_append(b, raw, 4);
		return 0;
	}
	case T_DOUBLE: {
		uint64_t x;
		memcpy(&x, &v->f, 8);
		append_control(b, T_DOUBLE, 8);
		for (int i = 0; i < 8; i++)
			raw[i] = (uint8_t)(x >> (56 - 8 * i));
		buf_append(b, raw, 8);
		return 0;
	}
	}
	err_set("streamwriter cannot encode MMDB type %d", v->type);
	return -1;
}

/* canonical: the pointer-free encoding that identifies a value. */
static int canonical(buf_t *b, const mval *v)
{
	switch (v->type) {
	case T_MAP:
		if (append_control(b, T_MAP, v->n) < 0)
			return -1;
		for (uint32_t i = 0; i < v->n; i++) {
			if (append_scalar(b, &v->items[2 * i]) < 0 || canonical(b, &v->items[2 * i + 1]) < 0)
				return -1;
		}
		return 0;
	case T_ARRAY:
		if (append_control(b, T_ARRAY, v->n) < 0)
			return -1;
		for (uint32_t i = 0; i < v->n; i++)
			if (canonical(b, &v->items[i]) < 0)
				return -1;
		return 0;
	default:
		return append_scalar(b, v);
	}
}

int mval_canonical(buf_t *b, const mval *v) { return canonical(b, v); }

void ds_init(datasec *d, bool dedup)
{
	memset(d, 0, sizeof *d);
	d->fd = -1;
	d->dedup = dedup;
	d->dedup_records = true;
	ht16_init(&d->seen);
}

void ds_free(datasec *d)
{
	buf_free(&d->buf);
	buf_free(&d->scratch);
	ht16_free(&d->seen);
}

int pwrite_all(int fd, const void *p, size_t n, uint64_t off)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = pwrite(fd, b, n, (off_t)off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			err_set("write: %s", go_strerror(errno));
			return -1;
		}
		b += w;
		n -= (size_t)w;
		off += (uint64_t)w;
	}
	return 0;
}

int ds_flush(datasec *d)
{
	if (d->fd < 0 || d->buf.len == 0)
		return 0;
	if (pwrite_all(d->fd, d->buf.p, d->buf.len, d->file_off + d->flushed) < 0)
		return -1;
	d->flushed += d->buf.len;
	d->buf.len = 0;
	return 0;
}

static int ds_write(datasec *d, const mval *v);

int ds_inline(datasec *d, const mval *v)
{
	switch (v->type) {
	case T_MAP:
		if (append_control(&d->buf, T_MAP, v->n) < 0)
			return -1;
		for (uint32_t i = 0; i < v->n; i++)
			if (ds_write(d, &v->items[2 * i]) < 0 || ds_write(d, &v->items[2 * i + 1]) < 0)
				return -1;
		return 0;
	case T_ARRAY:
		if (append_control(&d->buf, T_ARRAY, v->n) < 0)
			return -1;
		for (uint32_t i = 0; i < v->n; i++)
			if (ds_write(d, &v->items[i]) < 0)
				return -1;
		return 0;
	default:
		return append_scalar(&d->buf, v);
	}
}

static int ds_write(datasec *d, const mval *v)
{
	if (!d->dedup || !cacheable(v))
		return ds_inline(d, v);
	d->scratch.len = 0;
	if (canonical(&d->scratch, v) < 0)
		return -1;
	size_t clen = d->scratch.len;
	uint8_t h[16];
	siphash128(d->scratch.p, clen, h);
	uint32_t off;
	if (ht16_get(&d->seen, h, &off)) {
		if ((size_t)pointer_size(off) < clen) {
			append_pointer(&d->buf, off);
			return 0;
		}
		return ds_inline(d, v);
	}
	uint64_t start = ds_size(d);
	if (ds_inline(d, v) < 0)
		return -1;
	if (start <= UINT32_MAX) {
		uint32_t dummy;
		ht16_get_or_put(&d->seen, h, (uint32_t)start, &dummy);
	}
	return 0;
}

int ds_record(datasec *d, const mval *v, uint32_t *out)
{
	uint8_t h[16] = {0};
	if (d->dedup_records) {
		d->scratch.len = 0;
		if (canonical(&d->scratch, v) < 0)
			return -1;
		siphash128(d->scratch.p, d->scratch.len, h);
		uint32_t off;
		if (ht16_get(&d->seen, h, &off)) {
			*out = off;
			return 0;
		}
	}
	uint64_t start = ds_size(d);
	if (ds_inline(d, v) < 0)
		return -1;
	if (ds_size(d) > UINT32_MAX) {
		err_set("data section exceeds the 4 GiB the MMDB format can address");
		return -1;
	}
	if (d->dedup_records) {
		uint32_t dummy;
		ht16_get_or_put(&d->seen, h, (uint32_t)start, &dummy);
	}
	*out = (uint32_t)start;
	if (d->buf.len >= FLUSH_AT)
		return ds_flush(d);
	return 0;
}
