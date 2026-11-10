/* The mmdbio commands and the helpers they share. */
#ifndef CMDS_H
#define CMDS_H

#include "flags.h"
#include "mmdb_read.h"
#include "shrink.h"

extern command cmd_read, cmd_metadata, cmd_stats, cmd_inspect, cmd_verify, cmd_export, cmd_diff, cmd_import,
	cmd_completion, cmd_shrink;
extern command *const all_commands[];
extern const int ncommands;

#define MMDBIO_VERSION "2.0.0"

/* "Error: <msg>" on stdout, the command's help, exit 1: the Go commands'
 * missing-flag path (fmt.Println, cmd.Help, os.Exit(1)). */
_Noreturn void missing_flag(command *c, const char *msg);

/* extractField: follows a dotted path through maps; NULL when a step is
 * missing or not a map. */
const dnode *extract_field(const dnode *v, const char *path);
/* fmt.Sprint of a decoded value, as maxminddb-golang hands it to Go. */
void go_sprint(const dnode *v, buf_t *out);
/* Go's reflect type name of a decoded value: string, uint64, int, float64,
 * float32, bool, *big.Int, []uint8. */
const char *go_type_name(const dnode *v);
/* reflect.DeepEqual of two decoded values after maxminddb-golang's decoding
 * into interface{} (all unsigned types but uint128 become uint64). */
bool go_deep_equal(const dnode *a, const dnode *b);

/* mmdbio __complete N WORD...: the shell completion scripts' question. */
int completion_answer(int argc, char **argv);

/* foo.mmdb -> foo.shrunk.mmdb */
void shrink_default_output(const char *in, buf_t *out);
/* Shrinks in to a temporary file next to out (out NULL: a dry run), checks
 * it against in, then gives it out's name; the report goes to stdout (to
 * stderr, shortened, when quiet). Returns the exit code: 0, 1, or a
 * CHECK_ code. */
int shrink_and_check(const char *in, const char *out, const shrink_opts *so, const check_opts *co, bool check,
		     bool bench, bool full_report, bool quiet);

/* Writes all of b to path (0644), creating or truncating it. */
int write_file(const char *path, const uint8_t *p, size_t n);

#endif
