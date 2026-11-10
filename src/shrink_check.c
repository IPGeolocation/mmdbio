/* The checks of a shrunk file against its original, the lookup benchmark,
 * and the reader-memory sample taken in a child process. */
#include <stdlib.h>
#include <string.h>

#include "cmds.h"
#include "compat.h"
#include "netaddr.h"
#include "shrink.h"

/* ---- the address streams ---- */

static uint64_t splitmix(uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

void rng_seed(rng *g, uint64_t seed)
{
	for (int i = 0; i < 4; i++)
		g->s[i] = splitmix(&seed);
}

static uint64_t rotl(uint64_t x, int k) { return x << k | x >> (64 - k); }

uint64_t rng_next(rng *g)
{
	uint64_t *s = g->s;
	uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = rotl(s[3], 45);
	return r;
}

/* ---- comparing lookups ---- */

typedef struct {
	mmdb_reader a, b;
	arena ar;
	buf_t ka, kb;
	check_report *rep;
} pair;

static void describe(const mmdb_reader *r, arena *ar, bool found, uint64_t off, int bits, buf_t *out)
{
	if (!found) {
		buf_printf(out, "no data (/%d)", bits);
		return;
	}
	dnode *v;
	uint64_t next;
	arena_reset(ar);
	if (mdec_value(&r->data, off, ar, &v, &next) < 0) {
		buf_printf(out, "undecodable: %s", err_msg());
		return;
	}
	dnode_json(v, out);
	buf_printf(out, " (/%d)", bits);
}

/* 1 when both files answer ip the same way, 0 when not (with the detail
 * set), -1 when a lookup fails. */
static int same_answer(pair *p, u128 ip, bool is4, bool *hit)
{
	uint64_t oa = 0, ob = 0;
	bool fa, fb;
	int ba, bb;
	if (mmdb_lookup_net(&p->a, ip, is4, &oa, &fa, &ba) < 0 || mmdb_lookup_net(&p->b, ip, is4, &ob, &fb, &bb) < 0)
		return -1;
	*hit = fa;
	bool same = fa == fb;
	if (same && fa) {
		same = ba == bb;
		dnode *va, *vb;
		uint64_t next;
		arena_reset(&p->ar);
		if (same) {
			if (mdec_value(&p->a.data, oa, &p->ar, &va, &next) < 0 || mdec_value(&p->b.data, ob, &p->ar, &vb, &next) < 0)
				return -1;
			p->ka.len = p->kb.len = 0;
			dnode_canonical(va, &p->ka);
			dnode_canonical(vb, &p->kb);
			same = p->ka.len == p->kb.len && memcmp(p->ka.p, p->kb.p, p->ka.len) == 0;
		}
	}
	if (same)
		return 1;
	char t[64];
	buf_t da = {0}, db = {0};
	describe(&p->a, &p->ar, fa, oa, is4 && p->a.ip_version == 6 ? ba - 96 : ba, &da);
	describe(&p->b, &p->ar, fb, ob, is4 && p->b.ip_version == 6 ? bb - 96 : bb, &db);
	snprintf(p->rep->detail, sizeof p->rep->detail, "ip=%s:\n  original: %.*s\n  shrunk:   %.*s", fmt_ip(ip, is4, t),
		 (int)(da.len < 450 ? da.len : 450), (const char *)da.p, (int)(db.len < 450 ? db.len : 450),
		 (const char *)db.p);
	buf_free(&da);
	buf_free(&db);
	return 0;
}

static int run_one(pair *p, check_stage *st, u128 ip, bool is4)
{
	bool hit;
	int ok = same_answer(p, ip, is4, &hit);
	if (ok < 0) {
		char t[64];
		snprintf(p->rep->detail, sizeof p->rep->detail, "lookup of %s failed: %s", fmt_ip(ip, is4, t), err_msg());
		p->rep->failed = CHECK_LOOKUP;
		return -1;
	}
	if (!ok) {
		p->rep->failed = CHECK_LOOKUP;
		return -1;
	}
	st->n++;
	st->hits += hit;
	return 0;
}

static bool skip_key(const dnode *k)
{
	return (k->n == 10 && memcmp(k->s, "node_count", 10) == 0) || (k->n == 11 && memcmp(k->s, "record_size", 11) == 0);
}

/* every metadata key but node_count and record_size, with equal values */
static bool metadata_equal(const mmdb_reader *a, const mmdb_reader *b, char *why, size_t n)
{
	const dnode *ma = a->meta_root, *mb = b->meta_root;
	for (int side = 0; side < 2; side++) {
		const dnode *x = side ? mb : ma, *y = side ? ma : mb;
		for (uint32_t i = 0; i < x->n; i++) {
			const dnode *k = &x->items[2 * i];
			if (skip_key(k))
				continue;
			const dnode *other = NULL;
			for (uint32_t j = 0; j < y->n; j++) {
				const dnode *k2 = &y->items[2 * j];
				if (k2->n == k->n && memcmp(k2->s, k->s, k->n) == 0) {
					other = &y->items[2 * j + 1];
					break;
				}
			}
			if (!other) {
				snprintf(why, n, "metadata key %.*s is only in the %s file", (int)k->n, (const char *)k->s,
					 side ? "shrunk" : "original");
				return false;
			}
			if (!side && !go_deep_equal(&x->items[2 * i + 1], other)) {
				snprintf(why, n, "metadata %.*s differs", (int)k->n, (const char *)k->s);
				return false;
			}
		}
	}
	return true;
}

/* edge addresses: IPv4 class and private/reserved boundaries, and their
 * IPv6 counterparts */
static const char *const FIXED4[] = {
	"0.0.0.0",	   "0.0.0.1",	     "9.255.255.255",	"10.0.0.0",	"10.255.255.255",  "11.0.0.0",
	"100.63.255.255",  "100.64.0.0",     "100.127.255.255", "100.128.0.0",	"127.0.0.0",	   "127.0.0.1",
	"127.255.255.255", "169.254.0.0",    "169.254.255.255", "172.15.255.255", "172.16.0.0",	   "172.31.255.255",
	"172.32.0.0",	   "192.0.2.0",	     "192.168.0.0",	"192.168.255.255", "223.255.255.255", "224.0.0.0",
	"239.255.255.255", "255.255.255.255",
};
static const char *const FIXED6[] = {
	"::",
	"::1",
	"::ffff:0:0",
	"::ffff:255.255.255.255",
	"2001:db8::",
	"2001:db8::1",
	"2001:db8:ffff:ffff:ffff:ffff:ffff:ffff",
	"2002::",
	"2002:808:808::",
	"fc00::",
	"fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
	"fe80::",
	"febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
	"ff00::",
	"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
};

/* the walk of every network, both files in step */
static int all_networks(pair *p, check_stage *st)
{
	net_iter ia, ib;
	net_iter_init(&ia, &p->a);
	net_iter_init(&ib, &p->b);
	ht64 same;
	ht64_init(&same);
	int rc = -1;
	for (;;) {
		u128 ipa = 0, ipb = 0;
		int ba = 0, bb = 0;
		uint64_t oa = 0, ob = 0;
		int ra = net_iter_next(&ia, &ipa, &ba, &oa), rb = net_iter_next(&ib, &ipb, &bb, &ob);
		if (ra < 0 || rb < 0) {
			snprintf(p->rep->detail, sizeof p->rep->detail, "walking the networks: %s", err_msg());
			p->rep->failed = CHECK_LOOKUP;
			goto done;
		}
		char n1[80], n2[80];
		if (ra != rb || (ra && (ipa != ipb || ba != bb))) {
			snprintf(p->rep->detail, sizeof p->rep->detail, "the networks differ: original %s, shrunk %s",
				 ra ? fmt_net(ipa, ba, n1) : "(end)", rb ? fmt_net(ipb, bb, n2) : "(end)");
			p->rep->failed = CHECK_LOOKUP;
			goto done;
		}
		if (!ra)
			break;
		st->n++;
		uint32_t known;
		if (ht64_get(&same, oa, &known) && known == (uint32_t)ob)
			continue;
		dnode *va, *vb;
		uint64_t next;
		arena_reset(&p->ar);
		if (mdec_value(&p->a.data, oa, &p->ar, &va, &next) < 0 || mdec_value(&p->b.data, ob, &p->ar, &vb, &next) < 0) {
			snprintf(p->rep->detail, sizeof p->rep->detail, "decoding %s: %s", fmt_net(ipa, ba, n1), err_msg());
			p->rep->failed = CHECK_LOOKUP;
			goto done;
		}
		p->ka.len = p->kb.len = 0;
		dnode_canonical(va, &p->ka);
		dnode_canonical(vb, &p->kb);
		if (p->ka.len != p->kb.len || memcmp(p->ka.p, p->kb.p, p->ka.len) != 0) {
			snprintf(p->rep->detail, sizeof p->rep->detail, "the record of %s differs", fmt_net(ipa, ba, n1));
			p->rep->failed = CHECK_LOOKUP;
			goto done;
		}
		uint32_t had;
		if (same.len < (1u << 22))
			ht64_get_or_put(&same, oa, (uint32_t)ob, &had);
	}
	rc = 0;
done:
	ht64_free(&same);
	net_iter_free(&ia);
	net_iter_free(&ib);
	return rc;
}

int shrink_check(const char *base, const char *shrunk, const check_opts *o, check_report *rep)
{
	memset(rep, 0, sizeof *rep);
	pair p;
	memset(&p, 0, sizeof p);
	p.rep = rep;
	arena_init(&p.ar);
	int rc = -1;
	if (mmdb_open(&p.a, base) < 0 || mmdb_open(&p.b, shrunk) < 0)
		goto done;
	rc = 0;
	if (!metadata_equal(&p.a, &p.b, rep->detail, sizeof rep->detail)) {
		rep->failed = CHECK_METADATA;
		goto done;
	}
	rep->metadata_ok = true;

	double t = mono_seconds();
	for (size_t i = 0; i < sizeof FIXED4 / sizeof *FIXED4; i++) {
		u128 ip;
		bool is4;
		parse_ip_text(FIXED4[i], strlen(FIXED4[i]), &ip, &is4);
		if (run_one(&p, &rep->fixed, ip, true) < 0)
			goto done;
	}
	if (p.a.ip_version == 6)
		for (size_t i = 0; i < sizeof FIXED6 / sizeof *FIXED6; i++) {
			u128 ip;
			bool is4;
			parse_ip_text(FIXED6[i], strlen(FIXED6[i]), &ip, &is4);
			/* the IPv4-mapped ones are looked up as IPv6 here */
			if (is4)
				ip |= (u128)0xffff << 32;
			if (run_one(&p, &rep->fixed, ip, false) < 0)
				goto done;
		}
	rep->fixed.seconds = mono_seconds() - t;

	rng g;
	rng_seed(&g, o->seed);
	t = mono_seconds();
	for (uint64_t i = 0; i < o->lookups; i++)
		if (run_one(&p, &rep->random4, (u128)(uint32_t)rng_next(&g), true) < 0)
			goto done;
	rep->random4.seconds = mono_seconds() - t;
	if (p.a.ip_version == 6) {
		t = mono_seconds();
		for (uint64_t i = 0; i < o->lookups; i++) {
			u128 ip = (u128)rng_next(&g) << 64 | rng_next(&g);
			if (run_one(&p, &rep->random6, ip, false) < 0)
				goto done;
		}
		rep->random6.seconds = mono_seconds() - t;
	}
	if (o->all_networks) {
		t = mono_seconds();
		if (all_networks(&p, &rep->networks) < 0)
			goto done;
		rep->networks.seconds = mono_seconds() - t;
	}
done:
	buf_free(&p.ka);
	buf_free(&p.kb);
	arena_free(&p.ar);
	mmdb_close(&p.a);
	mmdb_close(&p.b);
	return rc;
}

/* ---- benchmark and memory ---- */

static int timed_lookups(const mmdb_reader *r, uint64_t seed, uint64_t n, double *ns)
{
	rng g;
	rng_seed(&g, seed ^ 0xfeedfaceull);
	arena a;
	arena_init(&a);
	double t = mono_seconds();
	for (uint64_t i = 0; i < n; i++) {
		uint64_t off;
		bool found;
		if (mmdb_lookup(r, (u128)(uint32_t)rng_next(&g), true, &off, &found) < 0) {
			arena_free(&a);
			return -1;
		}
		if (found) {
			dnode *v;
			uint64_t next;
			arena_reset(&a);
			if (mdec_value(&r->data, off, &a, &v, &next) < 0) {
				arena_free(&a);
				return -1;
			}
		}
	}
	*ns = n ? (mono_seconds() - t) * 1e9 / (double)n : 0;
	arena_free(&a);
	return 0;
}

int shrink_bench(const char *path, uint64_t seed, uint64_t lookups, bench_result *out)
{
	mmdb_reader r;
	memset(out, 0, sizeof *out);
	if (mmdb_open(&r, path) < 0) {
		mmdb_close(&r);
		return -1;
	}
	int rc = timed_lookups(&r, seed, lookups, &out->ns_per_op);
	out->lookups = lookups;
	out->qps = out->ns_per_op > 0 ? 1e9 / out->ns_per_op : 0;
	mmdb_close(&r);
	return rc;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
	qsort(v, (size_t)n, sizeof *v, cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

int shrink_bench_paired(const char *a, const char *b, uint64_t seed, uint64_t lookups, int rounds, paired_result *out)
{
	memset(out, 0, sizeof *out);
	if (rounds < 1)
		rounds = 1;
	mmdb_reader r[2];
	memset(r, 0, sizeof r);
	if (mmdb_open(&r[0], a) < 0 || mmdb_open(&r[1], b) < 0) {
		mmdb_close(&r[0]);
		mmdb_close(&r[1]);
		return -1;
	}
	double *ns[2] = {xcalloc((size_t)rounds, sizeof(double)), xcalloc((size_t)rounds, sizeof(double))};
	double *ratio = xcalloc((size_t)rounds, sizeof(double));
	int rc = 0;
	for (int i = 0; i < rounds && rc == 0; i++) {
		int first = i % 2;
		for (int k = 0; k < 2 && rc == 0; k++) {
			int f = k == 0 ? first : 1 - first;
			rc = timed_lookups(&r[f], seed + (uint64_t)i, lookups, &ns[f][i]);
		}
		ratio[i] = ns[0][i] > 0 ? ns[1][i] / ns[0][i] : 0;
	}
	if (rc == 0) {
		out->rounds = rounds;
		for (int f = 0; f < 2; f++) {
			out->min[f] = out->max[f] = ns[f][0];
			for (int i = 1; i < rounds; i++) {
				if (ns[f][i] < out->min[f])
					out->min[f] = ns[f][i];
				if (ns[f][i] > out->max[f])
					out->max[f] = ns[f][i];
			}
			out->median[f] = median(ns[f], rounds);
		}
		out->ratio = median(ratio, rounds);
	}
	free(ns[0]);
	free(ns[1]);
	free(ratio);
	mmdb_close(&r[0]);
	mmdb_close(&r[1]);
	return rc;
}

int shrink_memory_probe(const char *path, uint64_t seed, uint64_t lookups)
{
	mmdb_reader r;
	if (mmdb_open(&r, path) < 0) {
		fprintf(stderr, "%s\n", err_msg());
		return 1;
	}
	double ns;
	if (timed_lookups(&r, seed, lookups, &ns) < 0) {
		fprintf(stderr, "%s\n", err_msg());
		return 1;
	}
	uint64_t resident = 0, peak = 0, minflt = 0, majflt = 0;
	if (mapped_resident_bytes(r.map, r.size, &resident) < 0)
		resident = 0;
	process_usage(&peak, &minflt, &majflt);
	printf("%llu %llu %llu %llu %.1f\n", (unsigned long long)resident, (unsigned long long)peak,
	       (unsigned long long)minflt, (unsigned long long)majflt, ns);
	mmdb_close(&r);
	return 0;
}

int shrink_memory_sample(const char *path, uint64_t seed, uint64_t lookups, memory_sample *out)
{
	memset(out, 0, sizeof *out);
	char seeds[32], ns[32];
	snprintf(seeds, sizeof seeds, "%llu", (unsigned long long)seed);
	snprintf(ns, sizeof ns, "%llu", (unsigned long long)lookups);
	const char *args[] = {"__memprobe", path, seeds, ns, NULL};
	buf_t got = {0};
	if (run_self(args, &got) < 0) {
		buf_free(&got);
		return -1;
	}
	unsigned long long a, b, c, d;
	double e;
	if (sscanf(buf_cstr(&got), "%llu %llu %llu %llu %lf", &a, &b, &c, &d, &e) != 5) {
		err_set("the memory probe printed nothing usable");
		buf_free(&got);
		return -1;
	}
	buf_free(&got);
	*out = (memory_sample){.mapped_resident = a, .peak_rss = b, .minflt = c, .majflt = d, .ns_per_op = e,
			       .lookups = lookups};
	return 0;
}
