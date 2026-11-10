/* The build: rows in any order, overlapping or not, to an MMDB file.
 *
 * 1. Ingest. Each row's record is encoded canonically; identical records
 *    share an id, and each new one is appended to a record store on disk.
 *    The rows' ranges (start, end, row number, record id) are sorted in
 *    memory-sized runs, spilled to disk when the buffer fills.
 * 2. Resolve. The runs are merged by start address and a sweep splits the
 *    ranges where they overlap: the later row wins (merge "replace"), the
 *    earlier one ("keep"), or the records are merged ("toplevel",
 *    "recurse"). The pieces come out sorted and disjoint.
 * 3. The streaming writer counts the tree from those pieces, keeping them
 *    in a replay file, and writes the file from the replay, reading each
 *    record back from the store when the tree first uses it. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build.h"
#include "compat.h"
#include "mmdb_enc.h"
#include "mmdb_read.h"
#include "netaddr.h"
#include "rows.h"
#include "tabular.h"
#include "sha256.h"
#include "shared.h"
#include "streamwriter.h"

#define IO_BUF (1u << 20)
#define MAX_WARNINGS 20

/* ---- temporary files and buffered I/O ---- */

static int temp_fd(const char *dir, const char *tag) { return temp_file_fd(dir, tag); }

typedef struct {
	int fd;
	uint8_t *buf;
	size_t len;
	uint64_t off; /* file offset of buf[0] */
} bwriter;

static void bw_init(bwriter *w, int fd)
{
	w->fd = fd;
	w->buf = xmalloc(IO_BUF);
	w->len = 0;
	w->off = 0;
}

static int bw_flush(bwriter *w)
{
	if (w->len && pwrite_all(w->fd, w->buf, w->len, w->off) < 0)
		return -1;
	w->off += w->len;
	w->len = 0;
	return 0;
}

static int bw_write(bwriter *w, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		if (w->len == IO_BUF && bw_flush(w) < 0)
			return -1;
		size_t k = IO_BUF - w->len < n ? IO_BUF - w->len : n;
		memcpy(w->buf + w->len, b, k);
		w->len += k;
		b += k;
		n -= k;
	}
	return 0;
}

static inline uint64_t bw_pos(const bwriter *w) { return w->off + w->len; }

static void put_varint(buf_t *b, u128 x)
{
	while (x >= 0x80) {
		buf_push(b, (uint8_t)(x | 0x80));
		x >>= 7;
	}
	buf_push(b, (uint8_t)x);
}

typedef struct {
	int fd;
	uint64_t pos, end;
	uint8_t *buf;
	size_t len, i;
} breader;

static void br_init(breader *r, int fd, uint64_t start, uint64_t end, size_t cap)
{
	r->fd = fd;
	r->pos = start;
	r->end = end;
	r->buf = xmalloc(cap);
	r->len = r->i = 0;
}

static int br_fill(breader *r, size_t cap)
{
	size_t want = r->end - r->pos < cap ? (size_t)(r->end - r->pos) : cap;
	size_t got = 0;
	while (got < want) {
		ssize_t n = pread(r->fd, r->buf + got, want - got, (off_t)(r->pos + got));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err_set("read a temporary file: %s", go_strerror(errno));
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	r->pos += got;
	r->len = got;
	r->i = 0;
	return 0;
}

/* 1 with a value, 0 at the end of the range, -1 on error */
static int br_varint(breader *r, size_t cap, u128 *out)
{
	u128 x = 0;
	int shift = 0;
	for (;;) {
		if (r->i == r->len) {
			if (r->pos >= r->end) {
				if (shift == 0)
					return 0;
				err_set("internal error: a temporary file ends inside a number");
				return -1;
			}
			if (br_fill(r, cap) < 0)
				return -1;
			if (r->len == 0) {
				err_set("internal error: a temporary file is shorter than written");
				return -1;
			}
		}
		uint8_t b = r->buf[r->i++];
		x |= (u128)(b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
		if (shift > 126) {
			err_set("internal error: a bad number in a temporary file");
			return -1;
		}
	}
	*out = x;
	return 1;
}

/* ---- the record store ---- */

typedef struct {
	int fd;
	bwriter w;
	uint64_t *offs; /* offs[i]: where record i starts; offs[n]: the end */
	size_t n, cap;
	buf_t rd;
} store;

static int store_add(store *s, const uint8_t *p, size_t n)
{
	if (s->n + 2 > s->cap) {
		s->cap = s->cap ? s->cap * 2 : 4096;
		s->offs = xrealloc(s->offs, s->cap * sizeof *s->offs);
	}
	s->offs[s->n] = bw_pos(&s->w);
	if (bw_write(&s->w, p, n) < 0)
		return -1;
	s->n++;
	s->offs[s->n] = bw_pos(&s->w);
	return 0;
}

static int store_get(store *s, uint32_t id, mdec *out)
{
	if (id >= s->n) {
		err_set("internal error: record %u is not in the store", id);
		return -1;
	}
	if (s->w.len && bw_flush(&s->w) < 0)
		return -1;
	uint64_t a = s->offs[id], b = s->offs[id + 1];
	s->rd.len = 0;
	buf_grow(&s->rd, (size_t)(b - a) + 1);
	size_t got = 0;
	while (got < b - a) {
		ssize_t n = pread(s->fd, s->rd.p + got, (size_t)(b - a) - got, (off_t)(a + got));
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			err_set("read the record store: %s", n < 0 ? go_strerror(errno) : "short read");
			return -1;
		}
		got += (size_t)n;
	}
	s->rd.len = got;
	out->buf = s->rd.p;
	out->len = got;
	return 0;
}

static int load_record(store *s, arena *a, uint32_t id, mval **out)
{
	mdec d;
	if (store_get(s, id, &d) < 0)
		return -1;
	/* the value's strings point into its bytes: give them their own copy,
	 * as the next load reuses the read buffer */
	uint8_t *own = arena_alloc(a, (size_t)d.len + 1);
	if (d.len)
		memcpy(own, d.buf, (size_t)d.len);
	d.buf = own;
	d.shared = true;
	dnode *v;
	uint64_t next;
	if (mdec_value(&d, 0, a, &v, &next) < 0)
		return -1;
	*out = dnode_to_mval(a, v);
	return 0;
}

/* ---- ranges ---- */

typedef struct {
	u128 start, end;
	uint64_t seq;
	uint32_t rec;
} entry;

static int cmp_entry(const void *a, const void *b)
{
	const entry *x = a, *y = b;
	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

typedef struct {
	uint64_t start, end;
} run_info;

typedef struct {
	breader r;
	entry cur;
	u128 prev;
} run_reader;

typedef struct {
	/* in memory, or k runs on disk */
	entry *mem;
	size_t nmem, imem;
	run_reader *rr;
	size_t nrr;
	size_t *heap; /* run readers by (start, seq) */
	size_t nheap;
} source;

static int rr_next(run_reader *r, bool *ok)
{
	u128 d, len, seq, rec;
	int rc = br_varint(&r->r, IO_BUF, &d);
	if (rc <= 0) {
		*ok = false;
		return rc;
	}
	if (br_varint(&r->r, IO_BUF, &len) <= 0 || br_varint(&r->r, IO_BUF, &seq) <= 0 ||
	    br_varint(&r->r, IO_BUF, &rec) <= 0) {
		err_set("internal error: a run ends inside an entry");
		return -1;
	}
	r->prev += d;
	r->cur = (entry){r->prev, r->prev + len, (uint64_t)seq, (uint32_t)rec};
	*ok = true;
	return 0;
}

static bool rr_less(const source *s, size_t a, size_t b)
{
	return cmp_entry(&s->rr[a].cur, &s->rr[b].cur) < 0;
}

static void heap_down(source *s, size_t i)
{
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		if (l < s->nheap && rr_less(s, s->heap[l], s->heap[m]))
			m = l;
		if (r < s->nheap && rr_less(s, s->heap[r], s->heap[m]))
			m = r;
		if (m == i)
			return;
		size_t t = s->heap[i];
		s->heap[i] = s->heap[m];
		s->heap[m] = t;
		i = m;
	}
}

static int source_next(source *s, entry *out, bool *ok)
{
	if (s->mem) {
		*ok = s->imem < s->nmem;
		if (*ok)
			*out = s->mem[s->imem++];
		return 0;
	}
	if (s->nheap == 0) {
		*ok = false;
		return 0;
	}
	size_t top = s->heap[0];
	*out = s->rr[top].cur;
	*ok = true;
	bool more;
	if (rr_next(&s->rr[top], &more) < 0)
		return -1;
	if (!more)
		s->heap[0] = s->heap[--s->nheap];
	heap_down(s, 0);
	return 0;
}

/* The ranges of the ingest: sorted in memory, spilled as runs when full. */
typedef struct {
	entry *buf;
	size_t n, cap;
	const char *tmp;
	int fd;
	bwriter w;
	run_info *runs;
	size_t nruns;
} runset;

static int runs_spill(runset *rs)
{
	if (rs->n > 1)
		qsort(rs->buf, rs->n, sizeof *rs->buf, cmp_entry);
	if (rs->fd < 0) {
		if ((rs->fd = temp_fd(rs->tmp, "runs")) < 0)
			return -1;
		bw_init(&rs->w, rs->fd);
	}
	rs->runs = xrealloc(rs->runs, (rs->nruns + 1) * sizeof *rs->runs);
	rs->runs[rs->nruns].start = bw_pos(&rs->w);
	buf_t v = {0};
	u128 prev = 0;
	int rc = 0;
	for (size_t i = 0; i < rs->n && rc == 0; i++) {
		v.len = 0;
		put_varint(&v, rs->buf[i].start - prev);
		put_varint(&v, rs->buf[i].end - rs->buf[i].start);
		put_varint(&v, rs->buf[i].seq);
		put_varint(&v, rs->buf[i].rec);
		prev = rs->buf[i].start;
		rc = bw_write(&rs->w, v.p, v.len);
	}
	buf_free(&v);
	rs->runs[rs->nruns++].end = bw_pos(&rs->w);
	rs->n = 0;
	return rc;
}

static int runs_push(runset *rs, u128 start, u128 end, uint64_t seq, uint32_t rec)
{
	if (rs->n == rs->cap && runs_spill(rs) < 0)
		return -1;
	rs->buf[rs->n++] = (entry){start, end, seq, rec};
	return 0;
}

/* Hands the ranges to src: the buffer itself, or a merge of the runs. */
static int runs_finish(runset *rs, source *src)
{
	if (rs->nruns == 0) {
		if (rs->n > 1)
			qsort(rs->buf, rs->n, sizeof *rs->buf, cmp_entry);
		src->mem = rs->buf;
		src->nmem = rs->n;
		return 0;
	}
	if (rs->n && runs_spill(rs) < 0)
		return -1;
	if (bw_flush(&rs->w) < 0)
		return -1;
	free(rs->buf);
	rs->buf = NULL;
	src->rr = xcalloc(rs->nruns, sizeof *src->rr);
	src->heap = xmalloc(rs->nruns * sizeof *src->heap);
	src->nrr = rs->nruns;
	for (size_t i = 0; i < rs->nruns; i++) {
		br_init(&src->rr[i].r, rs->fd, rs->runs[i].start, rs->runs[i].end, IO_BUF);
		bool ok;
		if (rr_next(&src->rr[i], &ok) < 0)
			return -1;
		if (ok)
			src->heap[src->nheap++] = i;
	}
	for (size_t i = src->nheap; i-- > 0;)
		heap_down(src, i);
	return 0;
}

/* ---- the sweep ---- */

typedef struct build build;

typedef struct {
	source *src;
	merge_mode mode;
	entry ahead;
	bool has_ahead, finished;
	u128 pos;
	entry *act; /* replace/keep: a heap by priority; merge modes: a list */
	size_t nact, cap;
	build *b;
} sweep;

static bool prio_over(const sweep *s, const entry *a, const entry *b)
{
	return s->mode == MERGE_KEEP ? a->seq < b->seq : a->seq > b->seq;
}

static void act_push_raw(sweep *s, const entry *e)
{
	if (s->nact == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 64;
		s->act = xrealloc(s->act, s->cap * sizeof *s->act);
	}
	s->act[s->nact++] = *e;
}

static void heap_up(sweep *s, size_t i)
{
	while (i > 0) {
		size_t p = (i - 1) / 2;
		if (!prio_over(s, &s->act[i], &s->act[p]))
			return;
		entry t = s->act[i];
		s->act[i] = s->act[p];
		s->act[p] = t;
		i = p;
	}
}

static void act_heap_down(sweep *s, size_t i)
{
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		if (l < s->nact && prio_over(s, &s->act[l], &s->act[m]))
			m = l;
		if (r < s->nact && prio_over(s, &s->act[r], &s->act[m]))
			m = r;
		if (m == i)
			return;
		entry t = s->act[i];
		s->act[i] = s->act[m];
		s->act[m] = t;
		i = m;
	}
}

static bool merging(const sweep *s) { return s->mode == MERGE_TOPLEVEL || s->mode == MERGE_RECURSE; }

static void act_add(sweep *s, const entry *e)
{
	if (merging(s)) {
		act_push_raw(s, e);
		return;
	}
	/* a range the current winner outranks and outlasts can never show */
	if (s->nact && prio_over(s, &s->act[0], e) && s->act[0].end >= e->end)
		return;
	act_push_raw(s, e);
	heap_up(s, s->nact - 1);
}

/* takes every range that starts at or before pos */
static int take_ahead(sweep *s)
{
	while (s->has_ahead && s->ahead.start <= s->pos) {
		act_add(s, &s->ahead);
		bool ok;
		if (source_next(s->src, &s->ahead, &ok) < 0)
			return -1;
		s->has_ahead = ok;
	}
	return 0;
}

static int merged_record(sweep *s, uint32_t *rec);

/* The next piece: sorted, disjoint, with its record. */
static int sweep_next(sweep *s, u128 *start, u128 *end, uint32_t *rec, bool *ok)
{
	*ok = false;
	for (;;) {
		if (s->finished)
			return 0;
		if (merging(s)) {
			size_t w = 0;
			for (size_t i = 0; i < s->nact; i++)
				if (!(s->act[i].end < s->pos))
					s->act[w++] = s->act[i];
			s->nact = w;
		} else {
			while (s->nact && s->act[0].end < s->pos) {
				s->act[0] = s->act[--s->nact];
				act_heap_down(s, 0);
			}
		}
		if (s->nact == 0) {
			if (!s->has_ahead)
				return 0;
			s->pos = s->ahead.start;
			if (take_ahead(s) < 0)
				return -1;
			continue;
		}
		u128 lim;
		if (merging(s)) {
			lim = s->act[0].end;
			for (size_t i = 1; i < s->nact; i++)
				if (s->act[i].end < lim)
					lim = s->act[i].end;
			if (merged_record(s, rec) < 0)
				return -1;
		} else {
			lim = s->act[0].end;
			*rec = s->act[0].rec;
		}
		if (s->has_ahead && s->ahead.start - 1 < lim)
			lim = s->ahead.start - 1;
		*start = s->pos;
		*end = lim;
		*ok = true;
		if (lim == U128_MAX) {
			s->finished = true;
			return 0;
		}
		s->pos = lim + 1;
		return take_ahead(s);
	}
}

/* ---- the build ---- */

struct build {
	build_opts *o;
	sw_writer *w;
	store st;
	arena a;
	buf_t key;
	sweep sw;
	/* the replay of the counting pass's pieces */
	int replay_fd;
	bwriter replay;
	breader replay_r;
	u128 replay_prev; /* end + 1 of the previous piece */
	bool replay_first;
	ht16 merges; /* hash of a list of record ids -> the merged record */
	shared *sh;  /* the big values records repeat, kept once (shared.h) */
	buf_t mkey;  /* a merged value's encoding (reshared) */
	mval *supply;
	uint64_t pieces;
};

/* ---- merging records (mmdbwriter's TopLevelMergeWith, DeepMergeWith) ---- */

static int key_cmp(const mval *a, const mval *b)
{
	size_t n = a->n < b->n ? a->n : b->n;
	int c = n ? memcmp(a->s, b->s, n) : 0;
	return c ? c : (a->n < b->n ? -1 : a->n > b->n);
}

static int merge_values(build *b, const mval *old, const mval *new_, bool deep, bool top, mval *out);

/* A deep merge looks inside a shared value: its content (whose own shared
 * values stay T_SHARED). */
static int opened(build *b, const mval *v, const mval **out)
{
	if (v->type != T_SHARED) {
		*out = v;
		return 0;
	}
	mval *c;
	if (shared_get(b->sh, (uint32_t)v->u, &b->a, &c) < 0)
		return -1;
	*out = c;
	return 0;
}

/* A value a deep merge made becomes the shared value with its content, if
 * there is one, so the record stays equal to records that hold it unmerged. */
static int reshared(build *b, mval *v)
{
	b->mkey.len = 0;
	if (mval_canonical(&b->mkey, v) < 0)
		return -1;
	uint32_t id;
	if (b->mkey.len >= SHARE_MIN && shared_find(b->sh, b->mkey.p, b->mkey.len, &id))
		*v = (mval){.type = T_SHARED, .u = id};
	return 0;
}

static int merge_values(build *b, const mval *old, const mval *new_, bool deep, bool top, mval *out)
{
	arena *ar = &b->a;
	if (deep && (old->type == T_SHARED || new_->type == T_SHARED)) {
		if (old->type == new_->type && old->u == new_->u) {
			*out = *new_; /* a value merged with itself */
			return 0;
		}
		const mval *o, *n;
		if (opened(b, old, &o) < 0 || opened(b, new_, &n) < 0)
			return -1;
		if ((o->type == T_MAP && n->type == T_MAP) || (o->type == T_ARRAY && n->type == T_ARRAY))
			return merge_values(b, o, n, true, top, out);
		*out = *new_;
		return 0;
	}
	if (old->type == T_MAP && new_->type == T_MAP) {
		mval *items = arena_alloc(ar, sizeof *items * (2 * ((size_t)old->n + new_->n) + 2));
		uint32_t i = 0, j = 0, k = 0;
		while (i < old->n || j < new_->n) {
			int c = i == old->n ? 1 : j == new_->n ? -1 : key_cmp(&old->items[2 * i], &new_->items[2 * j]);
			if (c < 0) {
				items[2 * k] = old->items[2 * i];
				items[2 * k + 1] = old->items[2 * i + 1];
				i++;
			} else if (c > 0) {
				items[2 * k] = new_->items[2 * j];
				items[2 * k + 1] = new_->items[2 * j + 1];
				j++;
			} else {
				items[2 * k] = new_->items[2 * j];
				if (!deep)
					items[2 * k + 1] = new_->items[2 * j + 1];
				else if (merge_values(b, &old->items[2 * i + 1], &new_->items[2 * j + 1], true, false,
						      &items[2 * k + 1]) < 0)
					return -1;
				i++;
				j++;
			}
			k++;
		}
		*out = (mval){.type = T_MAP, .n = k, .items = items};
		return deep && !top ? reshared(b, out) : 0;
	}
	if (deep && old->type == T_ARRAY && new_->type == T_ARRAY) {
		uint32_t n = old->n > new_->n ? old->n : new_->n;
		mval *items = arena_alloc(ar, sizeof *items * ((size_t)n + 1));
		for (uint32_t i = 0; i < n; i++) {
			if (i >= new_->n)
				items[i] = old->items[i];
			else if (i >= old->n)
				items[i] = new_->items[i];
			else if (merge_values(b, &old->items[i], &new_->items[i], true, false, &items[i]) < 0)
				return -1;
		}
		*out = (mval){.type = T_ARRAY, .n = n, .items = items};
		return top ? 0 : reshared(b, out);
	}
	*out = *new_;
	return 0;
}

/* the record in b->key (its canonical encoding) to its id */
static int intern_key(build *b, uint32_t *id)
{
	sw_records *r = sw_get_records(b->w);
	size_t before = r->n;
	if (sw_intern(r, b->key.p, b->key.len, NULL, NULL, id) < 0)
		return -1;
	if (r->n > before && store_add(&b->st, b->key.p, b->key.len) < 0)
		return -1;
	return 0;
}

static int intern(build *b, const mval *rec, uint32_t *id)
{
	b->key.len = 0;
	if (mval_canonical(&b->key, rec) < 0)
		return -1;
	return intern_key(b, id);
}

static int cmp_seq(const void *a, const void *b)
{
	const entry *x = a, *y = b;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int merged_record(sweep *s, uint32_t *rec)
{
	if (s->nact == 1) {
		*rec = s->act[0].rec;
		return 0;
	}
	build *b = s->b;
	entry *v = xmalloc(sizeof *v * s->nact);
	memcpy(v, s->act, sizeof *v * s->nact);
	qsort(v, s->nact, sizeof *v, cmp_seq);
	buf_t ids = {0};
	for (size_t i = 0; i < s->nact; i++)
		buf_append(&ids, &v[i].rec, sizeof v[i].rec);
	uint8_t h[16];
	sha256_16(ids.p, ids.len, h);
	buf_free(&ids);
	if (ht16_get(&b->merges, h, rec)) {
		free(v);
		return 0;
	}
	arena_reset(&b->a);
	mval *acc;
	int rc = load_record(&b->st, &b->a, v[0].rec, &acc);
	for (size_t i = 1; rc == 0 && i < s->nact; i++) {
		mval *next, *m = arena_alloc(&b->a, sizeof *m);
		if ((rc = load_record(&b->st, &b->a, v[i].rec, &next)) < 0 ||
		    (rc = merge_values(b, acc, next, s->mode == MERGE_RECURSE, true, m)) < 0)
			break;
		acc = m;
	}
	free(v);
	if (rc < 0 || intern(b, acc, rec) < 0)
		return -1;
	uint32_t dummy;
	ht16_get_or_put(&b->merges, h, *rec, &dummy);
	return 0;
}

static int count_next(void *ctx, sw_segment *seg, bool *ok)
{
	build *b = ctx;
	u128 start = 0, end = 0;
	uint32_t rec = 0;
	if (sweep_next(&b->sw, &start, &end, &rec, ok) < 0)
		return -1;
	if (!*ok)
		return 0;
	seg->start = start;
	seg->end = end;
	seg->start4 = seg->end4 = false;
	seg->record = rec;
	buf_t v = {0};
	put_varint(&v, b->replay_first ? start : start - b->replay_prev);
	put_varint(&v, end - start);
	put_varint(&v, rec);
	int rc = bw_write(&b->replay, v.p, v.len);
	buf_free(&v);
	b->replay_first = false;
	b->replay_prev = end + 1;
	b->pieces++;
	return rc;
}

static int load_shared(void *ctx, uint32_t id, mval **out)
{
	build *b = ctx;
	return shared_get(b->sh, id, &b->a, out);
}

static int supply_value(void *ctx, mval **out)
{
	*out = ((build *)ctx)->supply;
	return 0;
}

static int write_next(void *ctx, sw_segment *seg, bool *ok)
{
	build *b = ctx;
	u128 gap, len, rec;
	int rc = br_varint(&b->replay_r, IO_BUF, &gap);
	if (rc < 0)
		return -1;
	if (rc == 0) {
		*ok = false;
		return 0;
	}
	if (br_varint(&b->replay_r, IO_BUF, &len) <= 0 || br_varint(&b->replay_r, IO_BUF, &rec) <= 0) {
		err_set("internal error: the replay file ends inside a piece");
		return -1;
	}
	u128 start = b->replay_first ? gap : b->replay_prev + gap;
	seg->start = start;
	seg->end = start + len;
	seg->start4 = seg->end4 = false;
	seg->record = (uint32_t)rec;
	b->replay_first = false;
	b->replay_prev = seg->end + 1;
	*ok = true;
	sw_records *r = sw_get_records(b->w);
	if (sw_pending(r, seg->record)) {
		arena_reset(&b->a);
		if (load_record(&b->st, &b->a, seg->record, &b->supply) < 0 ||
		    sw_supply(r, seg->record, supply_value, b) < 0)
			return -1;
	}
	return 0;
}

/* the aliased IPv4 copies of an IPv6 tree: rows keep only what is outside */
static const struct {
	u128 start, end;
} ALIASES[] = {
	{(u128)0xffff << 32, ((u128)0xffff << 32) | 0xffffffffu},
	{(u128)0x2001 << 112, ((u128)0x2001 << 112) | (U128_MAX >> 32)},
	{(u128)0x2002 << 112, ((u128)0x2002 << 112) | (U128_MAX >> 16)},
};

/* Pushes the pieces of [s, e] outside the aliased networks; *cut when the
 * range reached into them only in part. */
static int push_outside_aliases(runset *rs, bool aliased, u128 s, u128 e, uint64_t seq, uint32_t id, bool *cut)
{
	bool any = true;
	*cut = false;
	for (size_t k = 0; any && k < (aliased ? 3u : 0u); k++) {
		u128 lo = ALIASES[k].start, hi = ALIASES[k].end;
		if (e < lo || hi < s)
			continue;
		if (!(s <= lo && hi <= e))
			*cut = true;
		if (s < lo && runs_push(rs, s, lo - 1, seq, id) < 0)
			return -1;
		if (hi >= e)
			any = false;
		else
			s = hi + 1;
	}
	if (any && runs_push(rs, s, e, seq, id) < 0)
		return -1;
	return 0;
}

/* "1.2.3.0/24" when [s, e] is one CIDR, else "start-end" */
static void network_text(u128 s, u128 e, bool is4, char out[160])
{
	u128 span = e - s;
	char a[64], z[64];
	fmt_ip(s, is4, a);
	if ((span & (span + 1)) == 0 && (s & span) == 0) {
		int host = 0;
		for (u128 x = span; x; x >>= 1)
			host++;
		snprintf(out, 160, "%s/%d", a, (is4 ? 32 : 128) - host);
		return;
	}
	snprintf(out, 160, "%s-%s", a, fmt_ip(e, is4, z));
}

static bool is_key(const mval *k, const char *name)
{
	size_t n = strlen(name);
	return k->n == n && memcmp(k->s, name, n) == 0;
}

/* The leaf paths of a record (nested maps followed), segments joined by
 * \x1f: the fields --fill-gaps fills. */
static void leaf_paths(columns *keys, const mval *m, buf_t *path, bool skip_network)
{
	size_t keep = path->len;
	for (uint32_t i = 0; i < m->n; i++) {
		const mval *k = &m->items[2 * i], *v = &m->items[2 * i + 1];
		if (skip_network && is_key(k, "network"))
			continue;
		if (keep)
			buf_push(path, 0x1f);
		buf_append(path, k->s, k->n);
		if (v->type == T_MAP && v->n)
			leaf_paths(keys, v, path, false);
		else
			columns_add(keys, (const char *)path->p, path->len);
		path->len = keep;
	}
}

/* The fill record: every leaf path "" (a path that is also a map elsewhere
 * becomes the map). paths are sorted; off is where this level starts. */
static mval fill_map(arena *a, char **paths, size_t n, size_t off)
{
	mval m = {.type = T_MAP};
	m.items = arena_alloc(a, sizeof *m.items * (2 * n + 2));
	for (size_t i = 0; i < n;) {
		const char *seg = paths[i] + off;
		size_t sl = strcspn(seg, "\x1f");
		size_t j = i;
		bool deeper = false;
		while (j < n && strncmp(paths[j] + off, seg, sl) == 0 &&
		       (paths[j][off + sl] == 0 || paths[j][off + sl] == 0x1f)) {
			deeper = deeper || paths[j][off + sl] == 0x1f;
			j++;
		}
		mval v = {.type = T_STRING, .s = (const uint8_t *)"", .n = 0};
		if (deeper) {
			size_t k = i;
			while (k < j && paths[k][off + sl] == 0)
				k++; /* leaves that are maps elsewhere */
			v = fill_map(a, paths + k, j - k, off + sl + 1);
		}
		m.items[2 * m.n] = (mval){.type = T_STRING, .s = (const uint8_t *)seg, .n = (uint32_t)sl};
		m.items[2 * m.n + 1] = v;
		m.n++;
		i = j;
	}
	mval_sort_map(&m);
	return m;
}

/* --no-fields, --network-field, and the keys --fill-gaps fills */
static mval *shape_record(const build_opts *o, arena *a, mval *rec, const row *r, columns *keys)
{
	if (rec->type != T_MAP)
		return rec;
	if (o->fill_gaps && !o->no_fields) {
		buf_t path = {0};
		leaf_paths(keys, rec, &path, o->network_field);
		buf_free(&path);
	}
	if (!o->no_fields && !o->network_field)
		return rec;
	uint32_t n = o->no_fields ? 0 : rec->n;
	mval *m = arena_alloc(a, sizeof *m);
	memset(m, 0, sizeof *m);
	m->type = T_MAP;
	m->items = arena_alloc(a, sizeof *m->items * (2 * (size_t)n + 2));
	for (uint32_t i = 0; i < n; i++) {
		if (o->network_field && is_key(&rec->items[2 * i], "network"))
			continue;
		m->items[2 * m->n] = rec->items[2 * i];
		m->items[2 * m->n + 1] = rec->items[2 * i + 1];
		m->n++;
	}
	if (o->network_field) {
		char t[160];
		network_text(r->start, r->end, r->is4, t);
		size_t tl = strlen(t);
		uint8_t *copy = arena_alloc(a, tl + 1);
		memcpy(copy, t, tl + 1);
		m->items[2 * m->n] = (mval){.type = T_STRING, .s = (const uint8_t *)"network", .n = 7};
		m->items[2 * m->n + 1] = (mval){.type = T_STRING, .s = copy, .n = (uint32_t)tl};
		m->n++;
	}
	mval_sort_map(m);
	return m;
}

static void warn_row(build *b, uint64_t *shown, long line, const char *why)
{
	if (b->o->quiet)
		return;
	if (++*shown <= MAX_WARNINGS)
		fprintf(stderr, "warn: %s:%ld: %s\n", b->o->input, line, why);
}

/* the record cache of template rows (build_mmdb) */
#define KC_SKIP UINT32_MAX          /* the key's rows are left out by a skip rule */
#define KC_TRIAL 1000000            /* lookups before checking that keys repeat */
#define KC_MAX ((size_t)1 << 23)    /* keys kept at most */

int build_mmdb(build_opts *o)
{
	int rc = -1;
	build b;
	memset(&b, 0, sizeof b);
	b.o = o;
	b.replay_fd = -1;
	b.st.fd = -1;
	arena_init(&b.a);
	ht16_init(&b.merges);
	arena row_a;
	arena_init(&row_a);
	columns gap_keys;
	columns_init(&gap_keys, false);
	const char *tmp = o->tmp_dir && *o->tmp_dir ? o->tmp_dir : temp_dir();
	runset rs = {.tmp = tmp, .fd = -1};
	rs.cap = o->memory / sizeof(entry);
	if (rs.cap < 1024)
		rs.cap = 1024;
	source src;
	memset(&src, 0, sizeof src);
	uint64_t rows = 0, inserted = 0, skipped = 0, shown = 0, clipped = 0, ruled_out = 0;
	/* a record template's key -> the record's id (or KC_SKIP): rows with a
	 * key seen before skip building, encoding and interning their record.
	 * It is dropped when keys rarely repeat (every row's key differs). */
	ht16 kc;
	ht16_init(&kc);
	bool kc_on = true;
	uint64_t kc_lookups = 0, kc_hits = 0;
	char a1[32], a2[32], a3[32];

	row_source *in = rows_open(o);
	if (!in)
		return -1;
	const char *const desc_langs_default[] = {"en"};
	const char *const desc_texts_default[] = {"Custom IP Intelligence Database"};
	const char *const languages_default[] = {"en"};
	sw_options opts = {
		.database_type = o->database_type,
		.description_langs = o->ndesc ? (const char *const *)o->desc_langs : desc_langs_default,
		.description_texts = o->ndesc ? (const char *const *)o->desc_texts : desc_texts_default,
		.ndescription = o->ndesc ? o->ndesc : 1,
		.languages = o->nlanguages || o->languages_set ? (const char *const *)o->languages : languages_default,
		.nlanguages = o->nlanguages || o->languages_set ? o->nlanguages : 1,
		.record_size = 32,
		.ip_version = o->ip_version,
		.alias_ipv6_to_ipv4 = o->alias_ipv4 && o->ip_version == 6,
		.remove_reserved_networks = !o->include_reserved,
		.alias_rules_in_source = true,
		.distinct_encodings = true,
		.ids_kept_by_source = true,
	};
	b.w = sw_new(&opts);
	if (!b.w)
		goto done;
	b.sh = shared_new(tmp);
	rows_set_shared(in, b.sh);
	sw_get_records(b.w)->data.shared_load = load_shared;
	sw_get_records(b.w)->data.shared_ctx = &b;
	if ((b.st.fd = temp_fd(tmp, "records")) < 0)
		goto done;
	bw_init(&b.st.w, b.st.fd);
	rs.buf = xmalloc(sizeof *rs.buf * rs.cap);

	/* 1. ingest */
	char why[512];
	for (;;) {
		row r;
		row_status st = rows_next(in, &r, why, sizeof why);
		if (st == ROW_EOF)
			break;
		if (st == ROW_FATAL)
			goto done;
		rows++;
		if (st == ROW_SKIP) {
			ruled_out++;
			continue;
		}
		if (st == ROW_OK) {
			if (r.end < r.start) {
				char x[64], y[64];
				snprintf(why, sizeof why, "the range starts after it ends (%s - %s)", fmt_ip(r.start, r.is4, x),
					 fmt_ip(r.end, r.is4, y));
				st = ROW_BAD;
			} else if (o->ip_version == 4 && (r.end >> 32) != 0) {
				snprintf(why, sizeof why, "an IPv6 network in an IPv4 database (build with --ip 6)");
				st = ROW_BAD;
			}
		}
		uint32_t id = 0;
		uint8_t kh[16];
		bool keyed = st == ROW_OK && r.key && kc_on;
		if (keyed) {
			siphash128(r.key, r.keylen, kh);
			kc_lookups++;
			uint32_t v;
			if (ht16_get(&kc, kh, &v)) {
				kc_hits++;
				if (v == KC_SKIP) {
					ruled_out++;
					continue;
				}
				id = v;
				goto have_id;
			}
			if (kc_lookups == KC_TRIAL && kc_hits * 10 < kc_lookups) {
				/* keys rarely repeat: the cache costs more than it saves */
				kc_on = keyed = false;
				ht16_free(&kc);
				ht16_init(&kc);
			}
		}
		if (st == ROW_OK && !r.rec) {
			st = rows_build(in, &r, why, sizeof why);
			if (st == ROW_FATAL)
				goto done;
			if (st == ROW_SKIP) {
				uint32_t old;
				if (keyed && kc.len < KC_MAX)
					ht16_get_or_put(&kc, kh, KC_SKIP, &old);
				ruled_out++;
				continue;
			}
		}
		if (st == ROW_OK && (o->no_fields || o->network_field || o->fill_gaps)) {
			arena_reset(&row_a);
			r.rec = shape_record(o, &row_a, r.rec, &r, &gap_keys);
		}
		if (st == ROW_OK) {
			b.key.len = 0;
			if (mval_canonical(&b.key, r.rec) < 0) {
				snprintf(why, sizeof why, "%s", err_msg());
				st = ROW_BAD;
			} else if (intern_key(&b, &id) < 0) {
				goto done;
			} else if (keyed && kc.len < KC_MAX) {
				uint32_t old;
				ht16_get_or_put(&kc, kh, id, &old);
			}
		}
		if (st == ROW_BAD) {
			if (o->strict) {
				err_set("%s:%ld: %s", o->input, r.line, why);
				goto done;
			}
			skipped++;
			warn_row(&b, &shown, r.line, why);
			continue;
		}
	have_id:;
		/* the pieces of the row outside the aliased networks; with
		 * ipv6_first every IPv4 row counts as later than every IPv6 one, as
		 * if the file were read twice, IPv6 rows first */
		bool cut;
		uint64_t seq = o->ipv6_first && r.is4 ? rows | (uint64_t)1 << 62 : rows;
		if (push_outside_aliases(&rs, opts.alias_ipv6_to_ipv4, r.start, r.end, seq, id, &cut) < 0)
			goto done;
		if (cut)
			clipped++;
		inserted++;
		if (!o->quiet && isatty(2) && inserted % 1000000 == 0)
			fprintf(stderr, "\rread %s rows", fmt_int(inserted, a1));
	}
	if (!o->quiet && isatty(2) && inserted >= 1000000)
		fprintf(stderr, "\r");
	if (shown > MAX_WARNINGS && !o->quiet)
		fprintf(stderr, "warn: ... and %s more rows skipped\n", fmt_int(shown - MAX_WARNINGS, a1));

	/* --fill-gaps: a record of empty strings under every row, losing to all of them */
	if (o->fill_gaps) {
		columns_sort(&gap_keys);
		arena fa;
		arena_init(&fa);
		mval fill = fill_map(&fa, gap_keys.names, gap_keys.n, 0);
		b.key.len = 0;
		uint32_t id;
		int frc = mval_canonical(&b.key, &fill);
		arena_free(&fa);
		if (frc < 0 || intern_key(&b, &id) < 0)
			goto done;
		bool cut;
		u128 top = o->ip_version == 4 ? (u128)0xffffffffu : U128_MAX;
		if (push_outside_aliases(&rs, opts.alias_ipv6_to_ipv4, 0, top, o->merge == MERGE_KEEP ? UINT64_MAX : 0, id,
					 &cut) < 0)
			goto done;
	}

	/* 2. the sorted ranges */
	if (runs_finish(&rs, &src) < 0)
		goto done;

	/* 3. count, keeping the pieces */
	b.sw.src = &src;
	b.sw.mode = o->merge;
	b.sw.b = &b;
	bool ok;
	if (source_next(&src, &b.sw.ahead, &ok) < 0)
		goto done;
	b.sw.has_ahead = ok;
	if ((b.replay_fd = temp_fd(tmp, "pieces")) < 0)
		goto done;
	bw_init(&b.replay, b.replay_fd);
	b.replay_first = true;
	if (sw_count(b.w, count_next, &b) < 0 || bw_flush(&b.replay) < 0 || bw_flush(&b.st.w) < 0)
		goto done;
	sw_stats st = sw_get_stats(b.w);

	int size = o->record_size;
	if (size == 0) {
		/* at most every record and every shared value once */
		uint64_t data = (b.st.offs ? b.st.offs[b.st.n] : 0) + shared_bytes(b.sh);
		size = 32;
		for (int cand = 24; cand <= 32; cand += 4) {
			if (st.nodes + 16 + data < ((uint64_t)1 << cand)) {
				size = cand;
				break;
			}
		}
	}
	if (sw_set_record_size(b.w, size) < 0)
		goto done;

	/* 4. write */
	br_init(&b.replay_r, b.replay_fd, 0, bw_pos(&b.replay), IO_BUF);
	b.replay_first = true;
	if (sw_write_file(b.w, o->output, write_next, &b) < 0)
		goto done;
	st = sw_get_stats(b.w);
	if (!o->quiet) {
		fprintf(stderr, "input: %s\n", rows_describe(in));
		fprintf(stderr, "rows: %s read, %s inserted, %s skipped", fmt_int(rows, a1), fmt_int(inserted, a2),
			fmt_int(skipped, a3));
		if (ruled_out)
			fprintf(stderr, ", %s left out by the record's skip rules", fmt_int(ruled_out, a1));
		fprintf(stderr, "\n");
		if (clipped)
			fprintf(stderr,
				"note: %s rows reached into the aliased IPv4 networks (::ffff:0:0/96, 2001::/32, "
				"2002::/16); those parts point at the IPv4 data instead\n",
				fmt_int(clipped, a1));
		if (rows_lookup_missing(in))
			fprintf(stderr, "note: %s rows have an id the lookup lacks; every field of their records is empty\n",
				fmt_int(rows_lookup_missing(in), a1));
		buf_t notes = {0};
		rows_report(in, &notes);
		if (notes.len)
			fprintf(stderr, "%s", buf_cstr(&notes));
		buf_free(&notes);
		if (rows_utf8_fixed(in))
			fprintf(stderr, "note: %s values held invalid UTF-8; each bad byte became U+FFFD\n",
				fmt_int(rows_utf8_fixed(in), a1));
		fprintf(stderr, "tree: %s unique records, %s search-tree nodes, record size %d, %s bytes\n",
			fmt_int(st.records, a1), fmt_int(st.nodes, a2), st.record_size, fmt_int((uint64_t)st.file_bytes, a3));
	}
	if (!o->silent)
		fprintf(stderr, "\u2705 Successfully wrote %llu entries to %s\n", (unsigned long long)inserted,
			o->label ? o->label : o->output);
	rc = 0;
done:
	rows_close(in);
	sw_free(b.w);
	shared_free(b.sh);
	if (b.st.fd >= 0)
		close(b.st.fd);
	free(b.st.w.buf);
	free(b.st.offs);
	buf_free(&b.st.rd);
	if (b.replay_fd >= 0)
		close(b.replay_fd);
	free(b.replay.buf);
	free(b.replay_r.buf);
	if (rs.fd >= 0)
		close(rs.fd);
	free(rs.w.buf);
	free(rs.runs);
	if (src.mem != rs.buf)
		free(src.mem);
	free(rs.buf);
	for (size_t i = 0; i < src.nrr; i++)
		free(src.rr[i].r.buf);
	free(src.rr);
	free(src.heap);
	free(b.sw.act);
	buf_free(&b.key);
	buf_free(&b.mkey);
	ht16_free(&b.merges);
	arena_free(&b.a);
	arena_free(&row_a);
	columns_free(&gap_keys);
	ht16_free(&kc);
	return rc;
}
