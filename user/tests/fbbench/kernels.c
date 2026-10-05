/* fbbench: the loops being compared. Each writes `rows` rows of w pixels
 * (struct rows). The store loops differ only in how they store:
 *   4      one 32-bit store a pixel (vectorising turned off)
 *   16     16-byte stores (SSE2, what -march=x86-64 code gets, and what
 *          libfun's present compiles to)
 *   32     32-byte stores (AVX2; only measured where AVX2 is usable)
 *   nt     16-byte non-temporal stores (movntdq), then sfence: they skip
 *          the cache, which helps a copy to RAM and should change nothing
 *          for write-combining memory
 *   rep    rep stosd for a fill, rep movsb for a copy, a row at a time
 * None is ever turned into a call to memcpy or memset (the attribute on
 * each). Each copies r's fields it loops on into locals first: its stores
 * could alias *r, so the compiler would read them again every pixel.
 *
 * The blends put a premultiplied argb pixel over an opaque one, exactly
 * as libfun's px_over does (alpha.c): px_over itself a pixel at a time,
 * and libfun's SSE2 and AVX2 row blends (over.c, what the compositor
 * paints with); kernels_selftest checks that they give px_over's answer
 * for every pixel. */
#include "fbbench.h"

#define NOLIB __attribute__((optimize("no-tree-loop-distribute-patterns")))
#define AVX2  __attribute__((target("avx2")))

typedef uint32_t  v4u32  __attribute__((vector_size(16)));
typedef uint32_t  v8u32  __attribute__((vector_size(32)));
typedef long long v2i64  __attribute__((vector_size(16)));

static inline uint32_t *drow(const struct rows *r, int y)
{
    return r->dst + (uint64_t)y * r->dpitch;
}

static inline const uint32_t *srow(const struct rows *r, int y)
{
    return r->src + (uint64_t)y * r->spitch;
}

/* ---- fills --------------------------------------------------------------------------- */

NOLIB __attribute__((optimize("no-tree-vectorize")))
static void fill_4(const struct rows *r)
{
    int w = r->w;
    uint32_t c = r->colour;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        for (int i = 0; i < w; i++)
            d[i] = c;
    }
}

NOLIB static void fill_16(const struct rows *r)
{
    int w = r->w;
    uint32_t c = r->colour;
    v4u32 v = { c, c, c, c };
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        int i = 0;
        for (; i + 4 <= w; i += 4)
            __builtin_memcpy(d + i, &v, 16);
        for (; i < w; i++)
            d[i] = c;
    }
}

NOLIB AVX2 static void fill_32(const struct rows *r)
{
    int w = r->w;
    uint32_t c = r->colour;
    v8u32 v = { c, c, c, c, c, c, c, c };
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        int i = 0;
        for (; i + 8 <= w; i += 8)
            __builtin_memcpy(d + i, &v, 32);
        for (; i < w; i++)
            d[i] = c;
    }
}

/* Pixels before d is 16-byte aligned (movntdq needs it). */
static inline int to_align16(const uint32_t *d, int w)
{
    int n = (int)((16 - ((uintptr_t)d & 15)) & 15) / 4;
    return n < w ? n : w;
}

NOLIB static void fill_nt(const struct rows *r)
{
    int w = r->w;
    uint32_t c = r->colour;
    v4u32 v = { c, c, c, c };
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        int i = 0, head = to_align16(d, w);
        for (; i < head; i++)
            d[i] = c;
        for (; i + 4 <= w; i += 4)
            __builtin_ia32_movntdq((v2i64 *)(void *)(d + i), (v2i64)v);
        for (; i < w; i++)
            d[i] = c;
    }
    __builtin_ia32_sfence();
}

static void fill_rep(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        uint64_t n = (uint64_t)w;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(r->colour) : "memory");
    }
}

/* ---- copies ------------------------------------------------------------------------- */

NOLIB __attribute__((optimize("no-tree-vectorize")))
static void copy_4(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
}

NOLIB static void copy_16(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        int i = 0;
        for (; i + 4 <= w; i += 4) {
            v4u32 v;
            __builtin_memcpy(&v, s + i, 16);
            __builtin_memcpy(d + i, &v, 16);
        }
        for (; i < w; i++)
            d[i] = s[i];
    }
}

NOLIB AVX2 static void copy_32(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        int i = 0;
        for (; i + 8 <= w; i += 8) {
            v8u32 v;
            __builtin_memcpy(&v, s + i, 32);
            __builtin_memcpy(d + i, &v, 32);
        }
        for (; i < w; i++)
            d[i] = s[i];
    }
}

NOLIB static void copy_nt(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        int i = 0, head = to_align16(d, w);
        for (; i < head; i++)
            d[i] = s[i];
        for (; i + 4 <= w; i += 4) {
            v2i64 v;
            __builtin_memcpy(&v, s + i, 16);
            __builtin_ia32_movntdq((v2i64 *)(void *)(d + i), v);
        }
        for (; i < w; i++)
            d[i] = s[i];
    }
    __builtin_ia32_sfence();
}

static void copy_rep(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        uint64_t n = (uint64_t)w * 4;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    }
}

const char *const kernel_name[K_COUNT] = { "4-byte", "16-byte", "32-byte", "non-temporal",
                                           "rep" };
const kernel_fn kernel_fill[K_COUNT] = { fill_4, fill_16, fill_32, fill_nt, fill_rep };
const kernel_fn kernel_copy[K_COUNT] = { copy_4, copy_16, copy_32, copy_nt, copy_rep };

/* ---- blends ------------------------------------------------------------------------- */

static void blend_scalar(const struct rows *r)
{
    int w = r->w;
    for (int y = 0; y < r->rows; y++) {
        uint32_t *d = drow(r, y);
        const uint32_t *s = srow(r, y);
        for (int i = 0; i < w; i++)
            d[i] = px_over(d[i], s[i]);
    }
}

/* libfun's row blends (over.c), the compositor's: a row at a time. */
static void blend_sse2(const struct rows *r)
{
    for (int y = 0; y < r->rows; y++)
        px_over_row_sse2(drow(r, y), srow(r, y), r->w);
}

static void blend_avx2(const struct rows *r)
{
    for (int y = 0; y < r->rows; y++)
        px_over_row_avx2(drow(r, y), srow(r, y), r->w);
}

const char *const blend_name[B_COUNT] = { "scalar", "SSE2", "AVX2" };
const kernel_fn kernel_blend[B_COUNT] = { blend_scalar, blend_sse2, blend_avx2 };

/* ---- patterns and the self-test --------------------------------------------------- */

void pattern_xrgb(uint32_t *px, uint64_t n, uint64_t seed)
{
    uint64_t s = seed | 1;
    for (uint64_t i = 0; i < n; i++)
        px[i] = (uint32_t)rng_next(&s) & 0xffffff;
}

void pattern_argb(uint32_t *px, uint64_t n, uint64_t seed)
{
    uint64_t s = seed | 1;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)rng_next(&s);
        px[i] = argb_pm(v & 0xffffff, v >> 24);
    }
}

#define ST_W    37    /* a row's pixels: every tail length of both SIMD steps */
#define ST_ROWS 64

/* One blend against px_over on the same rows; the first difference into why. */
static bool same_as_scalar(int k, const uint32_t *src, const uint32_t *dst, char *why, size_t n)
{
    static uint32_t want[ST_W * ST_ROWS], got[ST_W * ST_ROWS];
    for (int w = 1; w <= ST_W; w++) {
        memcpy(want, dst, sizeof(want));
        memcpy(got, dst, sizeof(got));
        struct rows r = { .src = src, .dpitch = ST_W, .spitch = ST_W, .w = w, .rows = ST_ROWS };
        r.dst = want;
        blend_scalar(&r);
        r.dst = got;
        kernel_blend[k](&r);
        for (int i = 0; i < ST_W * ST_ROWS; i++)
            if (got[i] != want[i]) {
                snprintf(why, n, "%s blend, width %d, pixel %d: %#x over %#x gave %#x, want %#x",
                         blend_name[k], w, i, src[i], dst[i], got[i], want[i]);
                return false;
            }
    }
    return true;
}

bool kernels_selftest(char *why, size_t n)
{
    static uint32_t src[ST_W * ST_ROWS], dst[ST_W * ST_ROWS];
    pattern_xrgb(dst, ST_W * ST_ROWS, 7);
    pattern_argb(src, ST_W * ST_ROWS, 11);
    /* The ends of the range, and colours above their alpha (not valid
     * premultiplied: px_over clamps them, so must the SIMD ones). */
    src[0] = 0;
    src[1] = 0xff000000;
    src[2] = 0xffffffff;
    src[3] = 0x01ffffff;
    src[4] = 0x80ff00ff;
    dst[0] = dst[1] = dst[2] = 0xffffff;
    for (int k = B_SSE2; k < B_COUNT; k++) {
        if (k == B_AVX2 && !B.avx2)
            continue;
        if (!same_as_scalar(k, src, dst, why, n))
            return false;
    }
    return true;
}
