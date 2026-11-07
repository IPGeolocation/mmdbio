#include "netaddr.h"

#include "compat.h"

#include <string.h>

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t')) {
		(*s)++;
		(*n)--;
	}
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t'))
		(*n)--;
}

bool parse_ip_text(const char *s, size_t n, u128 *out, bool *is4)
{
	char tmp[64];
	if (n == 0 || n >= sizeof tmp || memchr(s, 0, n))
		return false;
	memcpy(tmp, s, n);
	tmp[n] = 0;
	if (memchr(s, ':', n)) {
		uint8_t b[16];
		if (pton6(tmp, b) != 1)
			return false;
		u128 v = u128_from_be(b);
		if ((v >> 32) == 0xffff) { /* ::ffff:a.b.c.d */
			*out = v & 0xffffffffu;
			*is4 = true;
		} else {
			*out = v;
			*is4 = false;
		}
		return true;
	}
	uint8_t b[4];
	if (pton4(tmp, b) != 1)
		return false;
	*out = (u128)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
	*is4 = true;
	return true;
}

static bool parse_integer(const char *s, size_t n, u128 *out)
{
	if (n == 0 || n > 39)
		return false;
	u128 v = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		u128 d = (u128)(s[i] - '0');
		if (v > (U128_MAX - d) / 10)
			return false;
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

bool parse_address(const char *s, size_t n, u128 *out, bool *is4)
{
	trim(&s, &n);
	if (parse_integer(s, n, out)) {
		*is4 = *out <= 0xffffffffu;
		return true;
	}
	return parse_ip_text(s, n, out, is4);
}

bool parse_network(const char *s, size_t n, u128 *start, u128 *end, bool *is4)
{
	trim(&s, &n);
	const char *slash = memchr(s, '/', n);
	if (slash) {
		u128 ip;
		bool four;
		size_t plen = n - (size_t)(slash - s) - 1;
		if (!parse_ip_text(s, (size_t)(slash - s), &ip, &four) || plen == 0 || plen > 3)
			return false;
		int bits = 0;
		for (size_t i = 0; i < plen; i++) {
			char c = slash[1 + i];
			if (c < '0' || c > '9')
				return false;
			bits = bits * 10 + (c - '0');
		}
		/* a v4-mapped address keeps its IPv6 prefix length */
		bool mapped = four && memchr(s, ':', (size_t)(slash - s)) != NULL;
		if (mapped) {
			if (bits < 96 || bits > 128)
				return false;
			bits -= 96;
		}
		int depth;
		if (four) {
			if (bits > 32)
				return false;
			depth = bits + 96;
		} else {
			if (bits > 128)
				return false;
			depth = bits;
		}
		u128 host = depth >= 128 ? 0 : U128_MAX >> depth;
		*start = ip & ~host;
		*end = *start | host;
		*is4 = four;
		return true;
	}
	const char *dash = memchr(s, '-', n);
	if (dash) {
		u128 a, b;
		bool a4, b4;
		if (!parse_address(s, (size_t)(dash - s), &a, &a4) ||
		    !parse_address(dash + 1, n - (size_t)(dash - s) - 1, &b, &b4))
			return false;
		*start = a;
		*end = b;
		*is4 = a4 && b4;
		return true;
	}
	u128 a;
	bool four;
	if (!parse_address(s, n, &a, &four))
		return false;
	*start = *end = a;
	*is4 = four;
	return true;
}

const char *fmt_ip(u128 ip, bool is4, char out[64])
{
	if (is4)
		fmt_ipv4(out, (uint32_t)ip);
	else
		fmt_ipv6(out, ip);
	return out;
}

const char *fmt_net(u128 ip, int bits, char out[80])
{
	char a[64];
	if ((ip >> 32) == 0 && bits >= 96) {
		fmt_ipv4(a, (uint32_t)ip);
		snprintf(out, 80, "%s/%d", a, bits - 96);
	} else {
		fmt_ipv6(a, ip);
		snprintf(out, 80, "%s/%d", a, bits);
	}
	return out;
}

static int ctz128(u128 v)
{
	if (v == 0)
		return 128;
	uint64_t lo = (uint64_t)v;
	if (lo)
		return __builtin_ctzll(lo);
	return 64 + __builtin_ctzll((uint64_t)(v >> 64));
}

void range_cidrs(u128 start, u128 end, void (*fn)(void *ctx, u128 ip, int bits), void *ctx)
{
	for (;;) {
		int k = ctz128(start);
		for (;;) {
			u128 span = k >= 128 ? U128_MAX : ((u128)1 << k) - 1;
			if (span <= end - start)
				break;
			k--;
		}
		fn(ctx, start, 128 - k);
		u128 span = k >= 128 ? U128_MAX : ((u128)1 << k) - 1;
		if (end - start == span)
			return;
		start += span + 1;
	}
}
