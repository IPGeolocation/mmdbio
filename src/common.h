/* Shared helpers: errors, buffers, formatting. */
#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* printf-style checks: mingw's C99 printf (see the Makefile) takes the GNU
 * formats, %zu and %llu among them */
#if defined(__MINGW32__) && !defined(__clang__)
#define PRINTF_FMT(a, b) __attribute__((format(gnu_printf, a, b)))
#else
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#endif

#if defined(__SIZEOF_INT128__)
typedef unsigned __int128 u128;
#else
/* 32-bit targets: C23's _BitInt (GCC 14+, Clang 16+) */
typedef unsigned _BitInt(128) u128;
#endif

#define U128_MAX (~(u128)0)

/* Functions that can fail return 0 on success and -1 on failure, after
 * setting the message with err_set(). err_prefix() wraps the current message
 * the way Go's fmt.Errorf("%s: %w", ...) does. */
void err_set(const char *fmt, ...) PRINTF_FMT(1, 2);
void err_prefix(const char *fmt, ...) PRINTF_FMT(1, 2);
const char *err_msg(void);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);

/* Growable byte buffer. */
typedef struct {
	uint8_t *p;
	size_t len, cap;
} buf_t;

void buf_grow(buf_t *b, size_t extra);
static inline void buf_push(buf_t *b, uint8_t c)
{
	if (b->len == b->cap)
		buf_grow(b, 1);
	b->p[b->len++] = c;
}
void buf_append(buf_t *b, const void *data, size_t n);
void buf_printf(buf_t *b, const char *fmt, ...) PRINTF_FMT(2, 3);
const char *buf_cstr(buf_t *b); /* NUL-terminates without counting it */
void buf_free(buf_t *b);

/* 1234567 -> "1,234,567" (mmdbutil.FmtInt). */
const char *fmt_int(uint64_t n, char out[32]);

/* strconv.Quote of a byte string. */
void go_quote(buf_t *out, const uint8_t *s, size_t n);

/* IPv4 dotted quad, or RFC 5952 IPv6 as Go's netip.Addr.String prints it
 * (IPv4-mapped addresses as ::ffff:a.b.c.d). */
void fmt_ipv4(char out[16], uint32_t v);
void fmt_ipv6(char out[64], u128 v);
/* The streamwriter addr formatting: IPv4 when the value fits in 32 bits. */
const char *fmt_addr(u128 a, char out[64]);

/* time.Duration.String() of a whole number of seconds. */
void fmt_duration(char out[32], int64_t secs);
/* Go's "Mon Jan  2 15:04:05 2006" in local time. */
void fmt_now(char out[64]);
double mono_seconds(void);

/* strerror with Go's lowercase first letter ("no such file or directory"). */
const char *go_strerror(int errnum);

/* log.Printf to stderr with a trailing newline. */
void log_line(const char *fmt, ...) PRINTF_FMT(1, 2);
/* Go's default logger: "2006/01/02 15:04:05 message" on stderr. go_fatalf
 * then exits with status 1, as log.Fatalf does. */
void go_logf(const char *fmt, ...) PRINTF_FMT(1, 2);
void go_fatalf(const char *fmt, ...) PRINTF_FMT(1, 2) __attribute__((noreturn));

/* mmdbutil.Progress: a status line on stdout every `every` rows, rewritten
 * in place with \r; progress_line(p, "\n") prints the final one. */
typedef struct {
	const char *pass;
	uint64_t processed, inserted, skipped, failed, every;
	double start;
} progress;

void progress_line(const progress *p, const char *suffix);
void progress_tick(progress *p);

/* High-water RSS as "N MB", or "unknown". */
void peak_rss(char out[48]);

/* filepath.Join + Clean for a few elements. */
void path_join(buf_t *out, const char *const *parts, int n);
/* filepath.Dir */
void path_dir(buf_t *out, const char *path);

static inline u128 u128_from_be(const uint8_t b[16])
{
	u128 v = 0;
	for (int i = 0; i < 16; i++)
		v = v << 8 | b[i];
	return v;
}

static inline void u128_to_be(u128 v, uint8_t b[16])
{
	for (int i = 15; i >= 0; i--) {
		b[i] = (uint8_t)v;
		v >>= 8;
	}
}

#endif
