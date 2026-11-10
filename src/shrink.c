#include "shrink.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hashtab.h"
#include "mmdb_enc.h"
#include "mmdb_read.h"

static const uint8_t MARKER[] = "\xAB\xCD\xEFMaxMind.com";
#define MARKER_LEN 14

/* A canonical pointer: 0 is the empty branch, 1 + id an output node, and
 * CV_LEAF | index the index-th distinct record the tree points at. */
#define CV_NULL 0u
#define CV_LEAF 0x80000000u
#define MAX_IDS 0x7ffffffeu

typedef struct {
	const mmdb_reader *r;
	uint8_t *refs;       /* 2 bits per input node: parents seen, 2 meaning more */
	ht64 memo;           /* shared input node -> its canonical pointer */
	ht64 table;          /* left << 32 | right -> output node id */
	uint32_t *nodes;     /* output nodes, two canonical pointers each */
	uint64_t nnodes, cap;
	ht64 leaf_ix;        /* data offset -> leaf index */
	uint64_t *leaves;    /* leaf index -> data offset */
	uint64_t nleaves, leaf_cap;
	uint64_t ipv4_node;  /* the IPv4 subtree's input node, never shared */
} canon_state;

static unsigned refs_get(const canon_state *s, uint64_t n) { return s->refs[n >> 2] >> ((n & 3) * 2) & 3; }

static void refs_inc(canon_state *s, uint64_t n)
{
	unsigned v = refs_get(s, n);
	if (v < 2)
		s->refs[n >> 2] = (uint8_t)(s->refs[n >> 2] + (1u << ((n & 3) * 2)));
}

static int new_node(canon_state *s, uint32_t l, uint32_t r, uint32_t *id)
{
	if (s->nnodes >= MAX_IDS) {
		err_set("the shrunk tree would have more than %u nodes", MAX_IDS);
		return -1;
	}
	if (s->nnodes == s->cap) {
		s->cap = s->cap < 1024 ? 1024 : s->cap + s->cap / 2;
		s->nodes = xrealloc(s->nodes, s->cap * 2 * sizeof *s->nodes);
	}
	s->nodes[2 * s->nnodes] = l;
	s->nodes[2 * s->nnodes + 1] = r;
	*id = (uint32_t)s->nnodes++;
	return 0;
}

static int canon(canon_state *s, uint64_t ptr, int depth, uint32_t *cv)
{
	const mmdb_reader *r = s->r;
	uint64_t n = r->node_count;
	if (ptr == n) {
		*cv = CV_NULL;
		return 0;
	}
	if (ptr > n) {
		if (ptr < n + 16 || ptr - n - 16 >= r->data.len) {
			err_set("the MaxMind DB file's search tree is corrupt");
			return -1;
		}
		uint64_t off = ptr - n - 16;
		uint32_t ix;
		if (!ht64_get_or_put(&s->leaf_ix, off, (uint32_t)s->nleaves, &ix)) {
			if (s->nleaves >= MAX_IDS) {
				err_set("too many distinct records to shrink");
				return -1;
			}
			if (s->nleaves == s->leaf_cap) {
				s->leaf_cap = s->leaf_cap < 1024 ? 1024 : s->leaf_cap * 2;
				s->leaves = xrealloc(s->leaves, s->leaf_cap * sizeof *s->leaves);
			}
			s->leaves[s->nleaves] = off;
			ix = (uint32_t)s->nleaves++;
		}
		*cv = CV_LEAF | ix;
		return 0;
	}
	if (depth >= 128) {
		err_set("invalid search tree: internal node at bit depth %d", depth);
		return -1;
	}
	bool shared = refs_get(s, ptr) >= 2;
	if (shared && ht64_get(&s->memo, ptr, cv))
		return 0;
	uint64_t left, right;
	if (mmdb_read_node(r, ptr, &left, &right) < 0)
		return -1;
	uint32_t l, rt;
	if (canon(s, left, depth + 1, &l) < 0 || canon(s, right, depth + 1, &rt) < 0)
		return -1;
	if (ptr == 0) {
		s->nodes[0] = l;
		s->nodes[1] = rt;
		*cv = 1;
	} else if (l == CV_NULL && rt == CV_NULL) {
		*cv = CV_NULL;
	} else {
		uint32_t id;
		if (ptr == s->ipv4_node) {
			if (new_node(s, l, rt, &id) < 0)
				return -1;
		} else if (!ht64_get_or_put(&s->table, (uint64_t)l << 32 | rt, (uint32_t)s->nnodes, &id)) {
			if (new_node(s, l, rt, &id) < 0)
				return -1;
		}
		*cv = id + 1;
	}
	if (shared) {
		uint32_t had;
		ht64_get_or_put(&s->memo, ptr, *cv, &had);
	}
	return 0;
}

/* ---- the data section ----
 *
 * Pass 1 finds what the tree reaches (records, and what their pointers
 * reach) with a bit per byte and one span per value. When that is all of
 * the section and no pointer is wider than its target needs, the section is
 * copied as it is. Otherwise pass 2 notes every pointer, and the kept
 * values are laid out again with each pointer at its narrowest. Offsets fit
 * 32 bits: pointers reach no further. */

typedef struct {
	uint32_t loc, target;
	uint8_t len, w; /* bytes now, bytes in the output */
} ptr_ref;

typedef struct {
	uint32_t start, end;
} span;

typedef struct {
	const uint8_t *b;
	uint64_t len;
	bool collect;    /* pass 2: note every pointer */
	uint64_t wide;   /* pointers wider than their target needs */
	ptr_ref *ptrs;
	size_t nptrs, ptr_cap;
	span *spans;
	size_t nspans, span_cap;
	uint32_t *todo;
	size_t ntodo, todo_cap;
	uint64_t *seen;  /* a bit per byte: a value queued there */
	bool verbatim;   /* the section is copied as it is */
	/* the output layout */
	span *regions;
	uint32_t *gap_before; /* bytes dropped before each region */
	size_t nregions;
	int64_t *delta;       /* sum of (w - len) of the pointers before each */
} compact;

static int corrupt(uint64_t off)
{
	err_set("invalid data section: unexpected end of data at offset %llu", (unsigned long long)off);
	return -1;
}

static void want(compact *c, uint32_t off)
{
	uint64_t bit = (uint64_t)1 << (off & 63);
	if (c->seen[off >> 6] & bit)
		return;
	c->seen[off >> 6] |= bit;
	if (c->ntodo == c->todo_cap) {
		c->todo_cap = c->todo_cap ? c->todo_cap * 2 : 1024;
		c->todo = xrealloc(c->todo, c->todo_cap * sizeof *c->todo);
	}
	c->todo[c->ntodo++] = off;
}

static int min_width(uint64_t target)
{
	return target <= 2047 ? 2 : target <= 526335 ? 3 : target <= 134744063 ? 4 : 5;
}

/* Walks the value at off: *end is just past it; the targets of the pointers
 * inside it are queued (and in pass 2 the pointers noted). */
static int walk(compact *c, uint64_t off, int depth, uint64_t *end)
{
	if (depth > 512) {
		err_set("invalid data section: values nested too deeply");
		return -1;
	}
	if (off >= c->len)
		return corrupt(off);
	uint64_t p = off;
	uint8_t ctrl = c->b[p++];
	int type = ctrl >> 5;
	if (type == T_POINTER) {
		int cls = (ctrl >> 3) & 3;
		if (p + (uint64_t)cls + 1 > c->len)
			return corrupt(off);
		uint64_t v = 0;
		for (int i = 0; i <= cls; i++)
			v = v << 8 | c->b[p + (uint64_t)i];
		uint64_t vvv = ctrl & 7;
		uint64_t target = cls == 0 ? (vvv << 8 | v)
			: cls == 1 ? (vvv << 16 | v) + 2048
			: cls == 2 ? (vvv << 24 | v) + 526336
			: v;
		if (target >= c->len) {
			err_set("invalid data section: pointer at offset %llu points past the data", (unsigned long long)off);
			return -1;
		}
		if (c->collect) {
			if (c->nptrs == c->ptr_cap) {
				c->ptr_cap = c->ptr_cap ? c->ptr_cap + c->ptr_cap / 2 : 1024;
				c->ptrs = xrealloc(c->ptrs, c->ptr_cap * sizeof *c->ptrs);
			}
			c->ptrs[c->nptrs++] = (ptr_ref){.loc = (uint32_t)off, .target = (uint32_t)target, .len = (uint8_t)(cls + 2)};
		} else if (min_width(target) < cls + 2) {
			c->wide++;
		}
		want(c, (uint32_t)target);
		*end = off + (uint64_t)cls + 2;
		return 0;
	}
	if (type == T_EXTENDED) {
		if (p >= c->len)
			return corrupt(off);
		type = 7 + c->b[p++];
		if (type < 8 || type > 15) {
			err_set("invalid data section: unknown type at offset %llu", (unsigned long long)off);
			return -1;
		}
	}
	uint64_t size = ctrl & 0x1f;
	if (size >= 29) {
		int nb = (int)size - 28;
		if (p + (uint64_t)nb > c->len)
			return corrupt(off);
		uint64_t v = 0;
		for (int i = 0; i < nb; i++)
			v = v << 8 | c->b[p++];
		size = size == 29 ? 29 + v : size == 30 ? 285 + v : 65821 + v;
	}
	switch (type) {
	case T_MAP:
		for (uint64_t i = 0; i < 2 * size; i++)
			if (walk(c, p, depth + 1, &p) < 0)
				return -1;
		break;
	case T_ARRAY:
		for (uint64_t i = 0; i < size; i++)
			if (walk(c, p, depth + 1, &p) < 0)
				return -1;
		break;
	case T_BOOL:
		break;
	case T_CONTAINER:
	case T_END_MARKER:
		err_set("invalid data section: unsupported type %d at offset %llu", type, (unsigned long long)off);
		return -1;
	default:
		if (size > c->len - p)
			return corrupt(off);
		p += size;
	}
	*end = p;
	return 0;
}

static int cmp_span(const void *a, const void *b)
{
	const span *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

static int cmp_ptr(const void *a, const void *b)
{
	const ptr_ref *x = a, *y = b;
	return x->loc < y->loc ? -1 : x->loc > y->loc;
}

/* the region holding off: the last one that starts at or before it */
static size_t region_of(const compact *c, uint64_t off)
{
	size_t lo = 0, hi = c->nregions;
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if (c->regions[mid].start <= off)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* the pointers that start before off */
static size_t ptrs_before(const compact *c, uint64_t off)
{
	size_t lo = 0, hi = c->nptrs;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c->ptrs[mid].loc < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static uint64_t new_offset(const compact *c, uint64_t off)
{
	if (c->verbatim)
		return off;
	size_t k = region_of(c, off);
	return (uint64_t)((int64_t)(off - c->gap_before[k]) + c->delta[ptrs_before(c, off)]);
}

static void compute_deltas(compact *c)
{
	c->delta[0] = 0;
	for (size_t i = 0; i < c->nptrs; i++)
		c->delta[i + 1] = c->delta[i] + (int64_t)c->ptrs[i].w - (int64_t)c->ptrs[i].len;
}

/* one pass over everything the roots reach */
static int reach(compact *c, const uint64_t *roots, size_t nroots)
{
	memset(c->seen, 0, (size_t)(c->len / 64 + 1) * sizeof *c->seen);
	c->ntodo = 0;
	for (size_t i = 0; i < nroots; i++)
		want(c, (uint32_t)roots[i]);
	while (c->ntodo) {
		uint64_t off = c->todo[--c->ntodo], end;
		if (walk(c, off, 0, &end) < 0)
			return -1;
		if (c->collect)
			continue;
		if (c->nspans == c->span_cap) {
			c->span_cap = c->span_cap ? c->span_cap + c->span_cap / 2 : 1024;
			c->spans = xrealloc(c->spans, c->span_cap * sizeof *c->spans);
		}
		c->spans[c->nspans++] = (span){(uint32_t)off, (uint32_t)end};
	}
	return 0;
}

/* The layout: what is kept, and the width of every pointer. */
static int compact_plan(compact *c, const uint64_t *roots, size_t nroots, uint64_t *out_len)
{
	*out_len = c->len;
	if (c->len > UINT32_MAX) {
		/* nothing past 4 GB is reachable, but the walk counts in 32 bits */
		c->verbatim = true;
		return 0;
	}
	c->seen = xcalloc((size_t)(c->len / 64 + 1), sizeof *c->seen);
	int rc = -1;
	if (reach(c, roots, nroots) < 0)
		goto done;
	if (c->nspans > 1)
		qsort(c->spans, c->nspans, sizeof *c->spans, cmp_span);
	/* overlapping or touching spans form one region */
	c->regions = xmalloc((c->nspans + 1) * sizeof *c->regions);
	c->gap_before = xmalloc((c->nspans + 1) * sizeof *c->gap_before);
	uint64_t kept = 0;
	for (size_t i = 0; i < c->nspans; i++) {
		span s = c->spans[i];
		if (c->nregions && s.start <= c->regions[c->nregions - 1].end) {
			span *last = &c->regions[c->nregions - 1];
			if (s.end > last->end) {
				kept += s.end - last->end;
				last->end = s.end;
			}
			continue;
		}
		c->gap_before[c->nregions] = (uint32_t)(s.start - kept);
		c->regions[c->nregions++] = s;
		kept += s.end - s.start;
	}
	free(c->spans);
	c->spans = NULL;
	if (c->nregions == 1 && c->regions[0].start == 0 && c->regions[0].end == c->len && c->wide == 0) {
		c->verbatim = true;
		rc = 0;
		goto done;
	}
	/* pass 2: the pointers */
	c->collect = true;
	if (reach(c, roots, nroots) < 0)
		goto done;
	/* a pointer inside a value reached twice was noted twice */
	if (c->nptrs > 1)
		qsort(c->ptrs, c->nptrs, sizeof *c->ptrs, cmp_ptr);
	size_t k = 0;
	for (size_t i = 0; i < c->nptrs; i++)
		if (k == 0 || c->ptrs[k - 1].loc != c->ptrs[i].loc)
			c->ptrs[k++] = c->ptrs[i];
	c->nptrs = k;
	for (size_t i = 0; i < c->nptrs; i++)
		c->ptrs[i].w = c->ptrs[i].len;
	c->delta = xmalloc((c->nptrs + 1) * sizeof *c->delta);
	/* Targets only move down as pointers narrow, and a lower target never
	 * needs a wider pointer, so this settles in a few rounds. */
	for (bool changed = true; changed;) {
		changed = false;
		compute_deltas(c);
		for (size_t i = 0; i < c->nptrs; i++) {
			int w = min_width(new_offset(c, c->ptrs[i].target));
			if (w < c->ptrs[i].w) {
				c->ptrs[i].w = (uint8_t)w;
				changed = true;
			}
		}
	}
	compute_deltas(c);
	for (size_t i = 0; i < c->nptrs; i++)
		if (min_width(new_offset(c, c->ptrs[i].target)) > c->ptrs[i].w) {
			err_set("invalid data section: pointer at offset %llu is too narrow", (unsigned long long)c->ptrs[i].loc);
			goto done;
		}
	*out_len = kept + (uint64_t)c->delta[c->nptrs];
	rc = 0;
done:
	free(c->seen);
	c->seen = NULL;
	free(c->todo);
	c->todo = NULL;
	return rc;
}

static void put_pointer(uint8_t *b, int w, uint64_t t)
{
	switch (w) {
	case 2:
		b[0] = (uint8_t)(0x20 | (t >> 8));
		b[1] = (uint8_t)t;
		break;
	case 3:
		t -= 2048;
		b[0] = (uint8_t)(0x20 | 1 << 3 | (t >> 16));
		b[1] = (uint8_t)(t >> 8);
		b[2] = (uint8_t)t;
		break;
	case 4:
		t -= 526336;
		b[0] = (uint8_t)(0x20 | 2 << 3 | (t >> 24));
		b[1] = (uint8_t)(t >> 16);
		b[2] = (uint8_t)(t >> 8);
		b[3] = (uint8_t)t;
		break;
	default:
		b[0] = 0x20 | 3 << 3;
		b[1] = (uint8_t)(t >> 24);
		b[2] = (uint8_t)(t >> 16);
		b[3] = (uint8_t)(t >> 8);
		b[4] = (uint8_t)t;
	}
}

static int compact_emit(const compact *c, FILE *f)
{
	if (c->verbatim) {
		fwrite(c->b, 1, (size_t)c->len, f);
		return 0;
	}
	size_t pi = 0;
	for (size_t k = 0; k < c->nregions; k++) {
		uint64_t pos = c->regions[k].start, end = c->regions[k].end;
		for (; pi < c->nptrs && c->ptrs[pi].loc < end; pi++) {
			const ptr_ref *p = &c->ptrs[pi];
			if (p->loc > pos)
				fwrite(c->b + pos, 1, (size_t)(p->loc - pos), f);
			uint8_t enc[5];
			put_pointer(enc, p->w, new_offset(c, p->target));
			fwrite(enc, 1, p->w, f);
			pos = (uint64_t)p->loc + p->len;
		}
		if (end > pos)
			fwrite(c->b + pos, 1, (size_t)(end - pos), f);
	}
	return 0;
}

static void compact_free(compact *c)
{
	free(c->ptrs);
	free(c->spans);
	free(c->todo);
	free(c->seen);
	free(c->regions);
	free(c->gap_before);
	free(c->delta);
}

/* ---- the output ---- */

static void sort_maps(mval *v)
{
	if (v->type == T_MAP) {
		mval_sort_map(v);
		for (uint32_t i = 0; i < v->n; i++)
			sort_maps(&v->items[2 * i + 1]);
	} else if (v->type == T_ARRAY) {
		for (uint32_t i = 0; i < v->n; i++)
			sort_maps(&v->items[i]);
	}
}

static bool key_is(const mval *k, const char *name)
{
	size_t n = strlen(name);
	return k->type == T_STRING && k->n == n && memcmp(k->s, name, n) == 0;
}

static int encode_metadata(const mmdb_reader *r, uint64_t nodes, int record_size, buf_t *out)
{
	arena a;
	arena_init(&a);
	mval *m = dnode_to_mval(&a, r->meta_root);
	bool has_nodes = false, has_size = false;
	for (uint32_t i = 0; i < m->n; i++) {
		mval *k = &m->items[2 * i], *v = &m->items[2 * i + 1];
		if (key_is(k, "node_count")) {
			*v = (mval){.type = T_UINT32, .u = nodes};
			has_nodes = true;
		} else if (key_is(k, "record_size")) {
			*v = (mval){.type = T_UINT16, .u = (uint64_t)record_size};
			has_size = true;
		}
	}
	int rc = -1;
	if (!has_nodes || !has_size)
		err_set("invalid metadata: node_count or record_size is missing");
	else {
		sort_maps(m);
		rc = mval_canonical(out, m);
	}
	arena_free(&a);
	return rc;
}

static void put_record(uint8_t *b, int size, uint64_t l, uint64_t r)
{
	switch (size) {
	case 24:
		b[0] = (uint8_t)(l >> 16), b[1] = (uint8_t)(l >> 8), b[2] = (uint8_t)l;
		b[3] = (uint8_t)(r >> 16), b[4] = (uint8_t)(r >> 8), b[5] = (uint8_t)r;
		break;
	case 28:
		b[0] = (uint8_t)(l >> 16), b[1] = (uint8_t)(l >> 8), b[2] = (uint8_t)l;
		b[3] = (uint8_t)((l >> 24 & 0xf) << 4 | (r >> 24 & 0xf));
		b[4] = (uint8_t)(r >> 16), b[5] = (uint8_t)(r >> 8), b[6] = (uint8_t)r;
		break;
	default:
		for (int i = 0; i < 4; i++) {
			b[i] = (uint8_t)(l >> (24 - 8 * i));
			b[4 + i] = (uint8_t)(r >> (24 - 8 * i));
		}
	}
}

static void phase(const shrink_opts *o, double *t, const char *what)
{
	double now = mono_seconds();
	if (o->verbose)
		fprintf(stderr, "shrink: %s (%.3fs)\n", what, now - *t);
	*t = now;
}

int mmdb_shrink(const char *in, const char *out, const shrink_opts *o, shrink_result *res)
{
	memset(res, 0, sizeof *res);
	double t0 = mono_seconds(), t = t0;
	if (o->record_size && o->record_size != 24 && o->record_size != 28 && o->record_size != 32) {
		err_set("unsupported record size %d, must be 24, 28 or 32", o->record_size);
		return -1;
	}
	mmdb_reader r;
	if (mmdb_open(&r, in) < 0) {
		mmdb_close(&r);
		return -1;
	}
	int rc = -1;
	canon_state s;
	memset(&s, 0, sizeof s);
	compact c;
	memset(&c, 0, sizeof c);
	uint64_t *newoff = NULL;
	FILE *f = NULL;
	buf_t meta = {0};
	ht64_init(&s.memo);
	ht64_init(&s.table);
	ht64_init(&s.leaf_ix);

	if (r.record_size != 24 && r.record_size != 28 && r.record_size != 32) {
		err_set("unsupported record size %llu", (unsigned long long)r.record_size);
		goto done;
	}
	if (r.node_count == 0) {
		err_set("the database has no search tree");
		goto done;
	}
	res->in_bytes = r.size;
	res->in_nodes = r.node_count;
	res->in_record_size = (int)r.record_size;
	res->in_tree = r.tree_size;
	res->in_data = r.data.len;
	res->meta_bytes = r.meta.len + MARKER_LEN;

	/* 1. which nodes have more than one parent */
	s.r = &r;
	s.ipv4_node = UINT64_MAX;
	if (r.ip_version == 6 && r.ipv4_start_bit_depth == 96 && r.ipv4_start != 0 && r.ipv4_start < r.node_count)
		s.ipv4_node = r.ipv4_start;
	s.refs = xcalloc((size_t)(r.node_count / 4 + 1), 1);
	refs_inc(&s, 0);
	for (uint64_t i = 0; i < r.node_count; i++) {
		uint64_t left, right;
		if (mmdb_read_node(&r, i, &left, &right) < 0)
			goto done;
		if (left < r.node_count)
			refs_inc(&s, left);
		if (right < r.node_count)
			refs_inc(&s, right);
	}
	phase(o, &t, "counted parents");

	/* 2. intern every node, bottom up. There are at most as many distinct
	 * nodes as input nodes: the intern table is sized for that once, and
	 * the node array too (its pages cost memory only once written). */
	if (r.node_count < MAX_IDS) {
		ht64_reserve(&s.table, (size_t)r.node_count);
		s.cap = r.node_count + 1;
		s.nodes = xmalloc((size_t)s.cap * 2 * sizeof *s.nodes);
	}
	uint32_t root, unused;
	if (new_node(&s, CV_NULL, CV_NULL, &unused) < 0 || canon(&s, 0, 0, &root) < 0)
		goto done;
	free(s.refs);
	s.refs = NULL;
	ht64_free(&s.memo);
	ht64_free(&s.table);
	ht64_init(&s.memo);
	ht64_init(&s.table);
	char n1[32], n2[32], n3[32];
	if (o->verbose)
		fprintf(stderr, "shrink: %s -> %s nodes, %s records\n", fmt_int(r.node_count, n1), fmt_int(s.nnodes, n2),
			fmt_int(s.nleaves, n3));
	phase(o, &t, "interned the tree");

	/* 3. the data section */
	uint64_t data_len = r.data.len;
	newoff = xmalloc((s.nleaves + 1) * sizeof *newoff);
	if (o->no_compact) {
		for (uint64_t i = 0; i < s.nleaves; i++)
			newoff[i] = s.leaves[i];
	} else {
		c.b = r.data.buf;
		c.len = r.data.len;
		if (compact_plan(&c, s.leaves, s.nleaves, &data_len) < 0)
			goto done;
		for (uint64_t i = 0; i < s.nleaves; i++)
			newoff[i] = new_offset(&c, s.leaves[i]);
		phase(o, &t, c.verbatim ? "the data section is kept as it is" : "planned the data section");
	}

	/* 4. the record size */
	uint64_t nodes = s.nnodes, max_ptr = nodes; /* the empty branch */
	for (uint64_t i = 0; i < s.nleaves; i++)
		if (nodes + 16 + newoff[i] > max_ptr)
			max_ptr = nodes + 16 + newoff[i];
	res->min_record_size = max_ptr < (1ull << 24) ? 24 : max_ptr < (1ull << 28) ? 28 : 32;
	if (max_ptr >= (1ull << 32)) {
		err_set("the shrunk tree does not fit any record size");
		goto done;
	}
	int size = o->record_size ? o->record_size : (int)r.record_size;
	if (size < res->min_record_size) {
		err_set("the shrunk tree needs record size %d or more (asked for %d)", res->min_record_size, size);
		goto done;
	}
	if (encode_metadata(&r, nodes, size, &meta) < 0)
		goto done;
	res->out_nodes = nodes;
	res->out_record_size = size;
	res->out_tree = nodes * (uint64_t)(size / 4);
	res->out_data = data_len;
	res->meta_bytes = meta.len + MARKER_LEN;
	res->out_bytes = res->out_tree + 16 + data_len + res->meta_bytes;

	/* 5. the file */
	if (out) {
		f = fopen(out, "wb");
		if (!f) {
			err_set("create %s: %s", out, go_strerror(errno));
			goto done;
		}
		static char fbuf[1 << 20];
		setvbuf(f, fbuf, _IOFBF, sizeof fbuf);
		uint8_t rec[8];
		int nb = size / 4;
		for (uint64_t i = 0; i < nodes; i++) {
			uint64_t v[2];
			for (int k = 0; k < 2; k++) {
				uint32_t cv = s.nodes[2 * i + (uint64_t)k];
				v[k] = cv == CV_NULL ? nodes : cv & CV_LEAF ? nodes + 16 + newoff[cv & ~CV_LEAF] : cv - 1u;
			}
			put_record(rec, size, v[0], v[1]);
			fwrite(rec, 1, (size_t)nb, f);
		}
		static const uint8_t zeros[16];
		fwrite(zeros, 1, 16, f);
		if (o->no_compact)
			fwrite(r.data.buf, 1, (size_t)r.data.len, f);
		else
			compact_emit(&c, f);
		fwrite(MARKER, 1, MARKER_LEN, f);
		fwrite(meta.p, 1, meta.len, f);
		if (fflush(f) != 0 || ferror(f)) {
			err_set("write %s: %s", out, go_strerror(errno));
			goto done;
		}
		int e = fclose(f);
		f = NULL;
		if (e != 0) {
			err_set("write %s: %s", out, go_strerror(errno));
			goto done;
		}
		phase(o, &t, "wrote the file");
	}
	res->seconds = mono_seconds() - t0;
	rc = 0;
done:
	if (f)
		fclose(f);
	free(s.refs);
	free(s.nodes);
	free(s.leaves);
	ht64_free(&s.memo);
	ht64_free(&s.table);
	ht64_free(&s.leaf_ix);
	compact_free(&c);
	free(newoff);
	buf_free(&meta);
	mmdb_close(&r);
	return rc;
}
