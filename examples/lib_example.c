/* Looks addresses up with libmmdbio:
 *
 *   make lib
 *   cc -Iinclude examples/lib_example.c build/libmmdbio.a -lm -o lookup
 *   ./lookup city.mmdb 8.8.8.8 1.1.1.1
 */
#include <stdio.h>
#include <stdlib.h>

#include "mmdbio.h"

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s DB.mmdb IP...\n", argv[0]);
		return 2;
	}
	mmdbio_db *db;
	if (mmdbio_open(argv[1], &db) < 0) {
		fprintf(stderr, "%s\n", mmdbio_error());
		return 1;
	}
	for (int i = 2; i < argc; i++) {
		int found;
		char *json, *network;
		if (mmdbio_lookup(db, argv[i], &found, &json, &network) < 0) {
			fprintf(stderr, "%s: %s\n", argv[i], mmdbio_error());
			continue;
		}
		if (found)
			printf("%s %s %s\n", argv[i], network, json);
		else
			printf("%s (no data)\n", argv[i]);
		free(json);
		free(network);
	}
	mmdbio_close(db);
	return 0;
}
