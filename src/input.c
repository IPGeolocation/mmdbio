#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#include "zlib.h"

#define RAW_BUF (1u << 20)

struct instream {
	const char *path;
	int fd;
	bool owned, gz, raw_eof, done;
	uint8_t *raw;        /* bytes read from fd */
	size_t raw_len, raw_pos;
	uint64_t raw_total;
	z_stream zs;
	bool zs_live;
};

static int fill_raw(instream *s)
{
	if (s->raw_eof)
		return 0;
	if (s->raw_pos < s->raw_len) {
		memmove(s->raw, s->raw + s->raw_pos, s->raw_len - s->raw_pos);
		s->raw_len -= s->raw_pos;
	} else {
		s->raw_len = 0;
	}
	s->raw_pos = 0;
	while (s->raw_len < RAW_BUF) {
		ssize_t n = read(s->fd, s->raw + s->raw_len, RAW_BUF - s->raw_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err_set("read %s: %s", s->path, go_strerror(errno));
			return -1;
		}
		if (n == 0) {
			s->raw_eof = true;
			break;
		}
		s->raw_len += (size_t)n;
		s->raw_total += (uint64_t)n;
		if (s->raw_len >= 4096)
			break;
	}
	return 0;
}

instream *in_open(const char *path)
{
	instream *s = xcalloc(1, sizeof *s);
	s->path = path;
	if (strcmp(path, "-") == 0) {
		s->fd = 0;
		s->path = "stdin";
	} else {
		s->fd = open(path, O_RDONLY | O_CLOEXEC);
		if (s->fd < 0) {
			err_set("open %s: %s", path, go_strerror(errno));
			free(s);
			return NULL;
		}
		s->owned = true;
	}
	s->raw = xmalloc(RAW_BUF);
	if (fill_raw(s) < 0) {
		in_close(s);
		return NULL;
	}
	if (s->raw_len >= 2 && s->raw[0] == 0x1f && s->raw[1] == 0x8b) {
		s->gz = true;
		if (inflateInit2(&s->zs, 15 + 16) != Z_OK) {
			err_set("%s: cannot start the gzip decoder", s->path);
			in_close(s);
			return NULL;
		}
		s->zs_live = true;
	}
	return s;
}

bool in_is_gzip(const instream *s) { return s->gz; }

uint64_t in_raw_bytes(const instream *s) { return s->raw_total - (s->raw_len - s->raw_pos); }

static ssize_t read_plain(instream *s, uint8_t *buf, size_t cap)
{
	if (s->raw_pos < s->raw_len) {
		size_t n = s->raw_len - s->raw_pos < cap ? s->raw_len - s->raw_pos : cap;
		memcpy(buf, s->raw + s->raw_pos, n);
		s->raw_pos += n;
		return (ssize_t)n;
	}
	if (s->raw_eof)
		return 0;
	for (;;) {
		ssize_t n = read(s->fd, buf, cap);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err_set("read %s: %s", s->path, go_strerror(errno));
			return -1;
		}
		if (n == 0)
			s->raw_eof = true;
		s->raw_total += (uint64_t)n;
		return n;
	}
}

static bool only_zeros(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

static ssize_t read_gzip(instream *s, uint8_t *buf, size_t cap)
{
	if (s->done)
		return 0;
	s->zs.next_out = buf;
	s->zs.avail_out = (uInt)(cap > (1u << 30) ? (1u << 30) : cap);
	while (s->zs.avail_out == (uInt)(cap > (1u << 30) ? (1u << 30) : cap)) {
		if (s->raw_pos == s->raw_len) {
			if (fill_raw(s) < 0)
				return -1;
			if (s->raw_len == 0) {
				err_set("%s: unexpected end of the gzip data", s->path);
				return -1;
			}
		}
		s->zs.next_in = s->raw + s->raw_pos;
		s->zs.avail_in = (uInt)(s->raw_len - s->raw_pos);
		int rc = inflate(&s->zs, Z_NO_FLUSH);
		s->raw_pos = s->raw_len - s->zs.avail_in;
		if (rc == Z_STREAM_END) {
			/* another member may follow; trailing zeros are padding */
			if (s->raw_pos == s->raw_len && fill_raw(s) < 0)
				return -1;
			if (s->raw_pos == s->raw_len || only_zeros(s->raw + s->raw_pos, s->raw_len - s->raw_pos)) {
				s->done = true;
				break;
			}
			if (s->raw_len - s->raw_pos < 2 || s->raw[s->raw_pos] != 0x1f || s->raw[s->raw_pos + 1] != 0x8b) {
				err_set("%s: data after the end of the gzip stream", s->path);
				return -1;
			}
			inflateReset(&s->zs);
			continue;
		}
		if (rc == Z_BUF_ERROR && s->zs.avail_in == 0 && s->raw_eof) {
			err_set("%s: unexpected end of the gzip data", s->path);
			return -1;
		}
		if (rc != Z_OK && rc != Z_BUF_ERROR) {
			err_set("%s: gzip: %s", s->path, s->zs.msg ? s->zs.msg : "invalid compressed data");
			return -1;
		}
	}
	return (ssize_t)((cap > (1u << 30) ? (1u << 30) : cap) - s->zs.avail_out);
}

ssize_t in_read(void *ctx, uint8_t *buf, size_t cap)
{
	instream *s = ctx;
	return s->gz ? read_gzip(s, buf, cap) : read_plain(s, buf, cap);
}

void in_close(instream *s)
{
	if (!s)
		return;
	if (s->zs_live)
		inflateEnd(&s->zs);
	if (s->owned)
		close(s->fd);
	free(s->raw);
	free(s);
}
