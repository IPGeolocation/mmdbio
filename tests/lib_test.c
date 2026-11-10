/* libmmdbio through its public header only: build, read, walk, verify,
 * shrink, check, time. lib-test WORKDIR; exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmdbio.h"

static int passed, failed;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		if (cond) {                                           \
			passed++;                                     \
		} else {                                              \
			failed++;                                     \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                 \
			fprintf(stderr, " (%s)\n", mmdbio_error());   \
		}                                                     \
	} while (0)

static int count_networks(void *ctx, const char *network, const char *json)
{
	int *n = ctx;
	if (strcmp(network, "8.8.8.0/24") == 0 && !strstr(json, "Mountain View"))
		return -5;
	(*n)++;
	return 0;
}

static int stop_early(void *ctx, const char *network, const char *json)
{
	(void)ctx;
	(void)network;
	(void)json;
	return 7;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: lib-test WORKDIR\n");
		return 2;
	}
	char csv[1024], cfg[1024], db[1024], small[1024];
	snprintf(csv, sizeof csv, "%s/lib.csv", argv[1]);
	snprintf(cfg, sizeof cfg, "%s/lib.json", argv[1]);
	snprintf(db, sizeof db, "%s/lib.mmdb", argv[1]);
	snprintf(small, sizeof small, "%s/lib.small.mmdb", argv[1]);
	FILE *f = fopen(csv, "w");
	fprintf(f, "network,city,score\n8.8.8.0/24,Mountain View,90\n1.1.1.0/24,Sydney,10\n2001:db8::/32,Test,5\n");
	for (int i = 0; i < 64; i++)
		fprintf(f, "10.%d.0.0/25,A,1\n10.%d.0.128/25,B,2\n", i, i);
	fclose(f);
	f = fopen(cfg, "w");
	fprintf(f, "{\"input\": \"lib.csv\", \"output\": \"lib.mmdb\", \"fields\": {\"types\": {\"score\": \"uint16\"}},"
		   " \"metadata\": {\"database_type\": \"Lib-Test\"}}");
	fclose(f);

	CHECK(strcmp(mmdbio_version(), "2.0.0") >= 0, "version %s", mmdbio_version());
	CHECK(mmdbio_build(cfg, 1) == 0, "build");
	mmdbio_db *d = NULL;
	CHECK(mmdbio_open(db, &d) == 0 && d, "open");
	if (!d)
		return 1;
	mmdbio_metadata m;
	CHECK(mmdbio_get_metadata(d, &m) == 0 && strcmp(m.database_type, "Lib-Test") == 0 && m.ip_version == 6 &&
		      m.node_count > 0 && m.data_size > 0,
	      "metadata %s %d", m.database_type, m.ip_version);
	int found;
	char *json, *net;
	CHECK(mmdbio_lookup(d, "8.8.8.8", &found, &json, &net) == 0 && found &&
		      strcmp(json, "{\"city\":\"Mountain View\",\"score\":90}") == 0 && strcmp(net, "8.8.8.0/24") == 0,
	      "lookup 8.8.8.8: %s %s", json ? json : "-", net ? net : "-");
	free(json);
	free(net);
	CHECK(mmdbio_lookup(d, "2001:db8::1", &found, &json, &net) == 0 && found && strcmp(net, "2001:db8::/32") == 0,
	      "lookup IPv6: %s", net ? net : "-");
	free(json);
	free(net);
	CHECK(mmdbio_lookup(d, "9.9.9.9", &found, &json, NULL) == 0 && !found && !json, "no data");
	CHECK(mmdbio_lookup(d, "nope", &found, NULL, NULL) == -1 && strstr(mmdbio_error(), "nope"), "a bad IP");
	int n = 0;
	CHECK(mmdbio_networks(d, count_networks, &n) == 0 && n == 131, "networks: %d", n);
	CHECK(mmdbio_networks(d, stop_early, NULL) == 7, "the callback stops the walk");
	mmdbio_close(d);
	CHECK(mmdbio_verify(db) == 0, "verify");
	CHECK(mmdbio_verify(csv) == -1 && *mmdbio_error(), "verify a CSV");
	CHECK(mmdbio_open("no/such.mmdb", &d) == -1 && d == NULL, "open a missing file");

	mmdbio_shrink_result r;
	CHECK(mmdbio_shrink(db, small, NULL, &r) == 0 && r.out_nodes < r.in_nodes && r.out_bytes < r.in_bytes,
	      "shrink %llu -> %llu nodes", (unsigned long long)r.in_nodes, (unsigned long long)r.out_nodes);
	mmdbio_check_options co = {.lookups = 20000, .all_networks = 1};
	mmdbio_check_result cr;
	CHECK(mmdbio_check(db, small, &co, &cr) == 0 && cr.failed == MMDBIO_CHECK_OK && cr.random4 == 20000 &&
		      cr.networks == 131,
	      "check: %d %s", cr.failed, cr.detail);
	CHECK(mmdbio_shrink(db, NULL, &(mmdbio_shrink_options){.record_size = 28}, &r) == 0 && r.out_record_size == 28,
	      "dry run at record size 28");
	mmdbio_bench_result b;
	CHECK(mmdbio_bench(small, 1, 10000, &b) == 0 && b.ns_per_op > 0 && b.qps > 0, "bench");
	mmdbio_paired_result p;
	CHECK(mmdbio_bench_paired(db, small, 1, 5000, 3, &p) == 0 && p.rounds == 3 && p.median[0] > 0 && p.ratio > 0,
	      "paired bench");

	printf("%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
