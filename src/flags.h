/* Command-line flags with the rules of spf13/pflag, which the Go mmdbio
 * used: --name value, --name=value, -s value, -svalue, --bool, --bool=false,
 * comma-separated lists that also repeat, and -- to end the flags. */
#ifndef FLAGS_H
#define FLAGS_H

#include "common.h"

typedef enum { F_STRING, F_BOOL, F_INT, F_LIST, F_ARRAY } flag_type;

typedef struct {
	char **v;
	size_t n, cap;
} strlist;

void strlist_push(strlist *l, const char *s);
void strlist_free(strlist *l);

typedef struct {
	const char *name;
	char shorthand;      /* 0 for none */
	flag_type type;
	const char *usage;
	const char *def;     /* the default: parsed into the value, shown in help */
	/* the value, by type */
	const char *str;
	bool b;
	long long i;
	strlist list;        /* F_LIST (split on commas) and F_ARRAY (one per use) */
	bool set;            /* given on the command line */
} flag;

typedef struct command {
	const char *name;
	const char *use;      /* usage line after "mmdbio ", e.g. "read [flags]" */
	const char *short_desc;
	const char *long_desc; /* NULL: short_desc */
	const char *examples;  /* NULL for none */
	flag *flags;
	int nflags;
	int (*run)(struct command *c, int argc, char **argv);
} command;

/* Parses argv (after the command name) into c->flags; positional arguments
 * are moved to the front of argv and counted in *nargs. Returns 0, 1 when
 * -h/--help was given, or -1 after printing the error to stderr. */
int flags_parse(command *c, int argc, char **argv, int *nargs);
void flags_reset(command *c);
flag *flag_get(command *c, const char *name);
/* The help text of a command, as cobra prints it. */
void command_help(const command *c, FILE *out);

#endif
