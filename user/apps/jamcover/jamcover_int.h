/* jamcover: what its files share. main.c serves the jamcover channel
 * (abi/idl/jamcover.idl) on the handles jamjar gave it (<jamcover.h>);
 * decode.c turns a picture into the cover's pixels; stbi.c is stb_image
 * with its bounded arena; selftest.c is `jamcover --selftest`. */
#pragma once

#include <fun.h>
#include <jamcover.h>

/* stbi.c: stb_image, PNG and JPEG, its memory from one bounded arena. */
/* The picture's size, without decoding it; false: not a PNG or JPEG it reads. */
bool     stbi_size(const uint8_t *data, size_t n, int *w, int *h);
/* RGBA pixels (in the arena; NULL: it doesn't decode or doesn't fit). */
uint8_t *stbi_rgba(const uint8_t *data, size_t n, int *w, int *h);
/* Empty the arena (every image decoded so far is gone). */
void     stbi_arena_reset(void);

/* decode.c: a picture of w x h may be decoded (JAMCOVER_MAX_SIDE,
 * JAMCOVER_MAX_PIXELS). */
bool     cover_size_ok(int w, int h);
/* The picture pic[0..n) cropped to its middle square and scaled into
 * small (JAMCOVER_SMALL squared) and, if large is not NULL, large
 * (JAMCOVER_LARGE squared), premultiplied 0xAARRGGBB; *w, *h its own size.
 * The errors are decode's in jamcover.idl. The arena is emptied after. */
status_t cover_decode(const uint8_t *pic, size_t n, uint32_t *small, uint32_t *large, int *w,
                      int *h);

/* selftest.c: `jamcover --selftest`. Its exit status. */
int      jamcover_selftest(void);
