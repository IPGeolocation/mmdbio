/* Keyed CSV tables: --lookup, and the tables of a record template. A table is
 * indexed by one column (an id's last row wins); only where each id's row
 * starts is kept in memory, and the row is read again when it is asked for.
 * A gzipped table is unpacked into the temporary directory first. */
#ifndef TABLES_H
#define TABLES_H

#include "common.h"
#include "csv.h"
#include "hashtab.h"

typedef struct {
	char *label;   /* "lookup", or "table \"place\"", for messages */
	char *path;
	char **cols;   /* the header */
	size_t *lens;
	int ncols;
	int key;       /* the id column */
	ht16 ix;       /* hash of an id -> index into offs */
	int64_t *offs; /* where each id's last row starts */
	size_t n, cap;
	uint64_t rows;
	csv_reader rd; /* the row of the last fetch */
	bool rd_open, ix_init;
	char *tmp;     /* the unpacked copy of a gzipped table */
} ktable;

/* Opens and indexes path by the column key_col (NULL: the first column). */
int ktable_open(ktable *t, const char *label, const char *path, const char *key_col, const char *tmp_dir,
		bool lazy_quotes);
/* The index of the column called name, or -1. */
int ktable_column(const ktable *t, const char *name, size_t n);
/* 1 with the row of id in t->rd, 0 when the table has no such id, -1 on error. */
int ktable_fetch(ktable *t, const uint8_t *id, size_t n);
void ktable_close(ktable *t);

#endif
