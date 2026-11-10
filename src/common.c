#include "common.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "compat.h"

#include "utf8.h"

static char err_buf[8192];

void err_set(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
}

void err_prefix(const char *fmt, ...)
{
	char prefix[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(prefix, sizeof prefix, fmt, ap);
	va_end(ap);
	size_t pl = strlen(prefix), ol = strlen(err_buf);
	if (pl + 2 + ol >= sizeof err_buf)
		ol = sizeof err_buf - pl - 3;
	memmove(err_buf + pl + 2, err_buf, ol);
	err_buf[pl + 2 + ol] = 0;
	memcpy(err_buf, prefix, pl);
	err_buf[pl] = ':';
	err_buf[pl + 1] = ' ';
}

const char *err_msg(void) { return err_buf; }

static void out_of_memory(void)
{
	fputs("FAILED: out of memory\n", stderr);
	exit(1);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		out_of_memory();
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		out_of_memory();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		out_of_memory();
	return p;
}

void buf_grow(buf_t *b, size_t extra)
{
	size_t need = b->len + extra;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap = cap < ((size_t)1 << 30) ? cap * 2 : cap + cap / 4;
	b->p = xrealloc(b->p, cap);
	b->cap = cap;
}

void buf_append(buf_t *b, const void *data, size_t n)
{
	if (!n)
		return;
	buf_grow(b, n);
	memcpy(b->p + b->len, data, n);
	b->len += n;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	buf_grow(b, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

const char *buf_cstr(buf_t *b)
{
	buf_grow(b, 1);
	b->p[b->len] = 0;
	return (const char *)b->p;
}

void buf_free(buf_t *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

const char *fmt_int(uint64_t n, char out[32])
{
	char digits[24];
	int nd = snprintf(digits, sizeof digits, "%llu", (unsigned long long)n);
	int o = 0;
	for (int i = 0; i < nd; i++) {
		if (i > 0 && (nd - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = digits[i];
	}
	out[o] = 0;
	return out;
}

/* An approximation of unicode.IsPrint for runes above ASCII: the control,
 * format, separator and private-use ranges a CSV is likely to carry. */
static bool go_is_print(uint32_t r)
{
	if (r < 0x20 || r == 0x7f)
		return false;
	if (r < 0x7f)
		return true;
	if (r >= 0x80 && r <= 0x9f)
		return false;
	if (r == 0xad || r == 0x2028 || r == 0x2029 || r == 0xfeff)
		return false;
	if ((r >= 0x200b && r <= 0x200f) || (r >= 0x202a && r <= 0x202e) || (r >= 0x2060 && r <= 0x2064))
		return false;
	if ((r >= 0xe000 && r <= 0xf8ff) || r >= 0xf0000)
		return false;
	if (r == 0xfffd)
		return true;
	return true;
}

void go_quote(buf_t *out, const uint8_t *s, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	buf_push(out, '"');
	size_t i = 0;
	while (i < n) {
		uint32_t r;
		int size = go_decode_rune(s + i, n - i, &r);
		if (r == UTF8_RUNE_ERROR && size == 1) {
			buf_append(out, "\\x", 2);
			buf_push(out, (uint8_t)hex[s[i] >> 4]);
			buf_push(out, (uint8_t)hex[s[i] & 15]);
			i++;
			continue;
		}
		if (r == '"' || r == '\\') {
			buf_push(out, '\\');
			buf_push(out, (uint8_t)r);
		} else if (go_is_print(r)) {
			buf_append(out, s + i, (size_t)size);
		} else {
			switch (r) {
			case '\a': buf_append(out, "\\a", 2); break;
			case '\b': buf_append(out, "\\b", 2); break;
			case '\f': buf_append(out, "\\f", 2); break;
			case '\n': buf_append(out, "\\n", 2); break;
			case '\r': buf_append(out, "\\r", 2); break;
			case '\t': buf_append(out, "\\t", 2); break;
			case '\v': buf_append(out, "\\v", 2); break;
			default:
				if (r < ' ' || r == 0x7f)
					buf_printf(out, "\\x%c%c", hex[r >> 4], hex[r & 15]);
				else if (r < 0x10000)
					buf_printf(out, "\\u%04x", r);
				else
					buf_printf(out, "\\U%08x", r);
			}
		}
		i += (size_t)size;
	}
	buf_push(out, '"');
}

void fmt_ipv4(char out[16], uint32_t v)
{
	snprintf(out, 16, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
}

void fmt_ipv6(char out[64], u128 v)
{
	uint16_t g[8];
	for (int i = 0; i < 8; i++)
		g[i] = (uint16_t)(v >> (112 - 16 * i));
	if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xffff) {
		char v4[16];
		fmt_ipv4(v4, (uint32_t)v);
		snprintf(out, 64, "::ffff:%s", v4);
		return;
	}
	int zs = 255, ze = 255;
	for (int i = 0; i < 8; i++) {
		int j = i;
		while (j < 8 && g[j] == 0)
			j++;
		if (j - i >= 2 && j - i > ze - zs) {
			zs = i;
			ze = j;
		}
	}
	int o = 0;
	for (int i = 0; i < 8; i++) {
		if (i == zs) {
			out[o++] = ':';
			out[o++] = ':';
			i = ze;
			if (i >= 8)
				break;
		} else if (i > 0) {
			out[o++] = ':';
		}
		o += snprintf(out + o, (size_t)(64 - o), "%x", g[i]);
	}
	out[o] = 0;
}

const char *fmt_addr(u128 a, char out[64])
{
	if ((a >> 32) == 0) {
		fmt_ipv4(out, (uint32_t)a);
		return out;
	}
	fmt_ipv6(out, a);
	return out;
}

void fmt_duration(char out[32], int64_t secs)
{
	if (secs <= 0) {
		snprintf(out, 32, "0s");
		return;
	}
	int64_t h = secs / 3600, m = secs % 3600 / 60, s = secs % 60;
	if (h > 0)
		snprintf(out, 32, "%lldh%lldm%llds", (long long)h, (long long)m, (long long)s);
	else if (m > 0)
		snprintf(out, 32, "%lldm%llds", (long long)m, (long long)s);
	else
		snprintf(out, 32, "%llds", (long long)s);
}

void fmt_now(char out[64])
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	/* %e is not everywhere */
	char a[16], b[16], hms[16], year[8];
	strftime(a, sizeof a, "%a", &tm);
	strftime(b, sizeof b, "%b", &tm);
	strftime(hms, sizeof hms, "%H:%M:%S", &tm);
	strftime(year, sizeof year, "%Y", &tm);
	snprintf(out, 64, "%s %s %2d %s %s", a, b, tm.tm_mday, hms, year);
}

double mono_seconds(void)
{
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

const char *go_strerror(int errnum)
{
	static char msg[256];
	snprintf(msg, sizeof msg, "%s", strerror(errnum));
	if (msg[0] >= 'A' && msg[0] <= 'Z')
		msg[0] = (char)(msg[0] - 'A' + 'a');
	return msg;
}

void log_line(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void go_vlog(const char *fmt, va_list ap)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char stamp[32];
	strftime(stamp, sizeof stamp, "%Y/%m/%d %H:%M:%S ", &tm);
	fputs(stamp, stderr);
	vfprintf(stderr, fmt, ap);
	size_t n = strlen(fmt);
	if (n == 0 || fmt[n - 1] != '\n')
		fputc('\n', stderr);
	fflush(stderr);
}

void go_logf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	go_vlog(fmt, ap);
	va_end(ap);
}

void go_fatalf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	go_vlog(fmt, ap);
	va_end(ap);
	fflush(stdout);
	exit(1);
}

void progress_line(const progress *p, const char *suffix)
{
	double elapsed = mono_seconds() - p->start;
	if (elapsed <= 0)
		elapsed = 0.000001;
	char a[32], b[32], c[32], d[32];
	printf("[%s] processed=%s inserted=%s skipped=%s failed=%s rate=%.0f/s elapsed=%.0fs%s", p->pass,
	       fmt_int(p->processed, a), fmt_int(p->inserted, b), fmt_int(p->skipped, c), fmt_int(p->failed, d),
	       (double)p->inserted / elapsed, elapsed, suffix);
	fflush(stdout);
}

void progress_tick(progress *p)
{
	p->processed++;
	if (p->every && p->processed % p->every == 0)
		progress_line(p, "\r");
}

void peak_rss(char out[48])
{
	uint64_t bytes, minflt, majflt;
	process_usage(&bytes, &minflt, &majflt);
	char n[32];
	snprintf(out, 48, "%s MB", fmt_int(bytes >> 20, n));
}

/* filepath.Clean, Unix rules. */
static void path_clean(buf_t *out, const char *path)
{
	size_t n = strlen(path);
	out->len = 0;
	if (n == 0) {
		buf_push(out, '.');
		buf_cstr(out);
		return;
	}
	bool rooted = path[0] == '/';
	size_t r = 0, dotdot = 0;
	if (rooted) {
		buf_push(out, '/');
		r = dotdot = 1;
	}
	while (r < n) {
		if (path[r] == '/') {
			r++;
		} else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
			r++;
		} else if (path[r] == '.' && path[r + 1] == '.' && (r + 2 == n || path[r + 2] == '/')) {
			r += 2;
			if (out->len > dotdot) {
				out->len--;
				while (out->len > dotdot && out->p[out->len] != '/')
					out->len--;
			} else if (!rooted) {
				if (out->len > 0)
					buf_push(out, '/');
				buf_push(out, '.');
				buf_push(out, '.');
				dotdot = out->len;
			}
		} else {
			if ((rooted && out->len != 1) || (!rooted && out->len != 0))
				buf_push(out, '/');
			for (; r < n && path[r] != '/'; r++)
				buf_push(out, (uint8_t)path[r]);
		}
	}
	if (out->len == 0)
		buf_push(out, '.');
	buf_cstr(out);
}

void path_join(buf_t *out, const char *const *parts, int n)
{
	buf_t raw = {0};
	bool any = false;
	for (int i = 0; i < n; i++) {
		if (!parts[i][0])
			continue;
		if (any)
			buf_push(&raw, '/');
		buf_append(&raw, parts[i], strlen(parts[i]));
		any = true;
	}
	if (!any) {
		out->len = 0;
		buf_cstr(out);
		buf_free(&raw);
		return;
	}
	path_clean(out, buf_cstr(&raw));
	buf_free(&raw);
}

void path_dir(buf_t *out, const char *path)
{
	size_t i = strlen(path);
	while (i > 0 && path[i - 1] != '/')
		i--;
	buf_t head = {0};
	buf_append(&head, path, i);
	path_clean(out, buf_cstr(&head));
	buf_free(&head);
}
