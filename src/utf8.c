#include "utf8.h"

int go_decode_rune(const uint8_t *s, size_t n, uint32_t *r)
{
	*r = UTF8_RUNE_ERROR;
	if (n == 0)
		return 0;
	uint8_t c = s[0];
	if (c < 0x80) {
		*r = c;
		return 1;
	}
	int need;
	uint8_t lo = 0x80, hi = 0xBF;
	uint32_t v;
	if (c >= 0xC2 && c <= 0xDF) {
		need = 1;
		v = c & 0x1F;
	} else if (c >= 0xE0 && c <= 0xEF) {
		need = 2;
		v = c & 0x0F;
		if (c == 0xE0)
			lo = 0xA0;
		else if (c == 0xED)
			hi = 0x9F;
	} else if (c >= 0xF0 && c <= 0xF4) {
		need = 3;
		v = c & 0x07;
		if (c == 0xF0)
			lo = 0x90;
		else if (c == 0xF4)
			hi = 0x8F;
	} else {
		return 1;
	}
	if (n < (size_t)need + 1)
		return 1;
	uint8_t c1 = s[1];
	if (c1 < lo || c1 > hi)
		return 1;
	v = v << 6 | (c1 & 0x3F);
	for (int i = 2; i <= need; i++) {
		if (s[i] < 0x80 || s[i] > 0xBF)
			return 1;
		v = v << 6 | (s[i] & 0x3F);
	}
	*r = v;
	return need + 1;
}

int go_decode_last_rune(const uint8_t *s, size_t n, uint32_t *r)
{
	*r = UTF8_RUNE_ERROR;
	if (n == 0)
		return 0;
	size_t end = n, start = n - 1;
	if (s[start] < 0x80) {
		*r = s[start];
		return 1;
	}
	size_t lim = end >= 4 ? end - 4 : 0;
	while (start > lim && (s[start] & 0xC0) == 0x80)
		start--;
	if (start >= lim && (s[start] & 0xC0) == 0x80) {
		/* only continuation bytes in reach */
		return 1;
	}
	uint32_t v;
	int size = go_decode_rune(s + start, end - start, &v);
	if (start + (size_t)size != end)
		return 1;
	*r = v;
	return size;
}

bool utf8_valid(const uint8_t *s, size_t n)
{
	size_t i = 0;
	while (i < n) {
		if (s[i] < 0x80) {
			i++;
			continue;
		}
		uint32_t r;
		int size = go_decode_rune(s + i, n - i, &r);
		if (r == UTF8_RUNE_ERROR && size == 1)
			return false;
		i += (size_t)size;
	}
	return true;
}

bool go_is_space(uint32_t r)
{
	switch (r) {
	case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
	case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
	case 0x202F: case 0x205F: case 0x3000:
		return true;
	}
	return r >= 0x2000 && r <= 0x200A;
}

void trim_space(const uint8_t **sp, size_t *np)
{
	const uint8_t *s = *sp;
	size_t n = *np, start = 0;
	while (start < n) {
		uint32_t r;
		int size = go_decode_rune(s + start, n - start, &r);
		if (!go_is_space(r))
			break;
		start += (size_t)size;
	}
	size_t end = n;
	while (end > start) {
		uint32_t r;
		int size = go_decode_last_rune(s + start, end - start, &r);
		if (!go_is_space(r))
			break;
		end -= (size_t)size;
	}
	*sp = s + start;
	*np = end - start;
}

void perl_decode(const uint8_t **sp, size_t *np, buf_t *scratch)
{
	const uint8_t *s = *sp;
	size_t n = *np;
	if (utf8_valid(s, n))
		return;
	static const char hex[] = "0123456789ABCDEF";
	size_t start = scratch->len;
	buf_grow(scratch, n * 4);
	for (size_t i = 0; i < n;) {
		uint32_t r;
		int size = go_decode_rune(s + i, n - i, &r);
		if (r == UTF8_RUNE_ERROR && size == 1) {
			uint8_t esc[4] = {'\\', 'x', (uint8_t)hex[s[i] >> 4], (uint8_t)hex[s[i] & 15]};
			buf_append(scratch, esc, 4);
		} else {
			buf_append(scratch, s + i, (size_t)size);
		}
		i += (size_t)size;
	}
	/* scratch may have moved: callers must not hold older views into it */
	*sp = scratch->p + start;
	*np = scratch->len - start;
}
