/* libmmdbio: the public API (include/mmdbio.h) over the tool's own code. */
#include "../include/mmdbio.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build.h"
#include "cmds.h"
#include "compat.h"
#include "netaddr.h"
#include "shrink.h"

struct mmdbio_db {
	mmdb_reader r;
	arena a;
};

const char *mmdbio_version(void) { return MMDBIO_VERSION; }

const char *mmdbio_error(void) { return err_msg(); }

int mmdbio_open(const char *path, mmdbio_db **db)
{
	mmdbio_db *d = xcalloc(1, sizeof *d);
	if (mmdb_open(&d->r, path) < 0) {
		mmdb_close(&d->r);
		free(d);
		*db = NULL;
		return -1;
	}
	arena_init(&d->a);
	*db = d;
	return 0;
}

void mmdbio_close(mmdbio_db *db)
{
	if (!db)
		return;
	arena_free(&db->a);
	mmdb_close(&db->r);
	free(db);
}

int mmdbio_get_metadata(const mmdbio_db *db, mmdbio_metadata *out)
{
	const mmdb_reader *r = &db->r;
	static char type[1024];
	snprintf(type, sizeof type, "%.*s", (int)r->database_type_len, (const char *)r->database_type);
	*out = (mmdbio_metadata){.database_type = type,
				 .ip_version = (int)r->ip_version,
				 .record_size = (int)r->record_size,
				 .node_count = r->node_count,
				 .build_epoch = r->build_epoch,
				 .file_size = r->size,
				 .tree_size = r->tree_size,
				 .data_size = r->data.len};
	return 0;
}

static char *dup_buf(const buf_t *b)
{
	char *s = xmalloc(b->len + 1);
	if (b->len)
		memcpy(s, b->p, b->len);
	s[b->len] = 0;
	return s;
}

int mmdbio_lookup(const mmdbio_db *db, const char *ip, int *found, char **json, char **network)
{
	mmdbio_db *d = (mmdbio_db *)db;
	*found = 0;
	if (json)
		*json = NULL;
	if (network)
		*network = NULL;
	u128 a;
	bool is4;
	if (!parse_ip_text(ip, strlen(ip), &a, &is4)) {
		err_set("not an IP address: %s", ip);
		return -1;
	}
	uint64_t off;
	bool f;
	int bits;
	if (mmdb_lookup_net(&d->r, a, is4, &off, &f, &bits) < 0)
		return -1;
	if (!f)
		return 0;
	*found = 1;
	if (network) {
		u128 m = bits <= 0 ? 0 : bits >= 128 ? a : a & ~(U128_MAX >> bits);
		char n[80];
		fmt_net(m, bits, n);
		*network = strdup(n);
	}
	if (json) {
		dnode *v;
		uint64_t next;
		arena_reset(&d->a);
		if (mdec_value(&d->r.data, off, &d->a, &v, &next) < 0)
			return -1;
		buf_t out = {0};
		dnode_json(v, &out);
		*json = dup_buf(&out);
		buf_free(&out);
	}
	return 0;
}

int mmdbio_networks(const mmdbio_db *db, mmdbio_network_fn fn, void *ctx)
{
	mmdbio_db *d = (mmdbio_db *)db;
	net_iter it;
	net_iter_init(&it, &d->r);
	u128 ip;
	int bit, rc;
	uint64_t off;
	buf_t js = {0};
	int stop = 0;
	while (!stop && (rc = net_iter_next(&it, &ip, &bit, &off)) == 1) {
		dnode *v;
		uint64_t next;
		arena_reset(&d->a);
		if (mdec_value(&d->r.data, off, &d->a, &v, &next) < 0) {
			rc = -1;
			break;
		}
		js.len = 0;
		dnode_json(v, &js);
		char n[80];
		fmt_net(ip, bit, n);
		stop = fn(ctx, n, buf_cstr(&js));
	}
	net_iter_free(&it);
	buf_free(&js);
	if (rc < 0)
		return -1;
	return stop;
}

int mmdbio_verify(const char *path)
{
	mmdb_reader r;
	int rc = mmdb_open(&r, path) < 0 || mmdb_verify(&r) < 0 ? -1 : 0;
	mmdb_close(&r);
	return rc;
}

int mmdbio_shrink(const char *in, const char *out, const mmdbio_shrink_options *opts, mmdbio_shrink_result *res)
{
	shrink_opts so = {.record_size = opts ? opts->record_size : 0, .no_compact = opts && opts->no_compact};
	shrink_result r;
	int rc = mmdb_shrink(in, out, &so, &r);
	*res = (mmdbio_shrink_result){.in_bytes = r.in_bytes,
				      .out_bytes = r.out_bytes,
				      .in_nodes = r.in_nodes,
				      .out_nodes = r.out_nodes,
				      .in_record_size = r.in_record_size,
				      .out_record_size = r.out_record_size,
				      .min_record_size = r.min_record_size,
				      .in_tree = r.in_tree,
				      .out_tree = r.out_tree,
				      .in_data = r.in_data,
				      .out_data = r.out_data,
				      .seconds = r.seconds};
	return rc;
}

int mmdbio_check(const char *a, const char *b, const mmdbio_check_options *opts, mmdbio_check_result *res)
{
	check_opts co = {.seed = opts && opts->seed ? opts->seed : 1,
			 .lookups = opts && opts->lookups ? opts->lookups : 1000000,
			 .all_networks = opts && opts->all_networks};
	check_report rep;
	int rc = shrink_check(a, b, &co, &rep);
	memset(res, 0, sizeof *res);
	res->failed = rep.failed;
	snprintf(res->detail, sizeof res->detail, "%s", rep.detail);
	res->fixed = rep.fixed.n;
	res->random4 = rep.random4.n;
	res->random6 = rep.random6.n;
	res->networks = rep.networks.n;
	return rc;
}

int mmdbio_bench(const char *path, uint64_t seed, uint64_t lookups, mmdbio_bench_result *res)
{
	bench_result b;
	int rc = shrink_bench(path, seed, lookups, &b);
	res->ns_per_op = b.ns_per_op;
	res->qps = b.qps;
	return rc;
}

int mmdbio_bench_paired(const char *a, const char *b, uint64_t seed, uint64_t lookups, int rounds,
			mmdbio_paired_result *res)
{
	paired_result p;
	int rc = shrink_bench_paired(a, b, seed, lookups, rounds, &p);
	memset(res, 0, sizeof *res);
	res->rounds = p.rounds;
	for (int i = 0; i < 2; i++) {
		res->median[i] = p.median[i];
		res->min[i] = p.min[i];
		res->max[i] = p.max[i];
	}
	res->ratio = p.ratio;
	return rc;
}

int mmdbio_build(const char *config_path, int quiet)
{
	build_opts o;
	build_opts_init(&o);
	int rc = -1;
	char *final = NULL;
	buf_t built = {0};
	if (build_opts_load_config(&o, config_path) < 0)
		goto done;
	o.quiet = o.silent = quiet != 0;
	if (!o.input || !*o.input || !o.output || !*o.output || strcmp(o.output, "-") == 0) {
		err_set("%s: the config needs \"input\" and an \"output\" file", config_path);
		goto done;
	}
	if (!o.database_type || !*o.database_type) {
		const char *base = strrchr(o.input, '/');
		opts_set_str(&o.database_type, base ? base + 1 : o.input);
	}
	final = strdup(o.output);
	if (!o.shrink) {
		rc = build_mmdb(&o);
		goto done;
	}
	/* shrunk and checked, as import --shrink does */
	buf_printf(&built, "%s.unshrunk-%ld", final, (long)getpid());
	opts_set_str(&o.output, buf_cstr(&built));
	o.label = final;
	if (build_mmdb(&o) < 0)
		goto done;
	shrink_opts so = {0};
	check_opts co = {.seed = 1, .lookups = 1000000};
	rc = shrink_and_check(buf_cstr(&built), final, &so, &co, true, false, false, true) == 0 ? 0 : -1;
	if (rc < 0 && !*err_msg())
		err_set("the shrunk file failed its check");
done:
	if (built.len)
		unlink(buf_cstr(&built));
	buf_free(&built);
	free(final);
	build_opts_free(&o);
	return rc;
}
