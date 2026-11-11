/* Input bytes from a file or stdin ("-"), decompressed on the fly when they
 * start with the gzip magic bytes, whatever the file is called. Concatenated
 * gzip members are read one after another, as gzip -d does. */
#ifndef INPUT_H
#define INPUT_H

#include <sys/types.h>

#include "common.h"

typedef struct instream instream;

instream *in_open(const char *path);
/* csv_read_fn: up to cap bytes, 0 at the end, -1 with err_set. */
ssize_t in_read(void *ctx, uint8_t *buf, size_t cap);
bool in_is_gzip(const instream *s);
/* Compressed (on-disk) bytes consumed so far, for progress. */
uint64_t in_raw_bytes(const instream *s);
void in_close(instream *s);

#endif
