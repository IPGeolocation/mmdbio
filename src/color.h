/* Coloured output, only on a terminal: off with --nocolor, with NO_COLOR
 * set (https://no-color.org), or with TERM=dumb. */
#ifndef COLOR_H
#define COLOR_H

#include <stdbool.h>
#include <stdio.h>

#define C_RED "31"
#define C_GREEN "32"
#define C_YELLOW "33"
#define C_BOLD "1"
#define C_DIM "2"

void color_disable(void);
bool color_on(FILE *f);
/* The escape that starts code (and the one that resets), or "" when f gets
 * no colour. */
const char *col(FILE *f, const char *code);
const char *col_end(FILE *f);

#endif
