#include "sha256.h"

#include <string.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress_portable(uint32_t st[8], const uint8_t *p, size_t blocks)
{
	while (blocks--) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
			       (uint32_t)p[4 * i + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = h + S1 + ch + K[i] + w[i];
			uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
			uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + mj;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		st[0] += a;
		st[1] += b;
		st[2] += c;
		st[3] += d;
		st[4] += e;
		st[5] += f;
		st[6] += g;
		st[7] += h;
		p += 64;
	}
}

#if defined(__x86_64__)
__attribute__((target("sha,sse4.1,ssse3")))
static void compress_shani(uint32_t st[8], const uint8_t *p, size_t blocks)
{
	const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
	__m128i tmp = _mm_loadu_si128((const __m128i *)&st[0]);
	__m128i state1 = _mm_loadu_si128((const __m128i *)&st[4]);
	tmp = _mm_shuffle_epi32(tmp, 0xB1);       /* CDAB */
	state1 = _mm_shuffle_epi32(state1, 0x1B); /* EFGH */
	__m128i state0 = _mm_alignr_epi8(tmp, state1, 8);   /* ABEF */
	state1 = _mm_blend_epi16(state1, tmp, 0xF0);        /* CDGH */

	while (blocks--) {
		__m128i abef = state0, cdgh = state1;
		__m128i w[4];
		for (int g = 0; g < 16; g++) {
			if (g < 4) {
				w[g] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * g)), mask);
			} else {
				__m128i t = _mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]);
				t = _mm_add_epi32(t, _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4));
				w[g & 3] = _mm_sha256msg2_epu32(t, w[(g + 3) & 3]);
			}
			__m128i msg = _mm_add_epi32(w[g & 3], _mm_loadu_si128((const __m128i *)&K[4 * g]));
			state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
			msg = _mm_shuffle_epi32(msg, 0x0E);
			state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
		}
		state0 = _mm_add_epi32(state0, abef);
		state1 = _mm_add_epi32(state1, cdgh);
		p += 64;
	}

	tmp = _mm_shuffle_epi32(state0, 0x1B);       /* FEBA */
	state1 = _mm_shuffle_epi32(state1, 0xB1);    /* DCHG */
	state0 = _mm_blend_epi16(tmp, state1, 0xF0); /* DCBA */
	state1 = _mm_alignr_epi8(state1, tmp, 8);    /* ABEF */
	_mm_storeu_si128((__m128i *)&st[0], state0);
	_mm_storeu_si128((__m128i *)&st[4], state1);
}
#endif

typedef void (*compress_fn)(uint32_t st[8], const uint8_t *p, size_t blocks);

static compress_fn chosen;
static bool force_portable;

static void digest_with(compress_fn fn, const uint8_t *data, size_t len, uint8_t out[32])
{
	uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	size_t full = len / 64;
	if (full)
		fn(st, data, full);
	uint8_t tail[128];
	size_t rem = len - full * 64;
	if (rem)
		memcpy(tail, data + full * 64, rem);
	tail[rem] = 0x80;
	size_t tlen = rem + 1 > 56 ? 128 : 64;
	memset(tail + rem + 1, 0, tlen - rem - 1);
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++)
		tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
	fn(st, tail, tlen / 64);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(st[i] >> 24);
		out[4 * i + 1] = (uint8_t)(st[i] >> 16);
		out[4 * i + 2] = (uint8_t)(st[i] >> 8);
		out[4 * i + 3] = (uint8_t)st[i];
	}
}

static compress_fn choose(void)
{
#if defined(__x86_64__)
	unsigned a, b, c, d;
	if (__get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29))) {
		unsigned a1, b1, c1, d1;
		/* SSSE3 (ecx bit 9) and SSE4.1 (ecx bit 19) of leaf 1 */
		if (__get_cpuid(1, &a1, &b1, &c1, &d1) && (c1 & (1u << 9)) && (c1 & (1u << 19))) {
			/* self-test against the portable code before trusting it */
			uint8_t msg[300], x[32], y[32];
			for (size_t i = 0; i < sizeof msg; i++)
				msg[i] = (uint8_t)(i * 131 + 7);
			bool same = true;
			static const size_t lens[] = {0, 3, 55, 56, 63, 64, 65, 119, 128, 300};
			for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
				digest_with(compress_portable, msg, lens[i], x);
				digest_with(compress_shani, msg, lens[i], y);
				if (memcmp(x, y, 32) != 0)
					same = false;
			}
			if (same)
				return compress_shani;
		}
	}
#endif
	return compress_portable;
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	if (!chosen)
		chosen = choose();
	digest_with(force_portable ? compress_portable : chosen, data, len, out);
}

void sha256_16(const uint8_t *data, size_t len, uint8_t out[16])
{
	uint8_t full[32];
	sha256(data, len, full);
	memcpy(out, full, 16);
}

void sha256_force_portable(bool on) { force_portable = on; }

bool sha256_using_shani(void)
{
	if (!chosen)
		chosen = choose();
	return !force_portable && chosen != compress_portable;
}
