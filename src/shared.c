#include "shared.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"
#include "hashtab.h"

#define FLUSH_AT (1u << 20)

struct shared {
	char *tmp_dir;
	int fd;           /* -1 until the first flush */
	buf_t pend;       /* bytes not written yet; they start at flushed */
	uint64_t flushed;
	uint64_t *offs;   /* offs[i]: where value i starts; offs[n]: the end */
	size_t n, cap;
	ht16 ids;         /* SipHash of the encoding -> id */
};

shared *shared_new(const char *tmp_dir)
{
	shared *s = xmalloc(sizeof *s);
	memset(s, 0, sizeof *s);
	size_t dl = strlen(tmp_dir);
	s->tmp_dir = xmalloc(dl + 1);
	memcpy(s->tmp_dir, tmp_dir, dl + 1);
	s->fd = -1;
	ht16_init(&s->ids);
	return s;
}

void shared_free(shared *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s->tmp_dir);
	buf_free(&s->pend);
	free(s->offs);
	ht16_free(&s->ids);
	free(s);
}

static int flush(shared *s)
{
	if (s->fd < 0 && (s->fd = temp_file_fd(s->tmp_dir, "shared")) < 0)
		return -1;
	if (pwrite_all(s->fd, s->pend.p, s->pend.len, s->flushed) < 0)
		return -1;
	s->flushed += s->pend.len;
	s->pend.len = 0;
	return 0;
}

int shared_add(shared *s, const uint8_t *enc, size_t n, uint32_t *id)
{
	uint8_t h[16];
	siphash128(enc, n, h);
	if (s->n >= UINT32_MAX - 1) {
		err_set("more shared values than a build can keep");
		return -1;
	}
	uint32_t old;
	if (ht16_get_or_put(&s->ids, h, (uint32_t)s->n, &old)) {
		*id = old;
		return 0;
	}
	/* a value is written whole: it is either in the file or in pend */
	if (s->pend.len >= FLUSH_AT && flush(s) < 0)
		return -1;
	if (s->n + 2 > s->cap) {
		s->cap = s->cap ? s->cap * 2 : 1024;
		s->offs = xrealloc(s->offs, s->cap * sizeof *s->offs);
	}
	s->offs[s->n] = s->flushed + s->pend.len;
	buf_append(&s->pend, enc, n);
	*id = (uint32_t)s->n++;
	s->offs[s->n] = s->flushed + s->pend.len;
	return 0;
}

int shared_get(shared *s, uint32_t id, arena *a, mval **out)
{
	if (id >= s->n) {
		err_set("internal error: shared value %u is not in the store", id);
		return -1;
	}
	uint64_t at = s->offs[id];
	size_t len = (size_t)(s->offs[id + 1] - at);
	/* the decoded strings point into these bytes, which stay for the arena */
	uint8_t *own = arena_alloc(a, len + 1);
	if (at >= s->flushed) {
		memcpy(own, s->pend.p + (at - s->flushed), len);
	} else {
		size_t got = 0;
		while (got < len) {
			ssize_t r = pread(s->fd, own + got, len - got, (off_t)(at + got));
			if (r <= 0) {
				if (r < 0 && errno == EINTR)
					continue;
				err_set("read the shared values: %s", r < 0 ? go_strerror(errno) : "short read");
				return -1;
			}
			got += (size_t)r;
		}
	}
	mdec d = {.buf = own, .len = len, .shared = true};
	dnode *v;
	uint64_t next;
	if (mdec_value(&d, 0, a, &v, &next) < 0)
		return -1;
	*out = dnode_to_mval(a, v);
	return 0;
}

bool shared_find(const shared *s, const uint8_t *enc, size_t n, uint32_t *id)
{
	uint8_t h[16];
	siphash128(enc, n, h);
	return ht16_get(&s->ids, h, id);
}

size_t shared_count(const shared *s) { return s->n; }

uint64_t shared_bytes(const shared *s) { return s->n ? s->offs[s->n] : 0; }
