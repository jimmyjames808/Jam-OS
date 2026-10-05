/* libfun: stb_truetype's implementation (third_party/stb_truetype, public
 * domain or MIT), compiled once, here, for the smooth text's baking
 * (font.c), which is its only caller.
 *
 * No C library: its memory is libos's malloc and free (only while a font
 * is opened: font.c frees everything stb_truetype made before it
 * returns), its maths libfun's (fun.h: sqrtd, floord), and its string
 * functions libos's. It reads only the fonts built into libfun
 * (fontdata.c), never one from outside: stb_truetype doesn't check a
 * font's offsets against its size. A broken invariant traps rather than
 * carrying on. */
#include "internal.h"

static inline double fabsd(double x)
{
    return x < 0 ? -x : x;
}

/* fmod, pow, cos and acos: only stb_truetype's signed distance fields
 * (stbtt_GetGlyphSDF and its helpers) use them, and libfun never makes
 * one, so they trap instead of being written for nothing. */
static double sdf_only(void)
{
    __builtin_trap();
}

#define STBTT_ifloor(x)  ((int)floord(x))
#define STBTT_iceil(x)   (-(int)floord(-(x)))
#define STBTT_sqrt(x)    sqrtd(x)
#define STBTT_fabs(x)    fabsd(x)
#define STBTT_fmod(x, y) ((void)(x), (void)(y), sdf_only())
#define STBTT_pow(x, y)  ((void)(x), (void)(y), sdf_only())
#define STBTT_cos(x)     ((void)(x), sdf_only())
#define STBTT_acos(x)    ((void)(x), sdf_only())
#define STBTT_malloc(n, u) ((void)(u), malloc(n))
#define STBTT_free(p, u)   ((void)(u), free(p))
#define STBTT_assert(x)  ((x) ? (void)0 : __builtin_trap())
#define STBTT_strlen(s)  strlen(s)
#define STBTT_memcpy     memcpy
#define STBTT_memset     memset
#define STB_TRUETYPE_IMPLEMENTATION

/* The vendored code as released: its own warnings under -Wall -Wextra are
 * not ours to fix (it is not edited), so they are not errors here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <stb_truetype.h>
#pragma GCC diagnostic pop
