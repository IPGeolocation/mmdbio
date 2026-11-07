/* diff: networks added, removed or modified between two MMDB files. Both
 * trees are walked together in address order, so neither file is loaded
 * into memory; each network is compared by its exact prefix, as the Go tool
 * keyed them.
 *
 * --subnets and --records compare by lookup instead: each network of one
 * file is looked up in the other. A network whose address lands in a
 * network of another size there has changed boundaries; one that lands in
 * the same network with another record has a changed record. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cmds.h"
#include "color.h"
#include "compat.h"
#include "jsonw.h"
#include "netaddr.h"

static flag diff_flags[] = {
	{.name = "old", .type = F_STRING, .usage = "Path to the old MMDB file (or the first argument)"},
	{.name = "new", .type = F_STRING, .usage = "Path to the new MMDB file (or the second argument)"},
	{.name = "summary", .type = F_BOOL, .usage = "Show only summary counts"},
	{.name = "json", .type = F_BOOL, .usage = "Output the result as JSON"},
	{.name = "subnets",
	 .shorthand = 's',
	 .type = F_BOOL,
	 .usage = "List networks whose boundaries changed (old -> new), found by looking each one up in the other file"},
	{.name = "records",
	 .shorthand = 'r',
	 .type = F_BOOL,
	 .usage = "List networks that kept their boundaries but changed their record, with the old and new record"},
};

static int run_diff(command *c, int argc, char **argv);

command cmd_diff = {
	.name = "diff",
	.use = "diff [flags] [old.mmdb new.mmdb]",
	.short_desc = "Compare two MMDB files and show differences",
	.long_desc = "Compares two MMDB files and lists networks that were added, removed, or modified.",
	.examples = "  mmdbio diff --old old.mmdb --new new.mmdb\n  mmdbio diff --old old.mmdb --new new.mmdb --summary\n"
		    "  mmdbio diff --old old.mmdb --new new.mmdb --json\n  mmdbio diff old.mmdb new.mmdb --subnets --records",
	.flags = diff_flags,
	.nflags = (int)(sizeof diff_flags / sizeof diff_flags[0]),
	.run = run_diff,
};

typedef struct {
	mmdb_reader r;
	net_iter it;
	arena a;
	bool has;
	u128 ip;
	int bit;
	uint64_t off;
} side;

static int advance(side *s)
{
	int rc = net_iter_next(&s->it, &s->ip, &s->bit, &s->off);
	if (rc < 0)
		return -1;
	s->has = rc == 1;
	return 0;
}

static dnode *decode(side *s)
{
	arena_reset(&s->a);
	dnode *v;
	uint64_t next;
	if (mdec_value(&s->r.data, s->off, &s->a, &v, &next) < 0)
		return NULL;
	return v;
}

/* one section of the JSON output, members written as they come */
typedef struct {
	FILE *f;
	uint64_t n;
} section;

static void section_add(section *s, const char *net, const buf_t *json)
{
	buf_t k = {0};
	json_string(&k, (const uint8_t *)net, strlen(net));
	fputs(s->n ? ",\n    " : "\n    ", s->f);
	fwrite(k.p, 1, k.len, s->f);
	fputs(": ", s->f);
	fwrite(json->p, 1, json->len, s->f);
	s->n++;
	buf_free(&k);
}

static void section_copy(section *s, const char *name, FILE *out, bool last)
{
	fprintf(out, "  \"%s\": {", name);
	if (s->n) {
		rewind(s->f);
		char chunk[65536];
		size_t got;
		while ((got = fread(chunk, 1, sizeof chunk, s->f)) > 0)
			fwrite(chunk, 1, got, out);
		fputs("\n  }", out);
	} else {
		fputs("}", out);
	}
	fputs(last ? "\n" : ",\n", out);
}

/* ---- by lookup ---- */

typedef struct {
	u128 old_ip, new_ip;
	uint8_t old_bit, new_bit;
	bool old_data, new_data;
} subnet_change;

typedef struct {
	subnet_change *v;
	size_t n, cap;
} changes;

static void change_add(changes *c, subnet_change x)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 256;
		c->v = xrealloc(c->v, c->cap * sizeof *c->v);
	}
	c->v[c->n++] = x;
}

static int cmp_change(const void *a, const void *b)
{
	const subnet_change *x = a, *y = b;
	if (x->old_ip != y->old_ip)
		return x->old_ip < y->old_ip ? -1 : 1;
	if (x->old_bit != y->old_bit)
		return x->old_bit < y->old_bit ? -1 : 1;
	if (x->new_ip != y->new_ip)
		return x->new_ip < y->new_ip ? -1 : 1;
	return x->new_bit < y->new_bit ? -1 : x->new_bit > y->new_bit;
}

static u128 mask(u128 ip, int bits) { return bits <= 0 ? 0 : bits >= 128 ? ip : ip & ~(U128_MAX >> bits); }

/* the network of `other` holding the first address of ip/bit */
static int locate(const mmdb_reader *other, u128 ip, int bit, uint64_t *off, bool *found, int *obits)
{
	bool is4 = (ip >> 32) == 0 && bit >= 96;
	if (is4 || other->ip_version == 6)
		return mmdb_lookup_net(other, is4 ? (u128)(uint32_t)ip : ip, is4, off, found, obits);
	*found = false; /* an IPv6 network, an IPv4-only file */
	*obits = 0;
	return 0;
}

/* Walks `from`, looking each network up in `to`. old_side: `from` is the
 * old file. Records are compared from the new file only, when records is
 * set; recs, when not NULL, gets each changed one. */
static int lookup_pass(side *from, side *to, bool old_side, changes *ch, bool records, FILE *recs, bool json,
		       uint64_t *nrecs)
{
	net_iter_free(&from->it);
	net_iter_init(&from->it, &from->r);
	u128 ip;
	int bit;
	uint64_t off;
	int rc;
	buf_t ja = {0}, jb = {0};
	char n1[80];
	while ((rc = net_iter_next(&from->it, &ip, &bit, &off)) == 1) {
		uint64_t ooff = 0;
		bool found;
		int obits;
		if (locate(&to->r, ip, bit, &ooff, &found, &obits) < 0) {
			rc = -1;
			break;
		}
		if (obits != bit) {
			u128 oip = mask(ip, obits);
			/* the other direction already reported a data network that
			 * starts where this one does */
			if (old_side && found && oip == ip)
				continue;
			subnet_change x;
			if (old_side)
				x = (subnet_change){ip, oip, (uint8_t)bit, (uint8_t)obits, true, found};
			else
				x = (subnet_change){oip, ip, (uint8_t)obits, (uint8_t)bit, found, true};
			change_add(ch, x);
			continue;
		}
		if (old_side || !records)
			continue;
		from->off = off;
		dnode *nv = decode(from), *ov = NULL;
		if (!nv) {
			rc = -1;
			break;
		}
		if (found) {
			to->off = ooff;
			if (!(ov = decode(to))) {
				rc = -1;
				break;
			}
			if (go_deep_equal(ov, nv))
				continue;
		}
		(*nrecs)++;
		if (!recs)
			continue;
		fmt_net(ip, bit, n1);
		ja.len = jb.len = 0;
		if (json) {
			if (ov)
				jw_value(&ja, ov, "", 3);
			else
				buf_append(&ja, "null", 4);
			jw_value(&jb, nv, "", 3);
			buf_t k = {0};
			json_string(&k, (const uint8_t *)n1, strlen(n1));
			fprintf(recs, "%s\n    %.*s: {\n      \"new\": %.*s,\n      \"old\": %.*s\n    }", *nrecs > 1 ? "," : "",
				(int)k.len, (const char *)k.p, (int)jb.len, (const char *)jb.p, (int)ja.len, (const char *)ja.p);
			buf_free(&k);
		} else {
			if (ov)
				dnode_json(ov, &ja);
			dnode_json(nv, &jb);
			fprintf(recs, "   %s\n      %s- %.*s%s\n      %s+ %.*s%s\n", n1, col(stdout, C_RED),
				ov ? (int)ja.len : 7, ov ? (const char *)ja.p : "no data", col_end(stdout), col(stdout, C_GREEN),
				(int)jb.len, (const char *)jb.p, col_end(stdout));
		}
	}
	buf_free(&ja);
	buf_free(&jb);
	return rc < 0 ? -1 : 0;
}

static void net_text(u128 ip, int bit, bool data, char out[96])
{
	char n[80];
	fmt_net(ip, bit, n);
	snprintf(out, 96, "%s%s", n, data ? "" : " (no data)");
}

static int lookup_diff(side *o, side *n, bool subnets, bool records, bool json, bool summary)
{
	if (o->r.ip_version != n->r.ip_version) {
		fprintf(stderr, "Error: the files' IP versions differ: old %llu, new %llu\n", (unsigned long long)o->r.ip_version,
			(unsigned long long)n->r.ip_version);
		return 1;
	}
	changes ch = {0};
	FILE *recs = records && !summary ? temp_stream() : NULL;
	if (records && !summary && !recs) {
		fprintf(stderr, "Error: tmpfile: %s\n", go_strerror(errno));
		return 1;
	}
	uint64_t nrecs = 0;
	if (lookup_pass(n, o, false, &ch, records, recs, json, &nrecs) < 0 ||
	    lookup_pass(o, n, true, &ch, false, NULL, json, &nrecs) < 0) {
		fprintf(stderr, "Error: %s\n", err_msg());
		if (recs)
			fclose(recs);
		free(ch.v);
		return 1;
	}
	if (ch.n > 1)
		qsort(ch.v, ch.n, sizeof *ch.v, cmp_change);
	char a[96], b[96];
	if (summary) {
		if (subnets && records)
			printf("Subnets changed: %llu | Records changed: %llu\n", (unsigned long long)ch.n,
			       (unsigned long long)nrecs);
		else if (subnets)
			printf("Subnets changed: %llu\n", (unsigned long long)ch.n);
		else
			printf("Records changed: %llu\n", (unsigned long long)nrecs);
	} else if (json) {
		printf("{");
		if (records) {
			printf("\n  \"records\": {");
			if (nrecs) {
				fflush(stdout);
				rewind(recs);
				char chunk[65536];
				size_t got;
				while ((got = fread(chunk, 1, sizeof chunk, recs)) > 0)
					fwrite(chunk, 1, got, stdout);
				printf("\n  }");
			} else {
				printf("}");
			}
			if (subnets)
				printf(",");
		}
		if (subnets) {
			printf("\n  \"subnets\": [");
			for (size_t i = 0; i < ch.n; i++) {
				subnet_change *x = &ch.v[i];
				char on[80], nn[80];
				fmt_net(x->old_ip, x->old_bit, on);
				fmt_net(x->new_ip, x->new_bit, nn);
				printf("%s\n    {\n      \"new\": \"%s\",\n      \"new_data\": %s,\n      \"old\": \"%s\",\n"
				       "      \"old_data\": %s\n    }",
				       i ? "," : "", nn, x->new_data ? "true" : "false", on, x->old_data ? "true" : "false");
			}
			printf(ch.n ? "\n  ]" : "]");
		}
		printf("\n}\n");
	} else {
		if (subnets) {
			printf("Subnets changed: %llu\n", (unsigned long long)ch.n);
			for (size_t i = 0; i < ch.n; i++) {
				subnet_change *x = &ch.v[i];
				net_text(x->old_ip, x->old_bit, x->old_data, a);
				net_text(x->new_ip, x->new_bit, x->new_data, b);
				printf("   %s%s%s -> %s%s%s\n", col(stdout, C_RED), a, col_end(stdout), col(stdout, C_GREEN), b,
				       col_end(stdout));
			}
		}
		if (records) {
			if (subnets)
				printf("\n");
			printf("Records changed: %llu\n", (unsigned long long)nrecs);
			fflush(stdout);
			rewind(recs);
			char chunk[65536];
			size_t got;
			while ((got = fread(chunk, 1, sizeof chunk, recs)) > 0)
				fwrite(chunk, 1, got, stdout);
		}
	}
	if (recs)
		fclose(recs);
	free(ch.v);
	return 0;
}

static int run_diff(command *c, int argc, char **argv)
{
	const char *oldp = flag_get(c, "old")->str, *newp = flag_get(c, "new")->str;
	int pos = 0;
	if (!*oldp && pos < argc)
		oldp = argv[pos++];
	if (!*newp && pos < argc)
		newp = argv[pos++];
	if (pos < argc) {
		fprintf(stderr, "Error: unexpected argument \"%s\"\n", argv[pos]);
		return 1;
	}
	if (!*oldp || !*newp) {
		fprintf(stderr, "Error: both --old and --new flags are required\n");
		command_help(c, stderr);
		return 1;
	}
	side o = {0}, n = {0};
	if (mmdb_open(&o.r, oldp) < 0) {
		fprintf(stderr, "Error: failed to open old db: %s\n", err_msg());
		mmdb_close(&o.r);
		return 1;
	}
	if (mmdb_open(&n.r, newp) < 0) {
		fprintf(stderr, "Error: failed to open new db: %s\n", err_msg());
		mmdb_close(&n.r);
		mmdb_close(&o.r);
		return 1;
	}
	arena_init(&o.a);
	arena_init(&n.a);
	net_iter_init(&o.it, &o.r);
	net_iter_init(&n.it, &n.r);
	bool summary = flag_get(c, "summary")->b, json = flag_get(c, "json")->b;
	bool by_subnet = flag_get(c, "subnets")->b, by_record = flag_get(c, "records")->b;
	if (by_subnet || by_record) {
		int rc = lookup_diff(&o, &n, by_subnet, by_record, json, summary);
		net_iter_free(&o.it);
		net_iter_free(&n.it);
		arena_free(&o.a);
		arena_free(&n.a);
		mmdb_close(&o.r);
		mmdb_close(&n.r);
		return rc;
	}

	section added = {0}, removed = {0}, modified = {0};
	FILE *lists[3] = {NULL, NULL, NULL};
	if (!summary) {
		for (int i = 0; i < 3; i++) {
			lists[i] = temp_stream();
			if (!lists[i]) {
				fprintf(stderr, "Error: tmpfile: %s\n", go_strerror(errno));
				return 1;
			}
		}
	}
	added.f = lists[0];
	removed.f = lists[1];
	modified.f = lists[2];

	bool old_ok = advance(&o) == 0, new_ok = advance(&n) == 0;
	buf_t ja = {0}, jb = {0}, jm = {0};
	char net[80];
	while (old_ok && new_ok && (o.has || n.has)) {
		int which; /* -1 only old, 1 only new, 0 both */
		if (!n.has)
			which = -1;
		else if (!o.has)
			which = 1;
		else if (o.ip != n.ip)
			which = o.ip < n.ip ? -1 : 1;
		else
			which = o.bit == n.bit ? 0 : (o.bit < n.bit ? -1 : 1);
		if (which == 0) {
			dnode *a = decode(&o), *b = decode(&n);
			if (!a || !b)
				break;
			if (!go_deep_equal(a, b)) {
				fmt_net(o.ip, o.bit, net);
				if (lists[2]) {
					if (json) {
						jm.len = 0;
						ja.len = 0;
						jb.len = 0;
						jw_value(&ja, b, "", 3);
						jw_value(&jb, a, "", 3);
						buf_printf(&jm, "{\n      \"new\": %.*s,\n      \"old\": %.*s\n    }", (int)ja.len,
							   (const char *)ja.p, (int)jb.len, (const char *)jb.p);
						section_add(&modified, net, &jm);
					} else {
						fprintf(lists[2], "   %s\n", net);
						modified.n++;
					}
				} else {
					modified.n++;
				}
			}
			old_ok = advance(&o) == 0;
			new_ok = advance(&n) == 0;
			continue;
		}
		side *s = which < 0 ? &o : &n;
		section *sec = which < 0 ? &removed : &added;
		fmt_net(s->ip, s->bit, net);
		if (sec->f) {
			if (json) {
				dnode *v = decode(s);
				if (!v)
					break;
				ja.len = 0;
				jw_value(&ja, v, "", 2);
				section_add(sec, net, &ja);
			} else {
				fprintf(sec->f, "   %s\n", net);
				sec->n++;
			}
		} else {
			sec->n++;
		}
		if (which < 0)
			old_ok = advance(&o) == 0;
		else
			new_ok = advance(&n) == 0;
	}
	if (!old_ok || !new_ok)
		go_logf("warning: error reading %s db: %s", !old_ok ? "old" : "new", err_msg());

	if (summary) {
		printf("Added: %llu | Removed: %llu | Modified: %llu\n", (unsigned long long)added.n,
		       (unsigned long long)removed.n, (unsigned long long)modified.n);
	} else if (json) {
		fputs("{\n", stdout);
		section_copy(&added, "added", stdout, false);
		section_copy(&modified, "modified", stdout, false);
		section_copy(&removed, "removed", stdout, true);
		fputs("}\n", stdout);
	} else {
		printf("Added: %llu\nRemoved: %llu\nModified: %llu\n\n", (unsigned long long)added.n,
		       (unsigned long long)removed.n, (unsigned long long)modified.n);
		const char *names[3] = {"Added:", "Removed:", "Modified:"};
		section *secs[3] = {&added, &removed, &modified};
		for (int i = 0; i < 3; i++) {
			if (!secs[i]->n)
				continue;
			printf("%s\n", names[i]);
			fflush(stdout);
			rewind(secs[i]->f);
			char chunk[65536];
			size_t got;
			while ((got = fread(chunk, 1, sizeof chunk, secs[i]->f)) > 0)
				fwrite(chunk, 1, got, stdout);
		}
	}
	for (int i = 0; i < 3; i++)
		if (lists[i])
			fclose(lists[i]);
	buf_free(&ja);
	buf_free(&jb);
	buf_free(&jm);
	net_iter_free(&o.it);
	net_iter_free(&n.it);
	arena_free(&o.a);
	arena_free(&n.a);
	mmdb_close(&o.r);
	mmdb_close(&n.r);
	return 0;
}
