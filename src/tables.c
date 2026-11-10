#include "tables.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "compat.h"
#include "input.h"
#include "mmdb_enc.h"

static char *dupn(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	if (n)
		memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static bool tsv_name(const char *s)
{
	size_t n = strlen(s);
	return (n >= 4 && strcasecmp(s + n - 4, ".tsv") == 0) || (n >= 7 && strcasecmp(s + n - 7, ".tsv.gz") == 0);
}

int ktable_open(ktable *t, const char *label, const char *path, const char *key_col, const char *tmp_dir,
		bool lazy_quotes)
{
	memset(t, 0, sizeof *t);
	t->label = dupn(label, strlen(label));
	t->path = dupn(path, strlen(path));
	instream *in = in_open(path);
	if (!in) {
		err_prefix("%s", label);
		return -1;
	}
	const char *read_path = path;
	if (in_is_gzip(in) || access(path, F_OK) != 0) {
		/* read back at random, so unpacked first (also data.csv found as data.csv.gz) */
		buf_t p = {0};
		static unsigned seq;
		buf_printf(&p, "%s/mmdbio-lookup-%ld-%u.csv", tmp_dir && *tmp_dir ? tmp_dir : temp_dir(), (long)getpid(), seq++);
		FILE *f = fopen(buf_cstr(&p), "wb");
		if (!f) {
			err_set("create %s: %s (set --tmp-dir)", buf_cstr(&p), go_strerror(errno));
			buf_free(&p);
			in_close(in);
			return -1;
		}
		t->tmp = dupn(buf_cstr(&p), p.len);
		buf_free(&p);
		uint8_t chunk[1 << 16];
		ssize_t n;
		while ((n = in_read(in, chunk, sizeof chunk)) > 0)
			fwrite(chunk, 1, (size_t)n, f);
		bool failed = n < 0 || ferror(f);
		if (fclose(f) != 0 || failed) {
			if (n >= 0)
				err_set("write %s: %s", t->tmp, go_strerror(errno));
			in_close(in);
			return -1;
		}
		read_path = t->tmp;
	}
	in_close(in);
	char delim = tsv_name(path) ? '\t' : 0;
	if (!delim) {
		FILE *f = fopen(read_path, "rb");
		uint8_t head[1 << 16];
		size_t n = f ? fread(head, 1, sizeof head, f) : 0;
		if (f)
			fclose(f);
		delim = csv_detect_delimiter(head, n);
	}
	csv_reader r;
	if (csv_open(&r, read_path) < 0) {
		err_prefix("%s", label);
		return -1;
	}
	r.comma = (uint8_t)delim;
	r.lazy_quotes = lazy_quotes;
	bool eof;
	int rc = -1;
	if (csv_read(&r, &eof) < 0 || eof) {
		if (eof)
			err_set("the %s %s is empty", label, path);
		else
			err_prefix("%s %s", label, path);
		goto done;
	}
	t->ncols = (int)r.nfields;
	t->cols = xcalloc((size_t)t->ncols + 1, sizeof *t->cols);
	t->lens = xcalloc((size_t)t->ncols + 1, sizeof *t->lens);
	for (int i = 0; i < t->ncols; i++) {
		size_t n;
		const uint8_t *f = csv_field(&r, (size_t)i, &n);
		t->cols[i] = dupn((const char *)f, n);
		t->lens[i] = n;
	}
	if (key_col) {
		t->key = csv_find_column(&r, key_col);
		if (t->key < 0) {
			err_set("the %s %s has no column \"%s\"", label, path, key_col);
			goto done;
		}
	}
	ht16_init(&t->ix);
	t->ix_init = true;
	for (;;) {
		if (csv_read(&r, &eof) < 0) {
			err_prefix("%s %s", label, path);
			goto done;
		}
		if (eof)
			break;
		t->rows++;
		size_t n;
		const uint8_t *id = csv_field(&r, (size_t)t->key, &n);
		uint8_t h[16];
		siphash128(id, n, h);
		uint32_t ix;
		if (ht16_get_or_put(&t->ix, h, (uint32_t)t->n, &ix)) {
			t->offs[ix] = r.rec_off;
			continue;
		}
		if (t->n == t->cap) {
			t->cap = t->cap ? t->cap * 2 : 4096;
			t->offs = xrealloc(t->offs, t->cap * sizeof *t->offs);
		}
		t->offs[t->n++] = r.rec_off;
	}
	/* a row is read where it starts, a short read at a time */
	if (csv_open_at(&t->rd, read_path, 1 << 10) < 0) {
		err_prefix("%s", label);
		goto done;
	}
	t->rd_open = true;
	t->rd.comma = (uint8_t)delim;
	t->rd.lazy_quotes = lazy_quotes;
	rc = 0;
done:
	csv_close(&r);
	return rc;
}

int ktable_column(const ktable *t, const char *name, size_t n)
{
	int found = -1;
	for (int i = 0; i < t->ncols; i++)
		if (t->lens[i] == n && memcmp(t->cols[i], name, n) == 0)
			found = i;
	return found;
}

int ktable_fetch(ktable *t, const uint8_t *id, size_t n)
{
	uint8_t h[16];
	siphash128(id, n, h);
	uint32_t ix;
	if (!ht16_get(&t->ix, h, &ix))
		return 0;
	csv_seek(&t->rd, t->offs[ix]);
	bool eof;
	if (csv_read(&t->rd, &eof) < 0 || eof) {
		if (eof)
			err_set("%s %s: the row of id \"%.*s\" is gone", t->label, t->path, (int)(n < 100 ? n : 100), id);
		else
			err_prefix("%s %s", t->label, t->path);
		return -1;
	}
	return 1;
}

void ktable_close(ktable *t)
{
	if (t->rd_open)
		csv_close(&t->rd);
	if (t->ix_init)
		ht16_free(&t->ix);
	free(t->offs);
	for (int i = 0; i < t->ncols; i++)
		free(t->cols[i]);
	free(t->cols);
	free(t->lens);
	if (t->tmp) {
		unlink(t->tmp);
		free(t->tmp);
	}
	free(t->label);
	free(t->path);
	memset(t, 0, sizeof *t);
}
