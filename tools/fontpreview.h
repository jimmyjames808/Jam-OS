/* fontpreview (tools/fontpreview.c): what its files share. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* tools/fontcheck.c: the --check run; the exit code. */
int  font_check(void);
/* tools/png.c: w x h pixels (0xRRGGBB, stride a row) as an RGB PNG at
 * path, uncompressed. False (and a line on stderr) if it could not. */
bool png_write(const char *path, const uint32_t *px, int w, int h, int stride);
