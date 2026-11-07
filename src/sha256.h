/* SHA-256, with the x86 SHA extensions when the CPU has them. The fast path
 * is checked against the portable code on known vectors before first use and
 * is skipped if they disagree. */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
/* The first 16 bytes: the record and value keys use 128 bits, like Go's
 * hashKey. */
void sha256_16(const uint8_t *data, size_t len, uint8_t out[16]);

/* For tests: force the portable path (true) or restore auto selection. */
void sha256_force_portable(bool on);
bool sha256_using_shani(void);

#endif
