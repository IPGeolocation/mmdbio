/* mmdbio: read, query, convert, compare, validate, shrink and build MMDB
 * files. */
#include <stdlib.h>
#include <string.h>

#include "cmds.h"
#include "color.h"
#include "compat.h"

static const char ROOT_LONG[] =
	"mmdbio reads and writes MMDB files: look up IPs, export records to JSON, CSV, TSV or CIDR lists,\n"
	"compare two files, check one, inspect its schema, shrink it losslessly, and build new ones from\n"
	"CSV, TSV, JSON or NDJSON (gzipped or not).";

static void root_help(FILE *out)
{
	fprintf(out, "%s\n\nUsage:\n  mmdbio [command]\n\nAvailable Commands:\n", ROOT_LONG);
	fprintf(out, "  %-11s %s\n", "completion", cmd_completion.short_desc);
	for (int i = 0; i < ncommands; i++) {
		if (all_commands[i] == &cmd_completion)
			continue;
		if (strcmp(all_commands[i]->name, "help") > 0 && strcmp(all_commands[i - 1]->name, "help") < 0)
			fprintf(out, "  %-11s %s\n", "help", "Help about any command");
		fprintf(out, "  %-11s %s\n", all_commands[i]->name, all_commands[i]->short_desc);
	}
	fprintf(out, "\nFlags:\n  -h, --help      help for mmdbio\n      --nocolor   no coloured output (also NO_COLOR=1)\n"
		     "  -v, --version   version for mmdbio\n\n"
		     "Use \"mmdbio [command] --help\" for more information about a command.\n");
}

static command *find(const char *name)
{
	for (int i = 0; i < ncommands; i++)
		if (strcmp(all_commands[i]->name, name) == 0)
			return all_commands[i];
	return NULL;
}

static int run(command *c, int argc, char **argv)
{
	int nargs;
	int rc = flags_parse(c, argc, argv, &nargs);
	if (rc == 1) {
		command_help(c, stdout);
		return 0;
	}
	if (rc < 0)
		return 1;
	return c->run(c, nargs, argv);
}

int main(int argc, char **argv)
{
	compat_init(argc, argv);
	/* --nocolor is taken anywhere before a "--" */
	int k = 1;
	bool rest = false;
	for (int i = 1; i < argc; i++) {
		if (!rest && strcmp(argv[i], "--nocolor") == 0) {
			color_disable();
			continue;
		}
		if (strcmp(argv[i], "--") == 0)
			rest = true;
		argv[k++] = argv[i];
	}
	argc = k;
	argv[argc] = NULL;

	if (argc >= 3 && strcmp(argv[1], "__complete") == 0)
		return completion_answer(argc - 2, argv + 2);
	if (argc == 5 && strcmp(argv[1], "__memprobe") == 0)
		return shrink_memory_probe(argv[2], strtoull(argv[3], NULL, 10), strtoull(argv[4], NULL, 10));

	if (argc < 2) {
		root_help(stdout);
		return 0;
	}
	const char *name = argv[1];
	if (strcmp(name, "-h") == 0 || strcmp(name, "--help") == 0) {
		root_help(stdout);
		return 0;
	}
	if (strcmp(name, "-v") == 0 || strcmp(name, "--version") == 0) {
		printf("mmdbio version %s\n", MMDBIO_VERSION);
		return 0;
	}
	if (strcmp(name, "help") == 0) {
		if (argc < 3) {
			root_help(stdout);
			return 0;
		}
		command *c = find(argv[2]);
		if (!c) {
			fprintf(stderr, "Unknown help topic [`%s`]\n", argv[2]);
			root_help(stderr);
			return 1;
		}
		command_help(c, stdout);
		return 0;
	}
	command *c = find(name);
	if (!c) {
		fprintf(stderr, "Error: unknown command \"%s\" for \"mmdbio\"\nRun 'mmdbio --help' for usage.\n", name);
		return 1;
	}
	return run(c, argc - 2, argv + 2);
}
