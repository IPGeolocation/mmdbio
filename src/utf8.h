/* UTF-8 rules copied from Go: utf8.DecodeRune, utf8.Valid, unicode.IsSpace
 * and strings.TrimSpace. */
#ifndef UTF8_H
#define UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define UTF8_RUNE_ERROR 0xFFFDu

/* Decodes one rune like utf8.DecodeRune: an invalid or truncated sequence
 * gives (RUNE_ERROR, 1); empty input gives (RUNE_ERROR, 0). */
int go_decode_rune(const uint8_t *s, size_t n, uint32_t *r);
/* utf8.DecodeLastRune. */
int go_decode_last_rune(const uint8_t *s, size_t n, uint32_t *r);
bool utf8_valid(const uint8_t *s, size_t n);
bool go_is_space(uint32_t r);

/* strings.TrimSpace, in place on a (pointer, length) view. */
void trim_space(const uint8_t **s, size_t *n);

/* perlDecode: what reading through <:encoding(utf8)> makes of the bytes. A
 * valid string is returned unchanged; otherwise each invalid byte becomes the
 * four characters \xHH and the result is appended to scratch. */
void perl_decode(const uint8_t **s, size_t *n, buf_t *scratch);

#endif
