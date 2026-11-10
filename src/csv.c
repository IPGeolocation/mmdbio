#include "csv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#define READ_BUF (1u << 20)

static const char ERR_BARE_QUOTE[] = "bare \" in non-quoted-field";
static const char ERR_QUOTE[] = "extraneous or missing \" in quoted-field";

/* Go's syscall messages are lowercase versions of glibc's. */
static void set_errno_msg(const char *what, const char *path)
{
	char msg[256];
	snprintf(msg, sizeof msg, "%s", strerror(errno));
	if (msg[0] >= 'A' && msg[0] <= 'Z')
		msg[0] = (char)(msg[0] - 'A' + 'a');
	err_set("%s %s: %s", what, path, msg);
}

static void init(csv_reader *r, int fd, size_t cap)
{
	memset(r, 0, sizeof *r);
	r->fd = fd;
	r->rcap = cap;
	r->rbuf = xmalloc(cap);
	r->comma = ',';
	buf_grow(&r->rec, 1);
	buf_grow(&r->line, 1);
}

void csv_init_fd(csv_reader *r, int fd) { init(r, fd, READ_BUF); }

void csv_init_reader(csv_reader *r, csv_read_fn fn, void *ctx)
{
	init(r, -1, READ_BUF);
	r->readfn = fn;
	r->readctx = ctx;
}

int csv_open(csv_reader *r, const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		set_errno_msg("open", path);
		return -1;
	}
	csv_init_fd(r, fd);
	return 0;
}

int csv_open_at(csv_reader *r, const char *path, size_t chunk)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		set_errno_msg("open", path);
		return -1;
	}
	init(r, fd, chunk);
	r->use_pread = true;
	return 0;
}

void csv_seek(csv_reader *r, int64_t off)
{
	r->base = off;
	r->rlen = r->rpos = 0;
	r->fd_eof = false;
	r->num_line = 0;
}

char csv_detect_delimiter(const uint8_t *p, size_t n)
{
	static const char cands[] = {',', '\t', ';', '|'};
	size_t count[4] = {0};
	bool q = false;
	for (size_t i = 0; i < n && (q || p[i] != '\n'); i++) {
		if (p[i] == '"')
			q = !q;
		else if (!q)
			for (int k = 0; k < 4; k++)
				if (p[i] == (uint8_t)cands[k])
					count[k]++;
	}
	int best = 0;
	for (int k = 1; k < 4; k++)
		if (count[k] > count[best])
			best = k;
	return cands[best];
}

int csv_find_column(const csv_reader *r, const char *name)
{
	int found = -1;
	size_t want = strlen(name);
	for (size_t i = 0; i < r->nfields; i++) {
		size_t n;
		const uint8_t *f = csv_field(r, i, &n);
		if (n == want && memcmp(f, name, n) == 0)
			found = (int)i;
	}
	return found;
}

void csv_close(csv_reader *r)
{
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
	free(r->rbuf);
	r->rbuf = NULL;
	buf_free(&r->line);
	buf_free(&r->rec);
	free(r->ends);
	r->ends = NULL;
}

/* readLine: the next line including its \n. 1 means EOF with nothing read. */
static int read_line(csv_reader *r)
{
	r->line.len = 0;
	r->line_off = r->base + (int64_t)r->rpos;
	bool got_nl = false;
	for (;;) {
		if (r->rpos == r->rlen) {
			if (r->fd_eof)
				break;
			int64_t at = r->base + (int64_t)r->rlen;
			ssize_t n;
			if (r->readfn)
				n = r->readfn(r->readctx, r->rbuf, r->rcap);
			else if (r->use_pread)
				n = pread(r->fd, r->rbuf, r->rcap, (off_t)at);
			else
				n = read(r->fd, r->rbuf, r->rcap);
			if (n < 0 && r->readfn)
				return -1; /* the reader set the message */
			if (n < 0) {
				if (errno == EINTR)
					continue;
				set_errno_msg("read", "input");
				return -1;
			}
			r->base = at;
			r->rlen = (size_t)n;
			r->rpos = 0;
			if (n == 0) {
				r->fd_eof = true;
				break;
			}
		}
		const uint8_t *start = r->rbuf + r->rpos;
		const uint8_t *nl = memchr(start, '\n', r->rlen - r->rpos);
		size_t take = nl ? (size_t)(nl - start) + 1 : r->rlen - r->rpos;
		buf_append(&r->line, start, take);
		r->rpos += take;
		if (nl) {
			got_nl = true;
			break;
		}
	}
	if (r->num_line == 0 && r->line.len >= 3 && memcmp(r->line.p, "\xEF\xBB\xBF", 3) == 0) {
		memmove(r->line.p, r->line.p + 3, r->line.len - 3); /* a UTF-8 byte order mark */
		r->line.len -= 3;
	}
	int status = 0;
	if (!got_nl) {
		if (r->line.len == 0) {
			status = 1;
		} else if (r->line.p[r->line.len - 1] == '\r') {
			r->line.len--; /* trailing \r before EOF is dropped */
		}
	}
	r->num_line++;
	size_t n = r->line.len;
	if (n >= 2 && r->line.p[n - 2] == '\r' && r->line.p[n - 1] == '\n') {
		r->line.p[n - 2] = '\n';
		r->line.len = n - 1;
	}
	return status;
}

static size_t length_nl(const uint8_t *b, size_t n) { return n > 0 && b[n - 1] == '\n' ? 1 : 0; }

static void push_end(csv_reader *r)
{
	if (r->nfields == r->ends_cap) {
		r->ends_cap = r->ends_cap ? r->ends_cap * 2 : 64;
		r->ends = xrealloc(r->ends, r->ends_cap * sizeof *r->ends);
	}
	r->ends[r->nfields++] = r->rec.len;
}

static int parse_error(long start_line, long line, long col, const char *what)
{
	if (start_line != line)
		err_set("record on line %ld; parse error on line %ld, column %ld: %s", start_line, line, col, what);
	else
		err_set("parse error on line %ld, column %ld: %s", line, col, what);
	return -1;
}

int csv_read(csv_reader *r, bool *eof)
{
	*eof = false;
	r->count_error = false;
	for (;;) {
		int st = read_line(r);
		if (st < 0)
			return -1;
		if (st == 1) {
			*eof = true;
			return 0;
		}
		if (r->line.len == length_nl(r->line.p, r->line.len))
			continue; /* blank line */
		break;
	}

	r->rec.len = 0;
	r->nfields = 0;
	long rec_line = r->num_line;
	int64_t rec_off = r->line_off;
	long pos_line = r->num_line, pos_col = 1;
	const uint8_t *l = r->line.p;
	size_t ll = r->line.len;

	for (;;) {
		if (ll == 0 || l[0] != '"') {
			const uint8_t *comma = memchr(l, r->comma, ll);
			size_t flen = comma ? (size_t)(comma - l) : ll - length_nl(l, ll);
			const uint8_t *q = r->lazy_quotes ? NULL : memchr(l, '"', flen);
			if (q)
				return parse_error(rec_line, r->num_line, pos_col + (long)(q - l), ERR_BARE_QUOTE);
			buf_append(&r->rec, l, flen);
			push_end(r);
			if (comma) {
				l = comma + 1;
				ll -= flen + 1;
				pos_col += (long)flen + 1;
				continue;
			}
			break;
		}
		/* quoted field */
		l++;
		ll--;
		pos_col++;
		for (;;) {
			const uint8_t *q = memchr(l, '"', ll);
			if (q) {
				size_t i = (size_t)(q - l);
				buf_append(&r->rec, l, i);
				l = q + 1;
				ll -= i + 1;
				pos_col += (long)i + 1;
				if (ll > 0 && l[0] == '"') {
					buf_push(&r->rec, '"');
					l++;
					ll--;
					pos_col++;
				} else if (ll > 0 && l[0] == r->comma) {
					l++;
					ll--;
					pos_col++;
					push_end(r);
					goto next_field;
				} else if (length_nl(l, ll) == ll) {
					push_end(r);
					goto done;
				} else if (r->lazy_quotes) {
					buf_push(&r->rec, '"'); /* a bare quote inside the field */
				} else {
					return parse_error(rec_line, r->num_line, pos_col - 1, ERR_QUOTE);
				}
			} else if (ll > 0) {
				buf_append(&r->rec, l, ll);
				pos_col += (long)ll;
				int st = read_line(r);
				if (st < 0)
					return -1;
				l = r->line.p;
				ll = st == 1 ? 0 : r->line.len;
				if (ll > 0) {
					pos_line++;
					pos_col = 1;
				}
			} else if (r->lazy_quotes) {
				push_end(r); /* the input ended inside the quotes */
				goto done;
			} else {
				return parse_error(rec_line, pos_line, pos_col, ERR_QUOTE);
			}
		}
	next_field:;
	}
done:
	r->rec_line = rec_line;
	r->rec_off = rec_off;
	if (r->fields_per_record > 0) {
		if ((int)r->nfields != r->fields_per_record) {
			err_set("record on line %ld: wrong number of fields", rec_line);
			r->count_error = true;
			return -1;
		}
	} else if (r->fields_per_record == 0) {
		r->fields_per_record = (int)r->nfields;
	}
	return 0;
}
