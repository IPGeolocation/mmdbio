#include "streamwriter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"

#include "sha256.h"

/* ---- fixed networks (fixed.go) ---- */

static const char *const reserved_ipv4[] = {
	"0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
	"169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/29", "192.0.2.0/24",
	"192.88.99.0/24", "192.168.0.0/16", "198.18.0.0/15", "198.51.100.0/24",
	"203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4",
};

static const char *const reserved_ipv6[] = {
	"100::/64",
	"2001:1::/32", "2001:2::/31", "2001:4::/30", "2001:8::/29",
	"2001:10::/28", "2001:20::/27", "2001:40::/26", "2001:80::/25",
	"2001:100::/24",
	"2001:db8::/32", "fc00::/7", "fe80::/10", "ff00::/8",
};

static const char *const ipv4_aliases[] = {"::ffff:0:0/96", "2001::/32", "2002::/16"};

enum { FIXED_RESERVED = 1, FIXED_ALIAS = 2 };

typedef struct {
	u128 start, end;
	int depth;
	int kind;
} fixed_net;

const char *fmt_network(u128 a, int depth, char out[80])
{
	char ip[64];
	if ((a >> 32) == 0 && depth >= 96)
		snprintf(out, 80, "%s/%d", fmt_addr(a, ip), depth - 96);
	else
		snprintf(out, 80, "%s/%d", fmt_addr(a, ip), depth);
	return out;
}

static int parse_prefix(const char *s, fixed_net *f, int kind)
{
	char ip[64];
	const char *slash = strchr(s, '/');
	size_t n = (size_t)(slash - s);
	memcpy(ip, s, n);
	ip[n] = 0;
	int bits = atoi(slash + 1);
	if (strchr(ip, ':')) {
		uint8_t b[16];
		if (pton6(ip, b) != 1)
			return -1;
		f->start = u128_from_be(b);
		f->depth = bits;
	} else {
		uint8_t b[4];
		if (pton4(ip, b) != 1)
			return -1;
		f->start = (u128)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
		f->depth = bits + 96;
	}
	f->end = addr_last(f->start, f->depth);
	f->kind = kind;
	return 0;
}

static int cmp_fixed(const void *a, const void *b)
{
	const fixed_net *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

static int fixed_networks(bool alias, bool reserved, fixed_net **out, size_t *nout)
{
	fixed_net *v = xmalloc(sizeof *v * 64);
	size_t n = 0;
	if (alias)
		for (size_t i = 0; i < sizeof ipv4_aliases / sizeof *ipv4_aliases; i++)
			if (parse_prefix(ipv4_aliases[i], &v[n++], FIXED_ALIAS) < 0)
				goto bad;
	if (reserved) {
		for (size_t i = 0; i < sizeof reserved_ipv4 / sizeof *reserved_ipv4; i++)
			if (parse_prefix(reserved_ipv4[i], &v[n++], FIXED_RESERVED) < 0)
				goto bad;
		for (size_t i = 0; i < sizeof reserved_ipv6 / sizeof *reserved_ipv6; i++)
			if (parse_prefix(reserved_ipv6[i], &v[n++], FIXED_RESERVED) < 0)
				goto bad;
	}
	qsort(v, n, sizeof *v, cmp_fixed);
	for (size_t i = 1; i < n; i++) {
		if (!(v[i - 1].end < v[i].start)) {
			char a[80], b[80];
			err_set("fixed networks %s and %s overlap", fmt_network(v[i - 1].start, v[i - 1].depth, a),
				fmt_network(v[i].start, v[i].depth, b));
			free(v);
			return -1;
		}
	}
	*out = v;
	*nout = n;
	return 0;
bad:
	err_set("invalid fixed network");
	free(v);
	return -1;
}

/* ---- records (records.go) ---- */

static void records_init(sw_records *r)
{
	memset(r, 0, sizeof *r);
	ht16_init(&r->ids);
	ds_init(&r->data, true);
}

static void records_free(sw_records *r)
{
	ht16_free(&r->ids);
	free(r->offsets);
	free(r->ref);
	ds_free(&r->data);
}

static bool records_is_ref(const sw_records *r, uint32_t id)
{
	return (size_t)(id >> 6) < r->ref_words && (r->ref[id >> 6] >> (id & 63) & 1);
}

static void records_mark(sw_records *r, uint32_t id)
{
	size_t w = id >> 6;
	if (w >= r->ref_words) {
		size_t nw = r->ref_words ? r->ref_words : 64;
		while (nw <= w)
			nw *= 2;
		r->ref = xrealloc(r->ref, nw * sizeof *r->ref);
		memset(r->ref + r->ref_words, 0, (nw - r->ref_words) * sizeof *r->ref);
		r->ref_words = nw;
	}
	r->ref[w] |= (uint64_t)1 << (id & 63);
}

bool sw_pending(const sw_records *r, uint32_t id)
{
	return r->frozen && id < r->n && records_is_ref(r, id) && r->offsets[id] == SW_NOT_ENCODED;
}

int sw_supply(sw_records *r, uint32_t id, sw_value_fn fn, void *ctx)
{
	mval *v;
	if (fn(ctx, &v) < 0)
		return -1;
	uint32_t off;
	if (ds_record(&r->data, v, &off) < 0)
		return -1;
	r->offsets[id] = off;
	return 0;
}

int sw_intern(sw_records *r, const uint8_t *key, size_t keylen, sw_value_fn fn, void *ctx, uint32_t *idp)
{
	if (r->keys_dropped) {
		err_set("internal error: sw_intern after the key table was freed");
		return -1;
	}
	uint8_t h[16];
	sha256_16(key, keylen, h);
	uint32_t id;
	if (!ht16_get(&r->ids, h, &id)) {
		if (r->frozen) {
			err_set("record not seen in the counting pass; the input changed between passes");
			return -1;
		}
		if (r->n == UINT32_MAX) {
			err_set("too many unique records");
			return -1;
		}
		id = (uint32_t)r->n;
		uint32_t dummy;
		ht16_get_or_put(&r->ids, h, id, &dummy);
		if (r->n == r->cap) {
			r->cap = r->cap ? r->cap * 2 : 1024;
			r->offsets = xrealloc(r->offsets, r->cap * sizeof *r->offsets);
		}
		r->offsets[r->n++] = SW_NOT_ENCODED;
		*idp = id;
		return 0;
	}
	if (sw_pending(r, id) && sw_supply(r, id, fn, ctx) < 0)
		return -1;
	*idp = id;
	return 0;
}

/* ---- cursor (tree.go) ---- */

typedef struct {
	u128 start, end;
	uint32_t record;
} span;

typedef struct {
	sw_next_fn next;
	void *ctx;
	const fixed_net *aliases;
	size_t naliases;
	bool alias_whole; /* sw_options.alias_rules_in_source */

	span cur;
	bool has, loaded;
	span pending;
	bool has_pending, eof, started;
	u128 prev_end;
	uint64_t rows, spans;
} cursor;

static void fmt_seg_addr(u128 a, bool is4, char out[64])
{
	if (is4)
		fmt_ipv4(out, (uint32_t)a);
	else
		fmt_ipv6(out, a);
}

static int check_alias(const span *s, const fixed_net *a)
{
	if (s->end < a->start || a->end < s->start)
		return 0;
	if (a->start >= s->start && a->end <= s->end) {
		u128 p = addr_parent(a->start, a->depth);
		if (p >= s->start && addr_last(p, a->depth - 1) <= s->end)
			return 0;
	}
	char x[64], y[64], z[80];
	err_set("range %s - %s inserts into the aliased network %s, which holds no data of its own",
		fmt_addr(s->start, x), fmt_addr(s->end, y), fmt_network(a->start, a->depth, z));
	return -1;
}

/* With alias_rules_in_source the rows were cut at aliased networks before
 * they were split into pieces, so no piece edge can fall inside one. */
static int check_alias_whole(const span *s, const fixed_net *a)
{
	if (s->end < a->start || a->end < s->start || (s->start <= a->start && a->end <= s->end))
		return 0;
	char x[64], y[64], z[80];
	err_set("internal error: range %s - %s cuts the aliased network %s", fmt_addr(s->start, x),
		fmt_addr(s->end, y), fmt_network(a->start, a->depth, z));
	return -1;
}

static int read_row(cursor *c, span *out, bool *ok)
{
	*ok = false;
	if (c->eof)
		return 0;
	sw_segment seg;
	bool got;
	if (c->next(c->ctx, &seg, &got) < 0)
		return -1;
	if (!got) {
		c->eof = true;
		return 0;
	}
	span s = {seg.start, seg.end, seg.record};
	if (s.end < s.start) {
		char a[64], b[64];
		fmt_seg_addr(seg.start, seg.start4, a);
		fmt_seg_addr(seg.end, seg.end4, b);
		err_set("first IP (%s) in range comes after last IP (%s)", a, b);
		return -1;
	}
	if (c->started && !(c->prev_end < s.start)) {
		char a[64], b[64], p[64];
		fmt_seg_addr(seg.start, seg.start4, a);
		fmt_seg_addr(seg.end, seg.end4, b);
		err_set("range %s - %s starts at or before the end of the previous range (%s): the streaming "
			"writer needs input sorted by start IP with no overlaps",
			a, b, fmt_addr(c->prev_end, p));
		return -1;
	}
	for (size_t i = 0; i < c->naliases; i++)
		if ((c->alias_whole ? check_alias_whole(&s, &c->aliases[i]) : check_alias(&s, &c->aliases[i])) < 0)
			return -1;
	c->started = true;
	c->prev_end = s.end;
	c->rows++;
	*out = s;
	*ok = true;
	return 0;
}

static int peek(cursor *c, span *out, bool *ok)
{
	if (c->loaded) {
		*out = c->cur;
		*ok = c->has;
		return 0;
	}
	span s = c->pending;
	if (c->has_pending) {
		c->has_pending = false;
	} else {
		bool got;
		if (read_row(c, &s, &got) < 0)
			return -1;
		if (!got) {
			c->has = false;
			c->loaded = true;
			*ok = false;
			return 0;
		}
	}
	for (;;) {
		span row;
		bool got;
		if (read_row(c, &row, &got) < 0)
			return -1;
		if (!got)
			break;
		if (s.end != U128_MAX && s.end + 1 == row.start && row.record == s.record) {
			s.end = row.end;
			continue;
		}
		c->pending = row;
		c->has_pending = true;
		break;
	}
	c->cur = s;
	c->has = c->loaded = true;
	c->spans++;
	*out = s;
	*ok = true;
	return 0;
}

static int skip_through(cursor *c, u128 hi)
{
	for (;;) {
		span s;
		bool ok;
		if (peek(c, &s, &ok) < 0)
			return -1;
		if (!ok || hi < s.end)
			return 0;
		c->loaded = false;
	}
}

/* ---- tree writer ---- */

#define WINDOW_BYTES (32u << 20)

typedef struct {
	int fd;
	int record_size, node_bytes;
	uint8_t *buf;
	uint64_t start, window, nodes;
} tree_writer;

static void tw_init(tree_writer *t, int fd, int record_size, uint64_t nodes)
{
	t->fd = fd;
	t->record_size = record_size;
	t->node_bytes = record_size * 2 / 8;
	t->nodes = nodes;
	uint64_t window = WINDOW_BYTES / (uint64_t)t->node_bytes;
	uint64_t want = nodes > 1 ? nodes : 1;
	t->window = window < want ? window : want;
	t->buf = xcalloc((size_t)t->window, (size_t)t->node_bytes);
	t->start = 0;
}

static void tw_pack(const tree_writer *t, uint8_t *b, uint32_t l, uint32_t r)
{
	switch (t->record_size) {
	case 24:
		b[0] = (uint8_t)(l >> 16); b[1] = (uint8_t)(l >> 8); b[2] = (uint8_t)l;
		b[3] = (uint8_t)(r >> 16); b[4] = (uint8_t)(r >> 8); b[5] = (uint8_t)r;
		break;
	case 28:
		b[0] = (uint8_t)(l >> 16); b[1] = (uint8_t)(l >> 8); b[2] = (uint8_t)l;
		b[3] = (uint8_t)((l >> 24 & 0x0F) << 4 | (r >> 24 & 0x0F));
		b[4] = (uint8_t)(r >> 16); b[5] = (uint8_t)(r >> 8); b[6] = (uint8_t)r;
		break;
	default:
		for (int i = 0; i < 4; i++) {
			b[i] = (uint8_t)(l >> (24 - 8 * i));
			b[4 + i] = (uint8_t)(r >> (24 - 8 * i));
		}
	}
}

static int tw_flush(tree_writer *t)
{
	uint64_t count = t->nodes - t->start < t->window ? t->nodes - t->start : t->window;
	if (pwrite_all(t->fd, t->buf, (size_t)(count * (uint64_t)t->node_bytes), t->start * (uint64_t)t->node_bytes) < 0)
		return -1;
	memset(t->buf, 0, (size_t)(t->window * (uint64_t)t->node_bytes));
	t->start += count;
	return 0;
}

static int tw_put(tree_writer *t, uint64_t n, uint32_t l, uint32_t r)
{
	if (n >= t->nodes) {
		err_set("internal error: node %llu beyond the %llu counted", (unsigned long long)n,
			(unsigned long long)t->nodes);
		return -1;
	}
	while (n >= t->start + t->window)
		if (tw_flush(t) < 0)
			return -1;
	if (n >= t->start) {
		tw_pack(t, t->buf + (size_t)(n - t->start) * (size_t)t->node_bytes, l, r);
		return 0;
	}
	uint8_t one[8];
	tw_pack(t, one, l, r);
	return pwrite_all(t->fd, one, (size_t)t->node_bytes, n * (uint64_t)t->node_bytes);
}

static int tw_finish(tree_writer *t)
{
	while (t->start < t->nodes)
		if (tw_flush(t) < 0)
			return -1;
	return 0;
}

/* ---- builder (tree.go) ---- */

enum { REC_EMPTY, REC_DATA, REC_NODE, REC_RESERVED, REC_ALIAS };

typedef struct {
	uint8_t kind;
	uint32_t val;
} record;

typedef struct {
	cursor cur;
	const fixed_net *fixed;
	size_t nfixed, fi;
	bool aliasing;
	uint64_t next, ipv4_root;
	sw_records *records;
	tree_writer *out;
	uint64_t nodes, limit;
} builder;

static void use_rec(builder *b, record r)
{
	if (r.kind == REC_DATA)
		records_mark(b->records, r.val);
}

static int rec_value(builder *b, record r, uint32_t *v)
{
	switch (r.kind) {
	case REC_NODE:
		*v = r.val;
		return 0;
	case REC_DATA: {
		uint32_t off = b->records->offsets[r.val];
		if (off == SW_NOT_ENCODED) {
			err_set("internal error: record %u used before it was encoded", r.val);
			return -1;
		}
		uint64_t x = b->nodes + 16 + (uint64_t)off;
		if (x >= b->limit) {
			err_set("%llu nodes plus %lld bytes of data exceed what a %llu-value record can address",
				(unsigned long long)b->nodes, (long long)off, (unsigned long long)b->limit);
			return -1;
		}
		*v = (uint32_t)x;
		return 0;
	}
	case REC_ALIAS:
		*v = (uint32_t)b->ipv4_root;
		return 0;
	default:
		*v = (uint32_t)b->nodes;
		return 0;
	}
}

static int build(builder *b, u128 lo, int depth, record *out)
{
	u128 hi = addr_last(lo, depth);
	while (b->fi < b->nfixed && b->fixed[b->fi].end < lo)
		b->fi++;
	const fixed_net *f = NULL;
	if (b->fi < b->nfixed && !(hi < b->fixed[b->fi].start))
		f = &b->fixed[b->fi];

	if (f && f->start == lo && f->depth == depth) {
		if (skip_through(&b->cur, hi) < 0)
			return -1;
		out->kind = f->kind == FIXED_ALIAS ? REC_ALIAS : REC_RESERVED;
		out->val = 0;
		return 0;
	}

	bool fixed = f != NULL || (b->aliasing && depth <= 96 && lo == 0);
	if (!fixed) {
		span s;
		bool ok;
		if (peek(&b->cur, &s, &ok) < 0)
			return -1;
		if (!ok || hi < s.start) {
			out->kind = REC_EMPTY;
			out->val = 0;
			return 0;
		}
		if (!(lo < s.start) && !(s.end < hi)) {
			if (skip_through(&b->cur, hi) < 0)
				return -1;
			out->kind = REC_DATA;
			out->val = s.record;
			return 0;
		}
	}
	if (depth == 128) {
		char net[80];
		err_set("internal error: cannot split %s", fmt_network(lo, depth, net));
		return -1;
	}

	uint64_t n = b->next++;
	if (b->aliasing && depth == 96 && lo == 0)
		b->ipv4_root = n;
	record left, right;
	if (build(b, lo, depth + 1, &left) < 0 || build(b, addr_right(lo, depth), depth + 1, &right) < 0)
		return -1;

	if (!fixed && left.kind == right.kind &&
	    (left.kind == REC_EMPTY || (left.kind == REC_DATA && left.val == right.val))) {
		if (b->next != n + 1) {
			err_set("internal error: node %llu collapsed but its children took node numbers",
				(unsigned long long)n);
			return -1;
		}
		b->next = n;
		*out = left;
		return 0;
	}
	use_rec(b, left);
	use_rec(b, right);
	if (b->out) {
		uint32_t l, r;
		if (rec_value(b, left, &l) < 0 || rec_value(b, right, &r) < 0 || tw_put(b->out, n, l, r) < 0)
			return -1;
	}
	out->kind = REC_NODE;
	out->val = (uint32_t)n;
	return 0;
}

static int drain(builder *b)
{
	span s;
	bool ok;
	if (peek(&b->cur, &s, &ok) < 0)
		return -1;
	if (ok) {
		err_set("internal error: input left over after the last network");
		return -1;
	}
	return 0;
}

/* ---- writer (writer.go) ---- */

struct sw_writer {
	sw_options opts;
	fixed_net *fixed;
	size_t nfixed;
	fixed_net *aliases;
	size_t naliases;
	sw_records records;
	uint64_t nodes;
	bool root_leaf, counted;
	sw_stats stats;
};

sw_writer *sw_new(const sw_options *opts)
{
	sw_writer *w = xcalloc(1, sizeof *w);
	w->opts = *opts;
	if (w->opts.record_size == 0)
		w->opts.record_size = 32;
	int rs = w->opts.record_size;
	if (rs != 24 && rs != 28 && rs != 32) {
		err_set("record size must be 24, 28 or 32, not %d", rs);
		free(w);
		return NULL;
	}
	if (w->opts.ip_version == 0)
		w->opts.ip_version = 6;
	if (w->opts.ip_version != 4 && w->opts.ip_version != 6) {
		err_set("ip version must be 4 or 6, not %d", w->opts.ip_version);
		free(w);
		return NULL;
	}
	if (w->opts.ip_version == 4)
		w->opts.alias_ipv6_to_ipv4 = false; /* an IPv4 tree has no IPv6 to alias */
	if (fixed_networks(w->opts.alias_ipv6_to_ipv4, w->opts.remove_reserved_networks, &w->fixed, &w->nfixed) < 0) {
		free(w);
		return NULL;
	}
	w->aliases = xmalloc(sizeof *w->aliases * (w->nfixed + 1));
	for (size_t i = 0; i < w->nfixed; i++)
		if (w->fixed[i].kind == FIXED_ALIAS)
			w->aliases[w->naliases++] = w->fixed[i];
	records_init(&w->records);
	w->records.data.dedup_records = !w->opts.distinct_encodings;
	return w;
}

void sw_free(sw_writer *w)
{
	if (!w)
		return;
	records_free(&w->records);
	free(w->fixed);
	free(w->aliases);
	free(w);
}

void sw_reserve(sw_writer *w, size_t n)
{
	sw_records *r = &w->records;
	ht16_reserve(&r->ids, n);
	if (n > r->cap) {
		r->cap = n;
		r->offsets = xrealloc(r->offsets, r->cap * sizeof *r->offsets);
	}
}

sw_records *sw_get_records(sw_writer *w) { return &w->records; }

sw_stats sw_get_stats(const sw_writer *w) { return w->stats; }

double sw_ceiling_use(const sw_stats *s)
{
	return (double)(s->nodes + 16 + s->data_bytes) / (double)((uint64_t)1 << s->record_size);
}

/* An IPv4 tree is the ::/96 subtree on its own. */
static int root_depth(const sw_writer *w) { return w->opts.ip_version == 4 ? 96 : 0; }

int sw_set_record_size(sw_writer *w, int record_size)
{
	if (record_size != 24 && record_size != 28 && record_size != 32) {
		err_set("record size must be 24, 28 or 32, not %d", record_size);
		return -1;
	}
	if (w->nodes + 16 >= (uint64_t)1 << record_size) {
		err_set("%llu nodes exceed what a %d-bit record can address", (unsigned long long)w->nodes, record_size);
		return -1;
	}
	w->opts.record_size = record_size;
	w->stats.record_size = record_size;
	return 0;
}

static void builder_init(builder *b, sw_writer *w, sw_next_fn next, void *ctx, tree_writer *out)
{
	memset(b, 0, sizeof *b);
	b->cur.next = next;
	b->cur.ctx = ctx;
	b->cur.aliases = w->aliases;
	b->cur.naliases = w->naliases;
	b->cur.alias_whole = w->opts.alias_rules_in_source;
	b->fixed = w->fixed;
	b->nfixed = w->nfixed;
	b->aliasing = w->opts.alias_ipv6_to_ipv4;
	b->records = &w->records;
	b->out = out;
	b->nodes = w->nodes;
	b->limit = (uint64_t)1 << w->opts.record_size;
}

int sw_count(sw_writer *w, sw_next_fn next, void *ctx)
{
	if (w->counted) {
		err_set("streamwriter: Count already ran");
		return -1;
	}
	builder b;
	builder_init(&b, w, next, ctx, NULL);
	record root;
	if (build(&b, 0, root_depth(w), &root) < 0 || drain(&b) < 0)
		return -1;
	w->nodes = b.next;
	if (root.kind != REC_NODE) {
		w->nodes = 1;
		w->root_leaf = true;
		use_rec(&b, root);
	}
	memset(&w->stats, 0, sizeof w->stats);
	w->stats.rows = b.cur.rows;
	w->stats.spans = b.cur.spans;
	w->stats.records = w->records.n;
	w->stats.nodes = w->nodes;
	w->stats.record_size = w->opts.record_size;
	if (w->nodes + 16 >= b.limit) {
		err_set("%llu nodes exceed what a %d-bit record can address", (unsigned long long)w->nodes,
			w->opts.record_size);
		return -1;
	}
	/* freeze; ids past the end of the bitmap read as unreferenced */
	w->records.frozen = true;
	if (w->opts.ids_kept_by_source) {
		ht16_free(&w->records.ids);
		w->records.keys_dropped = true;
	}
	w->counted = true;
	return 0;
}

static int build_metadata(const sw_writer *w, buf_t *out)
{
	const char *keys[] = {"binary_format_major_version", "binary_format_minor_version", "build_epoch",
			      "database_type", "description", "ip_version", "languages", "node_count", "record_size"};
	enum { NKEYS = 9 };
	mval items[2 * NKEYS];
	memset(items, 0, sizeof items);
	int nd = w->opts.ndescription;
	mval *desc = xcalloc((size_t)(2 * nd + 1), sizeof *desc);
	for (int i = 0; i < nd; i++) {
		desc[2 * i] = (mval){.type = T_STRING, .s = (const uint8_t *)w->opts.description_langs[i],
				     .n = (uint32_t)strlen(w->opts.description_langs[i])};
		desc[2 * i + 1] = (mval){.type = T_STRING, .s = (const uint8_t *)w->opts.description_texts[i],
					 .n = (uint32_t)strlen(w->opts.description_texts[i])};
	}
	mval dmap = {.type = T_MAP, .n = (uint32_t)nd, .items = desc};
	mval_sort_map(&dmap);
	int nl = w->opts.nlanguages;
	mval *langs = xcalloc((size_t)nl + 1, sizeof *langs);
	for (int i = 0; i < nl; i++)
		langs[i] = (mval){.type = T_STRING, .s = (const uint8_t *)w->opts.languages[i],
				  .n = (uint32_t)strlen(w->opts.languages[i])};
	mval vals[NKEYS] = {
		{.type = T_UINT16, .u = 2},
		{.type = T_UINT16, .u = 0},
		{.type = T_UINT64, .u = (uint64_t)time(NULL)},
		{.type = T_STRING, .s = (const uint8_t *)w->opts.database_type, .n = (uint32_t)strlen(w->opts.database_type)},
		dmap,
		{.type = T_UINT16, .u = (uint64_t)w->opts.ip_version},
		{.type = T_ARRAY, .n = (uint32_t)nl, .items = langs},
		{.type = T_UINT32, .u = w->nodes},
		{.type = T_UINT16, .u = (uint64_t)w->opts.record_size},
	};
	for (int i = 0; i < NKEYS; i++) {
		items[2 * i] = (mval){.type = T_STRING, .s = (const uint8_t *)keys[i], .n = (uint32_t)strlen(keys[i])};
		items[2 * i + 1] = vals[i];
	}
	mval m = {.type = T_MAP, .n = NKEYS, .items = items};
	mval_sort_map(&m);
	datasec d;
	ds_init(&d, false);
	int rc = ds_inline(&d, &m);
	if (rc == 0) {
		buf_append(out, "\xAB\xCD\xEFMaxMind.com", 14);
		buf_append(out, d.buf.p, d.buf.len);
	}
	ds_free(&d);
	free(desc);
	free(langs);
	return rc;
}

static int write_to(sw_writer *w, int fd, sw_next_fn next, void *ctx, int64_t *size)
{
	tree_writer tw;
	tw_init(&tw, fd, w->opts.record_size, w->nodes);
	uint64_t data_start = w->nodes * (uint64_t)tw.node_bytes + 16;
	datasec *d = &w->records.data;
	d->fd = fd;
	d->file_off = data_start;
	builder b;
	builder_init(&b, w, next, ctx, &tw);
	record root;
	int rc = -1;
	if (build(&b, 0, root_depth(w), &root) < 0 || drain(&b) < 0)
		goto out;
	if (b.cur.rows != w->stats.rows) {
		err_set("write pass read %llu rows but the counting pass read %llu; the input changed between passes",
			(unsigned long long)b.cur.rows, (unsigned long long)w->stats.rows);
		goto out;
	}
	if (w->root_leaf) {
		uint32_t v;
		if (rec_value(&b, root, &v) < 0 || tw_put(&tw, 0, v, v) < 0)
			goto out;
	} else if (b.next != w->nodes) {
		err_set("write pass built %llu nodes but the counting pass counted %llu; the input changed between passes",
			(unsigned long long)b.next, (unsigned long long)w->nodes);
		goto out;
	}
	if (tw_finish(&tw) < 0 || ds_flush(d) < 0)
		goto out;
	w->stats.data_bytes = ds_size(d);
	if (w->nodes + 16 + w->stats.data_bytes > b.limit) {
		err_set("%llu nodes plus %llu bytes of data exceed what a %d-bit record can address",
			(unsigned long long)w->nodes, (unsigned long long)w->stats.data_bytes, w->opts.record_size);
		goto out;
	}
	buf_t meta = {0};
	if (build_metadata(w, &meta) < 0) {
		buf_free(&meta);
		goto out;
	}
	static const uint8_t zeros[16];
	uint64_t meta_off = data_start + w->stats.data_bytes;
	if (pwrite_all(fd, zeros, 16, data_start - 16) < 0 || pwrite_all(fd, meta.p, meta.len, meta_off) < 0) {
		buf_free(&meta);
		goto out;
	}
	*size = (int64_t)(meta_off + meta.len);
	buf_free(&meta);
	rc = 0;
out:
	free(tw.buf);
	d->fd = -1;
	return rc;
}

static int mkdir_all(const char *dir)
{
	buf_t p = {0};
	buf_append(&p, dir, strlen(dir));
	char *s = (char *)buf_cstr(&p);
	for (char *c = s + 1; *c; c++) {
		if (*c != '/')
			continue;
		*c = 0;
		if (make_dir(s) < 0) {
			err_set("mkdir %s: %s", s, go_strerror(errno));
			buf_free(&p);
			return -1;
		}
		*c = '/';
	}
	if (make_dir(s) < 0) {
		err_set("mkdir %s: %s", s, go_strerror(errno));
		buf_free(&p);
		return -1;
	}
	buf_free(&p);
	return 0;
}

int sw_write_file(sw_writer *w, const char *path, sw_next_fn next, void *ctx)
{
	if (!w->counted) {
		err_set("streamwriter: Count must run before WriteFile");
		return -1;
	}
	buf_t dir = {0};
	path_dir(&dir, path);
	if (mkdir_all(buf_cstr(&dir)) < 0) {
		buf_free(&dir);
		return -1;
	}
	buf_free(&dir);
	buf_t tmp = {0};
	buf_printf(&tmp, "%s.tmp", path);
	const char *tmpp = buf_cstr(&tmp);
	int fd = open(tmpp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		err_set("open %s: %s", tmpp, go_strerror(errno));
		buf_free(&tmp);
		return -1;
	}
	int64_t size = 0;
	int rc = write_to(w, fd, next, ctx, &size);
	if (rc == 0 && fsync(fd) < 0) {
		err_set("sync %s: %s", tmpp, go_strerror(errno));
		rc = -1;
	}
	if (close(fd) < 0 && rc == 0) {
		err_set("close %s: %s", tmpp, go_strerror(errno));
		rc = -1;
	}
	if (rc == 0 && replace_file(tmpp, path) < 0) {
		err_set("rename %s %s: %s", tmpp, path, go_strerror(errno));
		rc = -1;
	}
	if (rc < 0) {
		unlink(tmpp);
		err_prefix("%s", path);
	} else {
		w->stats.file_bytes = size;
	}
	buf_free(&tmp);
	return rc;
}
