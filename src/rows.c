#include "rows.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "compat.h"
#include "csv.h"
#include "input.h"
#include "jstream.h"
#include "mmdb_read.h"
#include "netaddr.h"
#include "utf8.h"

#define PREFIX_BYTES (64u << 10)

/* ---- the input, with its first bytes kept for sniffing ---- */

typedef struct {
	instream *in;
	uint8_t *pre;
	size_t npre, pos;
} pinput;

static ssize_t pin_read(void *ctx, uint8_t *buf, size_t cap)
{
	pinput *p = ctx;
	if (p->pos < p->npre) {
		size_t n = p->npre - p->pos < cap ? p->npre - p->pos : cap;
		memcpy(buf, p->pre + p->pos, n);
		p->pos += n;
		return (ssize_t)n;
	}
	return in_read(p->in, buf, cap);
}

/* ---- records ---- */

typedef struct field_col {
	char *key;
	size_t klen;
	int col;
	vtype type;
	const char **empties; /* --empty-if values of its column */
	int nempties;
} field_col;

typedef struct shape {
	const char *key;
	size_t klen;
	int field; /* index into fields, or -1 for a map */
	struct shape *kids;
	int nkids, cap;
} shape;

struct row_source {
	const build_opts *o;
	pinput pin;
	bool json;
	char describe[1024];
	arena a;
	uint64_t utf8_fixed;
	/* CSV */
	csv_reader csv;
	bool csv_open, pending;
	int ncols, net_col, start_col, end_col;
	field_col *fields;
	int nfields;
	int *order; /* fields in key order, for flat records */
	shape root;
	csv_reader *vals; /* where field values come from: the input, or a lookup row */
	bool blank;       /* an id the lookup lacks: every field reads as empty */
	/* --lookup */
	bool lk_on, lk_open;
	csv_reader lk;
	int id_col;       /* the input's id column */
	ht16 lk_ix;       /* hash of an id -> index into lk_offs */
	int64_t *lk_offs; /* where each id's last row starts */
	size_t lk_n, lk_cap;
	char *lk_tmp;     /* the unpacked copy of a gzipped lookup */
	char *lk_key;     /* the lookup's id column name */
	uint64_t lk_rows, lk_missing;
	uint8_t lk_last[16]; /* the previous row's id: neighbours often share one */
	bool lk_has_last, lk_last_blank;
	/* JSON */
	jstream js;
	bool js_open;
	int jmode; /* 1 keyed by network, 2 array of rows, 3 a stream of row objects */
	bool started, done, jsonl, need_comma;
	jnode first;
	char *jnet, *jstart, *jend;
};

static char *dupn(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	if (n)
		memcpy(p, s, n);
	p[n] = 0;
	return p;
}

/* Invalid UTF-8 becomes U+FFFD, so the file passes MaxMind's checks. */
static void clean_utf8(row_source *s, const uint8_t **p, size_t *n)
{
	if (utf8_valid(*p, *n))
		return;
	buf_t b = {0};
	for (size_t i = 0; i < *n;) {
		uint32_t r;
		int w = go_decode_rune(*p + i, *n - i, &r);
		if (r == UTF8_RUNE_ERROR && w == 1)
			buf_append(&b, "\xEF\xBF\xBD", 3);
		else
			buf_append(&b, *p + i, (size_t)w);
		i += (size_t)w;
	}
	uint8_t *d = arena_alloc(&s->a, b.len + 1);
	if (b.len)
		memcpy(d, b.p, b.len);
	*p = d;
	*n = b.len;
	buf_free(&b);
	s->utf8_fixed++;
}

static void trim(const uint8_t **s, size_t *n)
{
	while (*n && isspace(**s)) {
		(*s)++;
		(*n)--;
	}
	while (*n && isspace((*s)[*n - 1]))
		(*n)--;
}

/* JSON's number syntax: an integer becomes uint32, uint64, uint128 or int32
 * by size, anything else a double. */
static bool infer_number(const uint8_t *s, size_t n, mval *out)
{
	size_t i = 0;
	bool neg = false;
	if (i < n && s[i] == '-') {
		neg = true;
		i++;
	}
	if (i == n || !isdigit(s[i]) || (s[i] == '0' && i + 1 < n && isdigit(s[i + 1])))
		return false;
	size_t digits = i;
	while (i < n && isdigit(s[i]))
		i++;
	bool integer = true;
	if (i < n && s[i] == '.') {
		integer = false;
		i++;
		if (i == n || !isdigit(s[i]))
			return false;
		while (i < n && isdigit(s[i]))
			i++;
	}
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		integer = false;
		i++;
		if (i < n && (s[i] == '+' || s[i] == '-'))
			i++;
		if (i == n || !isdigit(s[i]))
			return false;
		while (i < n && isdigit(s[i]))
			i++;
	}
	if (i != n)
		return false;
	char tmp[400];
	if (n >= sizeof tmp)
		integer = false;
	if (integer) {
		u128 v = 0;
		bool overflow = false;
		for (size_t k = digits; k < n; k++) {
			u128 d = (u128)(s[k] - '0');
			if (v > (U128_MAX - d) / 10) {
				overflow = true;
				break;
			}
			v = v * 10 + d;
		}
		if (!overflow) {
			if (neg) {
				if (v <= (u128)2147483648u) {
					*out = (mval){.type = T_INT32, .u = (uint64_t)(uint32_t)(-(int64_t)v)};
					return true;
				}
			} else if (v <= 0xffffffffu) {
				*out = (mval){.type = T_UINT32, .u = (uint64_t)v};
				return true;
			} else if (v <= UINT64_MAX) {
				*out = (mval){.type = T_UINT64, .u = (uint64_t)v};
				return true;
			} else {
				*out = (mval){.type = T_UINT128, .u = (uint64_t)v, .u_hi = (uint64_t)(v >> 64)};
				return true;
			}
		}
	}
	size_t m = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
	memcpy(tmp, s, m);
	tmp[m] = 0;
	*out = (mval){.type = T_DOUBLE, .f = strtod(tmp, NULL)};
	return true;
}

static int cannot(const uint8_t *s, size_t n, vtype t, char *why, size_t wl)
{
	snprintf(why, wl, "cannot read \"%.*s\" as %s", (int)(n > 60 ? 60 : n), (const char *)s, vtype_name(t));
	return -1;
}

static bool parse_bool_word(const uint8_t *s, size_t n, bool *out)
{
	static const char *const yes[] = {"true", "t", "yes", "y", "1"};
	static const char *const no[] = {"false", "f", "no", "n", "0"};
	for (int i = 0; i < 5; i++) {
		if (strlen(yes[i]) == n && strncasecmp((const char *)s, yes[i], n) == 0) {
			*out = true;
			return true;
		}
		if (strlen(no[i]) == n && strncasecmp((const char *)s, no[i], n) == 0) {
			*out = false;
			return true;
		}
	}
	return false;
}

int conv_text(const uint8_t *s, size_t n, vtype t, char array_sep, bool omit_empty, void *arena_p, mval *out,
	      bool *omit, char *why, size_t whylen)
{
	arena *a = arena_p;
	*omit = false;
	if (t == VT_NONE || t == VT_STRING) {
		if (n == 0 && omit_empty) {
			*omit = true;
			return 0;
		}
		*out = (mval){.type = T_STRING, .s = s, .n = (uint32_t)n};
		return 0;
	}
	const uint8_t *v = s;
	size_t vn = n;
	trim(&v, &vn);
	if (vn == 0 && t != VT_ARRAY && t != VT_AUTO) {
		*omit = true; /* an empty number or boolean leaves the key out */
		return 0;
	}
	switch (t) {
	case VT_UINT16:
	case VT_UINT32:
	case VT_UINT64:
	case VT_UINT128: {
		u128 x = 0;
		size_t i = v[0] == '+' ? 1 : 0;
		if (i == vn)
			return cannot(s, n, t, why, whylen);
		for (; i < vn; i++) {
			if (!isdigit(v[i]))
				return cannot(s, n, t, why, whylen);
			u128 d = (u128)(v[i] - '0');
			if (x > (U128_MAX - d) / 10)
				return cannot(s, n, t, why, whylen);
			x = x * 10 + d;
		}
		u128 max = t == VT_UINT16 ? 65535 : t == VT_UINT32 ? 0xffffffffu : t == VT_UINT64 ? UINT64_MAX : U128_MAX;
		if (x > max)
			return cannot(s, n, t, why, whylen);
		int type = t == VT_UINT16 ? T_UINT16 : t == VT_UINT32 ? T_UINT32 : t == VT_UINT64 ? T_UINT64 : T_UINT128;
		*out = (mval){.type = (uint8_t)type, .u = (uint64_t)x, .u_hi = (uint64_t)(x >> 64)};
		return 0;
	}
	case VT_INT32: {
		char tmp[32];
		if (vn >= sizeof tmp)
			return cannot(s, n, t, why, whylen);
		memcpy(tmp, v, vn);
		tmp[vn] = 0;
		char *end;
		errno = 0;
		long long x = strtoll(tmp, &end, 10);
		if (*end || errno || x < INT32_MIN || x > INT32_MAX)
			return cannot(s, n, t, why, whylen);
		*out = (mval){.type = T_INT32, .u = (uint64_t)(uint32_t)(int32_t)x};
		return 0;
	}
	case VT_DOUBLE:
	case VT_FLOAT: {
		char tmp[400];
		if (vn >= sizeof tmp)
			return cannot(s, n, t, why, whylen);
		memcpy(tmp, v, vn);
		tmp[vn] = 0;
		char *end;
		double x = strtod(tmp, &end);
		if (*end || end == tmp)
			return cannot(s, n, t, why, whylen);
		*out = (mval){.type = t == VT_DOUBLE ? T_DOUBLE : T_FLOAT, .f = x};
		return 0;
	}
	case VT_BOOL: {
		bool b;
		if (!parse_bool_word(v, vn, &b))
			return cannot(s, n, t, why, whylen);
		*out = (mval){.type = T_BOOL, .u = b};
		return 0;
	}
	case VT_ARRAY: {
		if (vn >= 2 && v[0] == '[' && v[vn - 1] == ']') {
			v++;
			vn -= 2;
		}
		size_t count = 0;
		for (size_t i = 0; i <= vn; i++)
			if (i == vn || v[i] == (uint8_t)array_sep)
				count++;
		mval *items = arena_alloc(a, sizeof *items * (count + 1));
		size_t k = 0, start = 0;
		for (size_t i = 0; i <= vn; i++) {
			if (i < vn && v[i] != (uint8_t)array_sep)
				continue;
			const uint8_t *p = v + start;
			size_t pn = i - start;
			trim(&p, &pn);
			if (pn >= 2 && (p[0] == '"' || p[0] == '\'') && p[pn - 1] == p[0]) {
				p++;
				pn -= 2;
			}
			if (pn)
				items[k++] = (mval){.type = T_STRING, .s = p, .n = (uint32_t)pn};
			start = i + 1;
		}
		*out = (mval){.type = T_ARRAY, .n = (uint32_t)k, .items = items};
		return 0;
	}
	case VT_AUTO: {
		if (vn == 0) {
			if (omit_empty) {
				*omit = true;
				return 0;
			}
			*out = (mval){.type = T_STRING, .s = s, .n = 0};
			return 0;
		}
		if (vn == 4 && strncasecmp((const char *)v, "true", 4) == 0) {
			*out = (mval){.type = T_BOOL, .u = 1};
			return 0;
		}
		if (vn == 5 && strncasecmp((const char *)v, "false", 5) == 0) {
			*out = (mval){.type = T_BOOL, .u = 0};
			return 0;
		}
		if (v == s && vn == n && infer_number(v, vn, out))
			return 0;
		*out = (mval){.type = T_STRING, .s = s, .n = (uint32_t)n};
		return 0;
	}
	default:
		break;
	}
	return cannot(s, n, t, why, whylen);
}

/* Sorts a map's pairs by key and keeps the last of equal keys. */
static void finish_map(mval *m)
{
	mval_sort_map(m);
	uint32_t w = 0;
	for (uint32_t i = 0; i < m->n; i++) {
		if (w > 0 && m->items[2 * (w - 1)].n == m->items[2 * i].n &&
		    memcmp(m->items[2 * (w - 1)].s, m->items[2 * i].s, m->items[2 * i].n) == 0) {
			m->items[2 * (w - 1) + 1] = m->items[2 * i + 1];
			continue;
		}
		m->items[2 * w] = m->items[2 * i];
		m->items[2 * w + 1] = m->items[2 * i + 1];
		w++;
	}
	m->n = w;
}

static vtype type_for(const build_opts *o, const char *path, size_t n)
{
	for (int i = o->ntypes - 1; i >= 0; i--)
		if (strlen(o->types[i].path) == n && memcmp(o->types[i].path, path, n) == 0)
			return o->types[i].type;
	return o->infer_types ? VT_AUTO : VT_NONE;
}

static bool listed(char **list, int n, const char *k, size_t kl)
{
	for (int i = 0; i < n; i++)
		if (strlen(list[i]) == kl && memcmp(list[i], k, kl) == 0)
			return true;
	return false;
}

static bool keep_key(const build_opts *o, const char *k, size_t kl)
{
	if (o->ninclude && !listed(o->include, o->ninclude, k, kl))
		return false;
	return !listed(o->exclude, o->nexclude, k, kl);
}

/* ---- IP columns ---- */

static void normalize(const char *s, size_t n, char *out, size_t cap)
{
	size_t k = 0;
	for (size_t i = 0; i < n && k + 1 < cap; i++)
		if (isalnum((unsigned char)s[i]))
			out[k++] = (char)tolower((unsigned char)s[i]);
	out[k] = 0;
}

static const char *const START_NAMES[] = {"startip", "ipstart", "start", "from", "ipfrom", "fromip", "firstip",
					  "ipfirst", "rangestart", "startaddress", "beginip", "networkstartip",
					  "startipaddress", "lowip", "iplow", "minip", NULL};
static const char *const END_NAMES[] = {"endip", "ipend", "end", "to", "ipto", "toip", "lastip", "iplast",
					"rangeend", "endaddress", "networklastip", "endipaddress", "highip", "iphigh",
					"maxip", "stopip", NULL};
static const char *const NET_NAMES[] = {"network", "cidr", "prefix", "subnet", "iprange", "range", "ipnetwork",
					"net", "block", "netblock", "ip", "ipaddress", "address", "ips", "iprangecidr",
					NULL};

static int name_rank(const char *norm, const char *const *names)
{
	for (int i = 0; names[i]; i++)
		if (strcmp(norm, names[i]) == 0)
			return i;
	return -1;
}

/* Picks the IP columns among names[0..n): by the options' names, else by
 * the usual names. -1 when not found. */
static void pick_columns(const build_opts *o, char **names, size_t *lens, int n, int *net, int *start, int *end)
{
	*net = *start = *end = -1;
	if (o->range_first2 && n >= 2) {
		*start = 0;
		*end = 1;
		return;
	}
	if (o->start_column) {
		for (int i = 0; i < n; i++) {
			if (strlen(o->start_column) == lens[i] && memcmp(names[i], o->start_column, lens[i]) == 0)
				*start = i;
			if (strlen(o->end_column) == lens[i] && memcmp(names[i], o->end_column, lens[i]) == 0)
				*end = i;
		}
		return;
	}
	if (o->network_column) {
		for (int i = 0; i < n; i++)
			if (strlen(o->network_column) == lens[i] && memcmp(names[i], o->network_column, lens[i]) == 0)
				*net = i;
		return;
	}
	int bs = 1000, be = 1000, bn = 1000;
	for (int i = 0; i < n; i++) {
		char norm[128];
		normalize(names[i], lens[i], norm, sizeof norm);
		int r;
		if ((r = name_rank(norm, START_NAMES)) >= 0 && r < bs) {
			bs = r;
			*start = i;
		}
		if ((r = name_rank(norm, END_NAMES)) >= 0 && r < be) {
			be = r;
			*end = i;
		}
		if ((r = name_rank(norm, NET_NAMES)) >= 0 && r < bn) {
			bn = r;
			*net = i;
		}
	}
	if (*start >= 0 && *end >= 0 && *start != *end)
		*net = -1;
	else
		*start = *end = -1;
}

static int parse_cells(const uint8_t *net, size_t nn, const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
		       row *out, char *why, size_t wl)
{
	if (net) {
		if (!parse_network((const char *)net, nn, &out->start, &out->end, &out->is4)) {
			snprintf(why, wl, "invalid network \"%.*s\"", (int)(nn > 80 ? 80 : nn), (const char *)net);
			return -1;
		}
		return 0;
	}
	bool a4, b4;
	if (!parse_address((const char *)a, an, &out->start, &a4)) {
		snprintf(why, wl, "invalid start IP \"%.*s\"", (int)(an > 80 ? 80 : an), (const char *)a);
		return -1;
	}
	if (!parse_address((const char *)b, bn, &out->end, &b4)) {
		snprintf(why, wl, "invalid end IP \"%.*s\"", (int)(bn > 80 ? 80 : bn), (const char *)b);
		return -1;
	}
	out->is4 = a4 && b4;
	return 0;
}

/* ---- CSV ---- */

static bool has_suffix(const char *s, const char *suf)
{
	size_t n = strlen(s), m = strlen(suf);
	return n >= m && strcasecmp(s + n - m, suf) == 0;
}


static char detect_delimiter(const uint8_t *p, size_t n)
{
	static const char cands[] = {',', '\t', ';', '|'};
	size_t count[4] = {0};
	bool q = false;
	for (size_t i = 0; i < n && (q || p[i] != '\n'); i++) {
		if (p[i] == '"')
			q = !q;
		else if (!q)
			for (int k = 0; k < 4; k++)
				if (p[i] == (uint8_t)cands[k])
					count[k]++;
	}
	int best = 0;
	for (int k = 1; k < 4; k++)
		if (count[k] > count[best])
			best = k;
	return cands[best];
}

static int shape_add(shape *sh, const char *key, size_t klen, int field, const char *full)
{
	const char *dot = memchr(key, '.', klen);
	size_t part = dot ? (size_t)(dot - key) : klen;
	shape *kid = NULL;
	for (int i = 0; i < sh->nkids; i++)
		if (sh->kids[i].klen == part && memcmp(sh->kids[i].key, key, part) == 0)
			kid = &sh->kids[i];
	if (!kid) {
		if (sh->nkids == sh->cap) {
			sh->cap = sh->cap ? sh->cap * 2 : 8;
			sh->kids = xrealloc(sh->kids, sizeof *sh->kids * (size_t)sh->cap);
		}
		kid = &sh->kids[sh->nkids++];
		memset(kid, 0, sizeof *kid);
		kid->key = key;
		kid->klen = part;
		kid->field = dot ? -1 : field;
	} else if (!dot || kid->field >= 0) {
		err_set("columns overlap when nested: \"%s\" and another column set the same key", full);
		return -1;
	}
	return dot ? shape_add(kid, dot + 1, klen - part - 1, field, full) : 0;
}

static int cmp_shape(const void *a, const void *b)
{
	const shape *x = a, *y = b;
	size_t n = x->klen < y->klen ? x->klen : y->klen;
	int c = memcmp(x->key, y->key, n);
	return c ? c : (x->klen < y->klen ? -1 : x->klen > y->klen);
}

static void shape_sort(shape *sh)
{
	if (sh->nkids > 1)
		qsort(sh->kids, (size_t)sh->nkids, sizeof *sh->kids, cmp_shape);
	for (int i = 0; i < sh->nkids; i++)
		shape_sort(&sh->kids[i]);
}

static void shape_free(shape *sh)
{
	for (int i = 0; i < sh->nkids; i++)
		shape_free(&sh->kids[i]);
	free(sh->kids);
}

static row_source *global_sort_src;

static int cmp_field_order(const void *a, const void *b)
{
	const field_col *x = &global_sort_src->fields[*(const int *)a], *y = &global_sort_src->fields[*(const int *)b];
	size_t n = x->klen < y->klen ? x->klen : y->klen;
	int c = memcmp(x->key, y->key, n);
	return c ? c : (x->klen < y->klen ? -1 : x->klen > y->klen);
}

/* ---- fields: --rename, --empty-if, and the columns a header gives ---- */

static const char *renamed(const build_opts *o, const char *k, size_t kl)
{
	for (int i = o->nrenames - 1; i >= 0; i--)
		if (strlen(o->renames[i].column) == kl && memcmp(o->renames[i].column, k, kl) == 0)
			return o->renames[i].key;
	return NULL;
}

static void collect_empties(const build_opts *o, const char *k, size_t kl, const char ***out, int *n)
{
	*out = NULL;
	*n = 0;
	for (int i = 0; i < o->nempties; i++)
		if (strlen(o->empties[i].column) == kl && memcmp(o->empties[i].column, k, kl) == 0) {
			*out = xrealloc((void *)*out, (size_t)(*n + 1) * sizeof **out);
			(*out)[(*n)++] = o->empties[i].value;
		}
}

static bool is_empty_value(const char *const *vals, int n, const uint8_t *v, size_t vn)
{
	for (int i = 0; i < n; i++)
		if (strlen(vals[i]) == vn && (vn == 0 || memcmp(vals[i], v, vn) == 0))
			return true;
	return false;
}

static bool empty_for(const build_opts *o, const char *k, size_t kl, const uint8_t *v, size_t vn)
{
	for (int i = 0; i < o->nempties; i++)
		if (strlen(o->empties[i].column) == kl && memcmp(o->empties[i].column, k, kl) == 0 &&
		    strlen(o->empties[i].value) == vn && (vn == 0 || memcmp(o->empties[i].value, v, vn) == 0))
			return true;
	return false;
}

/* The fields of a header: every column but the skipped ones, kept by
 * --include/--exclude (which, like --type and --empty-if, name columns),
 * stored under the --rename name. A repeated name keeps the later column. */
static int build_fields(row_source *s, char **names, size_t *lens, int ncols, int skip1, int skip2, int skip3)
{
	const build_opts *o = s->o;
	s->fields = xcalloc((size_t)ncols + 1, sizeof *s->fields);
	for (int i = 0; i < ncols; i++) {
		if (i == skip1 || i == skip2 || i == skip3)
			continue;
		const uint8_t *k = (const uint8_t *)names[i];
		size_t kl = lens[i];
		clean_utf8(s, &k, &kl);
		if (!keep_key(o, (const char *)k, kl))
			continue;
		const char *nk = renamed(o, (const char *)k, kl);
		const char *key = nk ? nk : (const char *)k;
		size_t keyl = nk ? strlen(nk) : kl;
		int dup = -1;
		for (int j = 0; j < s->nfields; j++)
			if (s->fields[j].klen == keyl && memcmp(s->fields[j].key, key, keyl) == 0)
				dup = j;
		field_col *f = dup >= 0 ? &s->fields[dup] : &s->fields[s->nfields++];
		if (dup >= 0) {
			free(f->key);
			free((void *)f->empties);
		}
		f->type = type_for(o, (const char *)k, kl);
		collect_empties(o, (const char *)k, kl, &f->empties, &f->nempties);
		f->key = dupn(key, keyl);
		f->klen = keyl;
		f->col = i;
	}
	s->order = xmalloc(sizeof *s->order * (size_t)(s->nfields + 1));
	for (int i = 0; i < s->nfields; i++)
		s->order[i] = i;
	global_sort_src = s;
	if (s->nfields > 1)
		qsort(s->order, (size_t)s->nfields, sizeof *s->order, cmp_field_order);
	if (o->nest_keys) {
		for (int i = 0; i < s->nfields; i++)
			if (shape_add(&s->root, s->fields[i].key, s->fields[i].klen, i, s->fields[i].key) < 0)
				return -1;
		shape_sort(&s->root);
	}
	return 0;
}

/* ---- --lookup ---- */

/* Indexes the lookup CSV (an id's last row wins) and builds the fields from
 * its header. Nothing of the rows is kept but where they start: a row is
 * read again when an input row asks for its id. */
static int lookup_setup(row_source *s)
{
	const build_opts *o = s->o;
	const char *path = o->lookup_path;
	instream *in = in_open(path);
	if (!in) {
		err_prefix("lookup");
		return -1;
	}
	if (in_is_gzip(in)) {
		/* read back at random, so unpacked first */
		buf_t t = {0};
		buf_printf(&t, "%s/mmdbio-lookup-%ld.csv", o->tmp_dir && *o->tmp_dir ? o->tmp_dir : temp_dir(), (long)getpid());
		FILE *f = fopen(buf_cstr(&t), "wb");
		if (!f) {
			err_set("create %s: %s (set --tmp-dir)", buf_cstr(&t), go_strerror(errno));
			buf_free(&t);
			in_close(in);
			return -1;
		}
		s->lk_tmp = strdup(buf_cstr(&t));
		buf_free(&t);
		uint8_t chunk[1 << 16];
		ssize_t n;
		while ((n = in_read(in, chunk, sizeof chunk)) > 0)
			fwrite(chunk, 1, (size_t)n, f);
		bool failed = n < 0 || ferror(f);
		if (fclose(f) != 0 || failed) {
			if (n >= 0)
				err_set("write %s: %s", s->lk_tmp, go_strerror(errno));
			in_close(in);
			return -1;
		}
		path = s->lk_tmp;
	}
	in_close(in);
	char delim = has_suffix(o->lookup_path, ".tsv") || has_suffix(o->lookup_path, ".tsv.gz") ? '\t' : 0;
	if (!delim) {
		FILE *f = fopen(path, "rb");
		uint8_t head[1 << 16];
		size_t n = f ? fread(head, 1, sizeof head, f) : 0;
		if (f)
			fclose(f);
		delim = detect_delimiter(head, n);
	}
	csv_reader r;
	if (csv_open(&r, path) < 0) {
		err_prefix("lookup");
		return -1;
	}
	r.comma = (uint8_t)delim;
	r.lazy_quotes = o->lazy_quotes;
	bool eof;
	int rc = -1;
	char **names = NULL;
	size_t *lens = NULL;
	int ncols = 0;
	if (csv_read(&r, &eof) < 0 || eof) {
		if (eof)
			err_set("the lookup %s is empty", o->lookup_path);
		else
			err_prefix("lookup %s", o->lookup_path);
		goto done;
	}
	ncols = (int)r.nfields;
	names = xcalloc((size_t)ncols + 1, sizeof *names);
	lens = xcalloc((size_t)ncols + 1, sizeof *lens);
	for (int i = 0; i < ncols; i++) {
		size_t n;
		const uint8_t *f = csv_field(&r, (size_t)i, &n);
		names[i] = dupn((const char *)f, n);
		lens[i] = n;
	}
	int key = 0;
	if (o->lookup_key) {
		key = csv_find_column(&r, o->lookup_key);
		if (key < 0) {
			err_set("the lookup %s has no column \"%s\"", o->lookup_path, o->lookup_key);
			goto done;
		}
	}
	s->lk_key = dupn(names[key], lens[key]);
	if (build_fields(s, names, lens, ncols, key, -1, -1) < 0)
		goto done;
	ht16_init(&s->lk_ix);
	for (;;) {
		if (csv_read(&r, &eof) < 0) {
			err_prefix("lookup %s", o->lookup_path);
			goto done;
		}
		if (eof)
			break;
		s->lk_rows++;
		size_t n;
		const uint8_t *id = csv_field(&r, (size_t)key, &n);
		uint8_t h[16];
		siphash128(id, n, h);
		uint32_t ix;
		if (ht16_get_or_put(&s->lk_ix, h, (uint32_t)s->lk_n, &ix)) {
			s->lk_offs[ix] = r.rec_off;
			continue;
		}
		if (s->lk_n == s->lk_cap) {
			s->lk_cap = s->lk_cap ? s->lk_cap * 2 : 4096;
			s->lk_offs = xrealloc(s->lk_offs, s->lk_cap * sizeof *s->lk_offs);
		}
		s->lk_offs[s->lk_n++] = r.rec_off;
	}
	/* a row is read where it starts, a short read at a time */
	if (csv_open_at(&s->lk, path, 1 << 10) < 0) {
		err_prefix("lookup");
		goto done;
	}
	s->lk_open = true;
	s->lk.comma = (uint8_t)delim;
	s->lk.lazy_quotes = o->lazy_quotes;
	s->lk_on = true;
	rc = 0;
done:
	csv_close(&r);
	for (int i = 0; i < ncols; i++)
		free(names[i]);
	free(names);
	free(lens);
	return rc;
}

/* Points the field values at the lookup row of id, or at nothing (every
 * field empty) when the lookup has no such id. */
static int lookup_fetch(row_source *s, const uint8_t *id, size_t n)
{
	uint8_t h[16];
	siphash128(id, n, h);
	uint32_t ix;
	s->vals = &s->lk;
	if (s->lk_has_last && memcmp(h, s->lk_last, 16) == 0) {
		/* the same id as the row before: its lookup row is still read */
		s->blank = s->lk_last_blank;
		s->lk_missing += s->blank;
		return 0;
	}
	memcpy(s->lk_last, h, 16);
	s->lk_has_last = false;
	s->blank = !ht16_get(&s->lk_ix, h, &ix);
	if (s->blank) {
		s->lk_missing++;
		s->lk_has_last = true;
		s->lk_last_blank = true;
		return 0;
	}
	csv_seek(&s->lk, s->lk_offs[ix]);
	bool eof;
	if (csv_read(&s->lk, &eof) < 0 || eof) {
		if (eof)
			err_set("lookup %s: the row of id \"%.*s\" is gone", s->o->lookup_path, (int)(n < 100 ? n : 100), id);
		else
			err_prefix("lookup %s", s->o->lookup_path);
		return -1;
	}
	s->lk_has_last = true;
	s->lk_last_blank = false;
	return 0;
}

static int csv_setup(row_source *s)
{
	const build_opts *o = s->o;
	char delim = o->delimiter;
	if (!delim && (has_suffix(o->input, ".tsv") || has_suffix(o->input, ".tsv.gz")))
		delim = '\t';
	if (!delim)
		delim = detect_delimiter(s->pin.pre, s->pin.npre);
	csv_init_reader(&s->csv, pin_read, &s->pin);
	s->csv_open = true;
	s->csv.comma = (uint8_t)delim;
	s->csv.lazy_quotes = o->lazy_quotes;
	bool eof;
	if (csv_read(&s->csv, &eof) < 0) {
		err_prefix("%s", o->input);
		return -1;
	}
	if (eof) {
		err_set("%s: the input is empty", o->input);
		return -1;
	}
	s->ncols = (int)s->csv.nfields;
	char **names = xcalloc((size_t)s->ncols + 1, sizeof *names);
	size_t *lens = xcalloc((size_t)s->ncols + 1, sizeof *lens);
	bool header = o->header == 1;
	if (o->header == -1) {
		/* a header row holds no IP addresses (or integer IPs up front); a
		 * data row does */
		header = true;
		for (int i = 0; i < s->ncols && header; i++) {
			size_t n;
			const uint8_t *f = csv_field(&s->csv, (size_t)i, &n);
			u128 a, b;
			bool four;
			if (n && (memchr(f, '.', n) || memchr(f, ':', n)) && parse_network((const char *)f, n, &a, &b, &four))
				header = false;
		}
		size_t n0;
		const uint8_t *f0 = csv_field(&s->csv, 0, &n0);
		bool digits = n0 > 0;
		for (size_t i = 0; i < n0 && digits; i++)
			digits = f0[i] >= '0' && f0[i] <= '9';
		if (digits)
			header = false;
	}
	for (int i = 0; i < s->ncols; i++) {
		if (header) {
			size_t n;
			const uint8_t *f = csv_field(&s->csv, (size_t)i, &n);
			names[i] = dupn((const char *)f, n);
			lens[i] = n;
			if (n == 0) {
				free(names[i]);
				char t[32];
				snprintf(t, sizeof t, "column%d", i + 1);
				names[i] = dupn(t, strlen(t));
				lens[i] = strlen(t);
			}
		} else {
			char t[32];
			snprintf(t, sizeof t, "column%d", i + 1);
			names[i] = dupn(t, strlen(t));
			lens[i] = strlen(t);
		}
	}
	for (int i = 0; i < s->ncols && i < o->ncolumns; i++) {
		if (!*o->columns[i])
			continue;
		free(names[i]);
		names[i] = dupn(o->columns[i], strlen(o->columns[i]));
		lens[i] = strlen(o->columns[i]);
	}
	s->pending = !header;
	pick_columns(o, names, lens, s->ncols, &s->net_col, &s->start_col, &s->end_col);
	if (s->net_col < 0 && s->start_col < 0 && !o->network_column && !o->start_column) {
		/* no telling name: the first column, or the first two as a range */
		if (!header) {
			size_t n0, n1;
			const uint8_t *f0 = csv_field(&s->csv, 0, &n0);
			u128 a, b;
			bool four;
			if (parse_network((const char *)f0, n0, &a, &b, &four)) {
				if (s->ncols > 1) {
					const uint8_t *f1 = csv_field(&s->csv, 1, &n1);
					if (!memchr(f0, '/', n0) && parse_address((const char *)f1, n1, &a, &four)) {
						s->start_col = 0;
						s->end_col = 1;
					}
				}
				if (s->start_col < 0)
					s->net_col = 0;
			}
		}
	}
	int rc = -1;
	if (s->net_col < 0 && (s->start_col < 0 || s->end_col < 0)) {
		if (o->network_column)
			err_set("%s: no column named \"%s\"", o->input, o->network_column);
		else if (o->start_column)
			err_set("%s: no columns named \"%s\" and \"%s\"", o->input, o->start_column, o->end_column);
		else
			err_set("%s: cannot tell which column holds the IP networks; name it with --network-column, or "
				"--start-column and --end-column (or \"network\" in the config)",
				o->input);
		goto done;
	}
	s->vals = &s->csv;
	if (o->lookup_path) {
		if (lookup_setup(s) < 0)
			goto done;
		const char *id = o->lookup_on ? o->lookup_on : s->lk_key;
		s->id_col = -1;
		for (int i = 0; i < s->ncols; i++)
			if (strlen(id) == lens[i] && memcmp(id, names[i], lens[i]) == 0)
				s->id_col = i;
		if (s->id_col < 0) {
			err_set("%s: no id column \"%s\" for the lookup (name it with --lookup-on)", o->input, id);
			goto done;
		}
	} else if (build_fields(s, names, lens, s->ncols, s->net_col >= 0 ? s->net_col : o->joinkey_col ? 2 : -1,
				s->start_col, s->end_col) < 0) {
		goto done;
	}
	const char *dname = delim == ',' ? "comma" : delim == '\t' ? "tab" : delim == ';' ? "semicolon" : delim == '|' ? "pipe" : "custom";
	if (s->net_col >= 0)
		snprintf(s->describe, sizeof s->describe, "CSV (%s-delimited, %s); network column \"%s\"; %d fields", dname,
			 header ? "with a header" : "no header", names[s->net_col], s->nfields);
	else
		snprintf(s->describe, sizeof s->describe, "CSV (%s-delimited, %s); range columns \"%s\" - \"%s\"; %d fields",
			 dname, header ? "with a header" : "no header", names[s->start_col], names[s->end_col], s->nfields);
	if (s->lk_on) {
		size_t l = strlen(s->describe);
		snprintf(s->describe + l, sizeof s->describe - l, " from the lookup %s (%llu rows, %llu ids), by \"%s\"",
			 o->lookup_path, (unsigned long long)s->lk_rows, (unsigned long long)s->lk_n, names[s->id_col]);
	}
	rc = 0;
done:
	for (int i = 0; i < s->ncols; i++)
		free(names[i]);
	free(names);
	free(lens);
	return rc;
}

static int csv_value(row_source *s, int fi, mval *out, bool *omit, char *why, size_t wl)
{
	const field_col *f = &s->fields[fi];
	size_t n = 0;
	const uint8_t *v = (const uint8_t *)"";
	if (!s->blank)
		v = csv_field(s->vals, (size_t)f->col, &n);
	if (n && f->nempties && is_empty_value(f->empties, f->nempties, v, n))
		n = 0;
	clean_utf8(s, &v, &n);
	if (conv_text(v, n, f->type, s->o->array_sep, s->o->omit_empty, &s->a, out, omit, why, wl) < 0) {
		char tmp[300];
		snprintf(tmp, sizeof tmp, "column \"%s\": %s", f->key, why);
		snprintf(why, wl, "%s", tmp);
		return -1;
	}
	return 0;
}

static int build_shape(row_source *s, const shape *sh, mval *out, bool *omit, char *why, size_t wl)
{
	mval *items = arena_alloc(&s->a, sizeof *items * (size_t)(2 * sh->nkids + 2));
	uint32_t k = 0;
	for (int i = 0; i < sh->nkids; i++) {
		const shape *kid = &sh->kids[i];
		mval v;
		bool om;
		if (kid->field >= 0 ? csv_value(s, kid->field, &v, &om, why, wl) < 0
				    : build_shape(s, kid, &v, &om, why, wl) < 0)
			return -1;
		if (om)
			continue;
		items[2 * k] = (mval){.type = T_STRING, .s = (const uint8_t *)kid->key, .n = (uint32_t)kid->klen};
		items[2 * k + 1] = v;
		k++;
	}
	*omit = k == 0 && sh != &s->root;
	*out = (mval){.type = T_MAP, .n = k, .items = items};
	return 0;
}

static int fields_record(row_source *s, mval **out, char *why, size_t wl);

static row_status csv_next(row_source *s, row *out, char *why, size_t wl)
{
	if (!s->pending) {
		bool eof;
		if (csv_read(&s->csv, &eof) < 0) {
			snprintf(why, wl, "%s", err_msg());
			out->line = s->csv.rec_line ? s->csv.rec_line : s->csv.num_line;
			return ROW_BAD;
		}
		if (eof)
			return ROW_EOF;
	}
	s->pending = false;
	out->line = s->csv.rec_line;
	size_t nn = 0, an = 0, bn = 0;
	const uint8_t *net = s->net_col >= 0 ? csv_field(&s->csv, (size_t)s->net_col, &nn) : NULL;
	const uint8_t *a = s->start_col >= 0 ? csv_field(&s->csv, (size_t)s->start_col, &an) : NULL;
	const uint8_t *b = s->end_col >= 0 ? csv_field(&s->csv, (size_t)s->end_col, &bn) : NULL;
	if (parse_cells(net, nn, a, an, b, bn, out, why, wl) < 0)
		return ROW_BAD;
	if (s->lk_on) {
		size_t idn;
		const uint8_t *id = csv_field(&s->csv, (size_t)s->id_col, &idn);
		if (lookup_fetch(s, id, idn) < 0)
			return ROW_FATAL;
	}
	if (fields_record(s, &out->rec, why, wl) < 0)
		return ROW_BAD;
	return ROW_OK;
}

/* The record of the current row, from s->fields and the values in s->vals. */
static int fields_record(row_source *s, mval **out, char *why, size_t wl)
{
	mval *rec = arena_alloc(&s->a, sizeof *rec);
	if (s->o->nest_keys) {
		bool om;
		if (build_shape(s, &s->root, rec, &om, why, wl) < 0)
			return -1;
	} else {
		mval *items = arena_alloc(&s->a, sizeof *items * (size_t)(2 * s->nfields + 2));
		uint32_t k = 0;
		for (int i = 0; i < s->nfields; i++) {
			int fi = s->order[i];
			mval v;
			bool om;
			if (csv_value(s, fi, &v, &om, why, wl) < 0)
				return -1;
			if (om)
				continue;
			items[2 * k] = (mval){.type = T_STRING, .s = (const uint8_t *)s->fields[fi].key,
					      .n = (uint32_t)s->fields[fi].klen};
			items[2 * k + 1] = v;
			k++;
		}
		*rec = (mval){.type = T_MAP, .n = k, .items = items};
	}
	*out = rec;
	return 0;
}

/* ---- JSON ---- */

static int jconv(row_source *s, const jnode *v, buf_t *path, mval *out, bool *omit, char *why, size_t wl);

static int jconv_members(row_source *s, const jnode *obj, buf_t *path, bool top, const char *skip1,
			 const char *skip2, const char *skip3, mval *out, char *why, size_t wl)
{
	mval *items = arena_alloc(&s->a, sizeof *items * (2 * (size_t)obj->n + 2));
	uint32_t k = 0;
	size_t keep = path->len;
	for (uint32_t i = 0; i < obj->n; i++) {
		const char *key = obj->keys[i];
		size_t kl = obj->klens[i];
		if (top) {
			if ((skip1 && strlen(skip1) == kl && memcmp(skip1, key, kl) == 0) ||
			    (skip2 && strlen(skip2) == kl && memcmp(skip2, key, kl) == 0) ||
			    (skip3 && strlen(skip3) == kl && memcmp(skip3, key, kl) == 0))
				continue;
			if (!keep_key(s->o, key, kl))
				continue;
		}
		if (path->len)
			buf_push(path, '.');
		buf_append(path, key, kl);
		mval v;
		bool om;
		int rc = jconv(s, &obj->items[i], path, &v, &om, why, wl);
		path->len = keep;
		if (rc < 0)
			return -1;
		if (om)
			continue;
		const uint8_t *kp = (const uint8_t *)key;
		size_t kn = kl;
		if (top) {
			if (v.type == T_STRING && v.n && empty_for(s->o, key, kl, v.s, v.n)) {
				if (s->o->omit_empty)
					continue;
				v.n = 0;
			}
			const char *nk = renamed(s->o, key, kl);
			if (nk) {
				kp = (const uint8_t *)nk;
				kn = strlen(nk);
			}
		}
		clean_utf8(s, &kp, &kn);
		items[2 * k] = (mval){.type = T_STRING, .s = kp, .n = (uint32_t)kn};
		items[2 * k + 1] = v;
		k++;
	}
	*out = (mval){.type = T_MAP, .n = k, .items = items};
	finish_map(out);
	return 0;
}

static int jconv(row_source *s, const jnode *v, buf_t *path, mval *out, bool *omit, char *why, size_t wl)
{
	const build_opts *o = s->o;
	vtype t = type_for(o, (const char *)path->p, path->len);
	if (t == VT_AUTO && v->type != JN_STR)
		t = VT_NONE; /* JSON already says what the value is */
	*omit = false;
	buf_cstr(path);
	switch (v->type) {
	case JN_OBJ:
		if (t != VT_NONE) {
			snprintf(why, wl, "\"%s\" is an object, not %s", (const char *)path->p, vtype_name(t));
			return -1;
		}
		return jconv_members(s, v, path, false, NULL, NULL, NULL, out, why, wl);
	case JN_ARR: {
		if (t != VT_NONE && t != VT_ARRAY) {
			snprintf(why, wl, "\"%s\" is an array, not %s", (const char *)path->p, vtype_name(t));
			return -1;
		}
		mval *items = arena_alloc(&s->a, sizeof *items * ((size_t)v->n + 1));
		uint32_t k = 0;
		size_t keep = path->len;
		buf_append(path, "[]", 2);
		for (uint32_t i = 0; i < v->n; i++) {
			bool om;
			if (jconv(s, &v->items[i], path, &items[k], &om, why, wl) < 0) {
				path->len = keep;
				return -1;
			}
			if (!om)
				k++;
		}
		path->len = keep;
		*out = (mval){.type = T_ARRAY, .n = k, .items = items};
		return 0;
	}
	case JN_STR: {
		const uint8_t *p = (const uint8_t *)v->s;
		size_t n = v->len;
		clean_utf8(s, &p, &n);
		if (conv_text(p, n, t, o->array_sep, o->omit_empty, &s->a, out, omit, why, wl) < 0) {
			char tmp[300];
			snprintf(tmp, sizeof tmp, "\"%s\": %s", (const char *)path->p, why);
			snprintf(why, wl, "%s", tmp);
			return -1;
		}
		return 0;
	}
	case JN_NUM:
		if (t == VT_NONE) {
			infer_number((const uint8_t *)v->s, v->len, out);
			return 0;
		}
		if (t == VT_STRING) {
			*out = (mval){.type = T_STRING, .s = (const uint8_t *)v->s, .n = (uint32_t)v->len};
			return 0;
		}
		if (t == VT_ARRAY || t == VT_BOOL) {
			snprintf(why, wl, "\"%s\" is a number, not %s", (const char *)path->p, vtype_name(t));
			return -1;
		}
		if (conv_text((const uint8_t *)v->s, v->len, t, o->array_sep, false, &s->a, out, omit, why, wl) < 0) {
			char tmp[300];
			snprintf(tmp, sizeof tmp, "\"%s\": %s", (const char *)path->p, why);
			snprintf(why, wl, "%s", tmp);
			return -1;
		}
		return 0;
	case JN_BOOL:
		if (t == VT_STRING) {
			*out = (mval){.type = T_STRING, .s = (const uint8_t *)(v->b ? "true" : "false"), .n = v->b ? 4 : 5};
			return 0;
		}
		if (t != VT_NONE && t != VT_BOOL) {
			snprintf(why, wl, "\"%s\" is a boolean, not %s", (const char *)path->p, vtype_name(t));
			return -1;
		}
		*out = (mval){.type = T_BOOL, .u = v->b};
		return 0;
	default: /* null, as the Go import stored it: "" */
		if (o->omit_empty || (t != VT_NONE && t != VT_STRING)) {
			*omit = true;
			return 0;
		}
		*out = (mval){.type = T_STRING, .s = (const uint8_t *)"", .n = 0};
		return 0;
	}
}

static int cell_text(const jnode *v, const uint8_t **p, size_t *n)
{
	if (v->type == JN_STR || v->type == JN_NUM) {
		*p = (const uint8_t *)v->s;
		*n = v->len;
		return 0;
	}
	return -1;
}

static const jnode *member(const jnode *obj, const char *key)
{
	if (!key)
		return NULL;
	for (uint32_t i = 0; i < obj->n; i++)
		if (strlen(key) == obj->klens[i] && memcmp(obj->keys[i], key, obj->klens[i]) == 0)
			return &obj->items[i];
	return NULL;
}

static int detect_json_columns(row_source *s, const jnode *obj)
{
	char **names = xcalloc((size_t)obj->n + 1, sizeof *names);
	size_t *lens = xcalloc((size_t)obj->n + 1, sizeof *lens);
	for (uint32_t i = 0; i < obj->n; i++) {
		names[i] = (char *)obj->keys[i];
		lens[i] = obj->klens[i];
	}
	int net, a, b;
	pick_columns(s->o, names, lens, (int)obj->n, &net, &a, &b);
	if (net >= 0)
		s->jnet = dupn(names[net], lens[net]);
	if (a >= 0 && b >= 0) {
		s->jstart = dupn(names[a], lens[a]);
		s->jend = dupn(names[b], lens[b]);
	}
	free(names);
	free(lens);
	if (!s->jnet && !s->jstart) {
		err_set("%s: line %ld: cannot tell which key holds the IP network; name it with --network-column, or "
			"--start-column and --end-column (or \"network\" in the config)",
			s->o->input, obj->line);
		return -1;
	}
	if (s->jnet)
		snprintf(s->describe, sizeof s->describe, "JSON (%s); network key \"%s\"",
			 s->jmode == 2 ? "an array of objects" : "one object per row", s->jnet);
	else
		snprintf(s->describe, sizeof s->describe, "JSON (%s); range keys \"%s\" - \"%s\"",
			 s->jmode == 2 ? "an array of objects" : "one object per row", s->jstart, s->jend);
	return 0;
}

static row_status json_row_object(row_source *s, const jnode *obj, row *out, char *why, size_t wl)
{
	out->line = obj->line;
	if (obj->type != JN_OBJ) {
		snprintf(why, wl, "expected an object for each row");
		return ROW_BAD;
	}
	if (!s->jnet && !s->jstart && detect_json_columns(s, obj) < 0)
		return ROW_FATAL;
	const uint8_t *np = NULL, *ap = NULL, *bp = NULL;
	size_t nn = 0, an = 0, bn = 0;
	if (s->jnet) {
		const jnode *v = member(obj, s->jnet);
		if (!v || cell_text(v, &np, &nn) < 0) {
			snprintf(why, wl, "no \"%s\" network", s->jnet);
			return ROW_BAD;
		}
	} else {
		const jnode *a = member(obj, s->jstart), *b = member(obj, s->jend);
		if (!a || !b || cell_text(a, &ap, &an) < 0 || cell_text(b, &bp, &bn) < 0) {
			snprintf(why, wl, "no \"%s\" and \"%s\" range", s->jstart, s->jend);
			return ROW_BAD;
		}
	}
	if (parse_cells(np, nn, ap, an, bp, bn, out, why, wl) < 0)
		return ROW_BAD;
	if (s->lk_on) {
		const char *idk = s->o->lookup_on ? s->o->lookup_on : s->lk_key;
		const jnode *idv = member(obj, idk);
		const uint8_t *id;
		size_t idn;
		if (!idv || cell_text(idv, &id, &idn) < 0) {
			snprintf(why, wl, "no \"%s\" id for the lookup", idk);
			return ROW_BAD;
		}
		if (lookup_fetch(s, id, idn) < 0)
			return ROW_FATAL;
		return fields_record(s, &out->rec, why, wl) < 0 ? ROW_BAD : ROW_OK;
	}
	buf_t path = {0};
	mval *rec = arena_alloc(&s->a, sizeof *rec);
	int rc = jconv_members(s, obj, &path, true, s->jnet, s->jstart, s->jend, rec, why, wl);
	buf_free(&path);
	if (rc < 0)
		return ROW_BAD;
	out->rec = rec;
	return ROW_OK;
}

static row_status json_keyed(row_source *s, const char *key, size_t klen, const jnode *v, row *out, char *why,
			     size_t wl)
{
	out->line = v->line;
	if (!parse_network(key, klen, &out->start, &out->end, &out->is4)) {
		snprintf(why, wl, "invalid network \"%.*s\"", (int)(klen > 80 ? 80 : klen), key);
		return ROW_BAD;
	}
	buf_t path = {0};
	mval *rec = arena_alloc(&s->a, sizeof *rec);
	int rc;
	if (v->type == JN_OBJ) {
		rc = jconv_members(s, v, &path, true, NULL, NULL, NULL, rec, why, wl);
	} else {
		bool om;
		rc = jconv(s, v, &path, rec, &om, why, wl);
		if (rc == 0 && om)
			*rec = (mval){.type = T_STRING, .s = (const uint8_t *)"", .n = 0};
	}
	buf_free(&path);
	if (rc < 0)
		return ROW_BAD;
	out->rec = rec;
	return ROW_OK;
}

static row_status json_fatal(row_source *s)
{
	err_prefix("%s", s->o->input);
	s->done = true;
	return ROW_FATAL;
}

static row_status json_next(row_source *s, row *out, char *why, size_t wl)
{
	jstream *j = &s->js;
	if (s->done)
		return ROW_EOF;
	int c;
	if (!s->started) {
		s->started = true;
		c = js_peek(j);
		if (c == '[') {
			js_expect(j, '[');
			s->jmode = 2;
			if (js_peek(j) == ']') {
				js_expect(j, ']');
				s->done = true;
				return ROW_EOF;
			}
		} else if (c == '{') {
			js_expect(j, '{');
			if (js_peek(j) == '}') {
				js_expect(j, '}');
				s->done = true;
				return ROW_EOF;
			}
			const char *k;
			size_t kl;
			if (js_peek(j) != '"' || js_string(j, &k, &kl) < 0 || js_expect(j, ':') < 0)
				return json_fatal(s);
			jnode v;
			if (js_value(j, &v) < 0)
				return json_fatal(s);
			u128 a, b;
			bool four;
			if (!s->jsonl && parse_network(k, kl, &a, &b, &four)) {
				s->jmode = 1;
				return json_keyed(s, k, kl, &v, out, why, wl);
			}
			/* the first row object of a stream */
			s->jmode = 3;
			if (js_object_rest(j, k, kl, &v, &s->first) < 0)
				return json_fatal(s);
			return json_row_object(s, &s->first, out, why, wl);
		} else if (c == -1) {
			s->done = true;
			return ROW_EOF;
		} else {
			err_set("line %ld: the JSON input must start with { or [", j->line);
			return json_fatal(s);
		}
	}
	if (s->jmode == 1) {
		c = js_peek(j);
		if (c == '}') {
			js_expect(j, '}');
			s->done = true;
			if (js_peek(j) != -1) {
				err_set("line %ld: data after the end of the JSON object", j->line);
				return json_fatal(s);
			}
			return ROW_EOF;
		}
		if (js_expect(j, ',') < 0 || js_peek(j) != '"')
			return json_fatal(s);
		const char *k;
		size_t kl;
		jnode v;
		if (js_string(j, &k, &kl) < 0 || js_expect(j, ':') < 0 || js_value(j, &v) < 0)
			return json_fatal(s);
		return json_keyed(s, k, kl, &v, out, why, wl);
	}
	if (s->jmode == 2) {
		jnode v;
		if (s->need_comma) {
			c = js_peek(j);
			if (c == ']') {
				js_expect(j, ']');
				s->done = true;
				return ROW_EOF;
			}
			if (js_expect(j, ',') < 0)
				return json_fatal(s);
		}
		s->need_comma = true;
		if (js_value(j, &v) < 0)
			return json_fatal(s);
		return json_row_object(s, &v, out, why, wl);
	}
	/* jmode 3: further objects, one after another (NDJSON) */
	c = js_peek(j);
	if (c == -1) {
		s->done = true;
		return ROW_EOF;
	}
	if (c == ',')
		js_expect(j, ',');
	jnode v;
	if (js_value(j, &v) < 0)
		return json_fatal(s);
	return json_row_object(s, &v, out, why, wl);
}

/* ---- the source ---- */

row_source *rows_open(const build_opts *o)
{
	row_source *s = xcalloc(1, sizeof *s);
	s->o = o;
	arena_init(&s->a);
	s->pin.in = in_open(o->input);
	if (!s->pin.in) {
		free(s);
		return NULL;
	}
	s->pin.pre = xmalloc(PREFIX_BYTES);
	while (s->pin.npre < PREFIX_BYTES) {
		ssize_t n = in_read(s->pin.in, s->pin.pre + s->pin.npre, PREFIX_BYTES - s->pin.npre);
		if (n < 0) {
			rows_close(s);
			return NULL;
		}
		if (n == 0)
			break;
		s->pin.npre += (size_t)n;
	}
	in_format f = o->format;
	if (f == FMT_AUTO) {
		char base[1024];
		snprintf(base, sizeof base, "%s", o->input);
		size_t bl = strlen(base);
		if (has_suffix(base, ".gz"))
			base[bl - 3] = 0;
		if (has_suffix(base, ".jsonl") || has_suffix(base, ".ndjson")) {
			f = FMT_JSONL;
		} else if (has_suffix(base, ".json")) {
			f = FMT_JSON;
		} else if (has_suffix(base, ".csv") || has_suffix(base, ".tsv") || has_suffix(base, ".txt")) {
			f = FMT_CSV;
		} else {
			size_t i = 0;
			if (s->pin.npre >= 3 && memcmp(s->pin.pre, "\xEF\xBB\xBF", 3) == 0)
				i = 3;
			while (i < s->pin.npre && isspace(s->pin.pre[i]))
				i++;
			f = i < s->pin.npre && (s->pin.pre[i] == '{' || s->pin.pre[i] == '[') ? FMT_JSON : FMT_CSV;
		}
	}
	if (f == FMT_CSV) {
		if (csv_setup(s) < 0) {
			rows_close(s);
			return NULL;
		}
		return s;
	}
	s->json = true;
	if (o->lookup_path && lookup_setup(s) < 0) {
		rows_close(s);
		return NULL;
	}
	js_init(&s->js, pin_read, &s->pin, &s->a);
	s->js_open = true;
	s->jsonl = f == FMT_JSONL;
	snprintf(s->describe, sizeof s->describe, "JSON");
	return s;
}

row_status rows_next(row_source *s, row *out, char *why, size_t whylen)
{
	arena_reset(&s->a);
	memset(out, 0, sizeof *out);
	why[0] = 0;
	row_status st = s->json ? json_next(s, out, why, whylen) : csv_next(s, out, why, whylen);
	if (st == ROW_OK && s->json && s->jmode == 1 && strcmp(s->describe, "JSON") == 0)
		snprintf(s->describe, sizeof s->describe, "JSON (an object keyed by network)");
	return st;
}

const char *rows_describe(const row_source *s) { return s->describe; }

uint64_t rows_raw_bytes(const row_source *s) { return in_raw_bytes(s->pin.in); }

uint64_t rows_utf8_fixed(const row_source *s) { return s->utf8_fixed; }

uint64_t rows_lookup_missing(const row_source *s) { return s->lk_missing; }

void rows_close(row_source *s)
{
	if (!s)
		return;
	if (s->csv_open)
		csv_close(&s->csv);
	if (s->js_open)
		js_free(&s->js);
	for (int i = 0; i < s->nfields; i++) {
		free(s->fields[i].key);
		free((void *)s->fields[i].empties);
	}
	free(s->fields);
	if (s->lk_open)
		csv_close(&s->lk);
	if (s->lk_on || s->lk_n)
		ht16_free(&s->lk_ix);
	free(s->lk_offs);
	free(s->lk_key);
	if (s->lk_tmp) {
		unlink(s->lk_tmp);
		free(s->lk_tmp);
	}
	free(s->order);
	shape_free(&s->root);
	free(s->jnet);
	free(s->jstart);
	free(s->jend);
	in_close(s->pin.in);
	free(s->pin.pre);
	arena_free(&s->a);
	free(s);
}
