#include "color.h"

#include <stdlib.h>
#include <string.h>

#include "compat.h"

static bool disabled;

void color_disable(void) { disabled = true; }

bool color_on(FILE *f)
{
	if (disabled)
		return false;
	const char *no = getenv("NO_COLOR");
	if (no && *no)
		return false;
	const char *term = getenv("TERM");
	if (term && strcmp(term, "dumb") == 0)
		return false;
	return terminal_colors(f);
}

const char *col(FILE *f, const char *code)
{
	static char esc[4][16];
	static int i;
	if (!color_on(f))
		return "";
	i = (i + 1) % 4;
	snprintf(esc[i], sizeof esc[i], "\033[%sm", code);
	return esc[i];
}

const char *col_end(FILE *f) { return color_on(f) ? "\033[0m" : ""; }
