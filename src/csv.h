/* A CSV reader with the rules of Go's encoding/csv (no comment character):
 * \r\n becomes \n everywhere, blank lines are skipped, quoted fields may span
 * lines, and every record must have the field count of the first one. The
 * delimiter (Comma) and LazyQuotes can be set. */
#ifndef CSV_H
#define CSV_H

#include <sys/types.h>

#include "common.h"

/* Reads up to cap bytes into buf: the count, 0 at the end, -1 on error. */
typedef ssize_t (*csv_read_fn)(void *ctx, uint8_t *buf, size_t cap);

typedef struct {
	int fd;
	csv_read_fn readfn; /* when set, input comes from readfn(readctx) */
	void *readctx;
	bool fd_eof;
	uint8_t *rbuf;
	size_t rlen, rpos, rcap;
	bool use_pread;   /* csv_open_at: pread from base, file position untouched */
	int64_t base;     /* file offset of rbuf[0] */

	uint8_t comma;    /* the delimiter; ',' unless set */
	bool lazy_quotes; /* Go's LazyQuotes: stray quotes are kept as text */
	bool count_error; /* the last csv_read failed only on the field count */

	buf_t line;       /* the current physical line */
	int64_t line_off; /* file offset where it starts */
	long num_line;    /* lines read so far, as Go counts them */
	buf_t rec;        /* unescaped fields, back to back */
	size_t *ends;     /* end of field i in rec */
	size_t nfields, ends_cap;
	long rec_line;    /* line where the last record started */
	int64_t rec_off;  /* file offset where the last record started */
	int fields_per_record;
} csv_reader;

/* Opens path; on failure sets "open <path>: <reason>"-style errors. */
int csv_open(csv_reader *r, const char *path);
/* Reads from an already open descriptor (used by tests). */
void csv_init_fd(csv_reader *r, int fd);
/* Reads through fn(ctx), which csv_close does not close. */
void csv_init_reader(csv_reader *r, csv_read_fn fn, void *ctx);
/* Opens path for re-reading single records at known offsets: csv_seek, then
 * csv_read, with pread in chunk-sized reads. */
int csv_open_at(csv_reader *r, const char *path, size_t chunk);
/* Moves a csv_open_at reader to file offset off; lines count from 1 again. */
void csv_seek(csv_reader *r, int64_t off);
void csv_close(csv_reader *r);

/* Reads the next record. Returns 0 with *eof set at the end of input, 0 with
 * the record in r->rec/ends, or -1 with a Go-style ParseError message. After
 * a -1 with count_error set the record is complete and reading can go on. */
int csv_read(csv_reader *r, bool *eof);

static inline const uint8_t *csv_field(const csv_reader *r, size_t i, size_t *len)
{
	size_t start = i ? r->ends[i - 1] : 0;
	*len = r->ends[i] - start;
	return r->rec.p + start;
}

/* The index of the field called name in the current record (a header), or
 * -1. With duplicate names the last one wins, as in Go's index map. */
int csv_find_column(const csv_reader *r, const char *name);

/* The delimiter of the first line of p: the most frequent of comma, tab,
 * semicolon and pipe outside quotes (comma when none appears). */
char csv_detect_delimiter(const uint8_t *p, size_t n);

#endif
