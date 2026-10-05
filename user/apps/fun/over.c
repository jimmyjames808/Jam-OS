/* libfun: a row of premultiplied pixels over a row of opaque ones (fun.h
 * "px_over_row"), the compositor's blend and fbbench's.
 *
 * Every version gives px_over's result exactly (alpha.c), per channel:
 *     out = src + (x + (x >> 8)) >> 8,   x = dst * (255 - a) + 128
 * clamped to 255, top byte 0. The SIMD ones do it on 16-bit lanes (no lane
 * overflows: x <= 255 * 255 + 128 + 254 < 65536), alpha broadcast to its
 * pixel's lanes with a shuffle: SSE2 two pixels a step, AVX2 four (the
 * shuffle stays inside each 128-bit half, as vpshufb does). Loads and
 * stores go through __builtin_memcpy, so neither row has to be aligned.
 *
 * px_over_row picks AVX2 where the CPU and the OS allow it and we are not
 * under QEMU's emulator (TCG runs AVX slowly), else SSE2; the choice is
 * made once, by whichever thread asks first (every thread computes the
 * same answer, so the race is harmless; the word is atomic all the same).
 * On the PC (fbbench, 2026-10-05): scalar 2138 us per megapixel, SSE2 687,
 * AVX2 292, one thread. */
#include "internal.h"

#define AVX2 __attribute__((target("avx2")))

typedef uint8_t  v8u8   __attribute__((vector_size(8)));
typedef uint16_t v8u16  __attribute__((vector_size(16)));
typedef uint8_t  v16u8  __attribute__((vector_size(16)));
typedef uint16_t v16u16 __attribute__((vector_size(32)));

/* Two pixels as eight 16-bit lanes (b, g, r, a each). */
static inline v8u16 over2(v8u16 d, v8u16 s)
{
    static const v8u16 alpha = { 3, 3, 3, 3, 7, 7, 7, 7 };
    static const v8u16 rgb = { 0xffff, 0xffff, 0xffff, 0, 0xffff, 0xffff, 0xffff, 0 };
    v8u16 x = d * (255 - __builtin_shuffle(s, alpha)) + 128;
    x = ((x + (x >> 8)) >> 8) + s;
    v8u16 big = (v8u16)(x > 255);
    return ((x & ~big) | (big & 255)) & rgb;
}

void px_over_row_sse2(uint32_t *dst, const uint32_t *src, int n)
{
    int i = 0;
    for (; i + 2 <= n; i += 2) {
        v8u8 d8, s8;
        __builtin_memcpy(&d8, dst + i, 8);
        __builtin_memcpy(&s8, src + i, 8);
        v8u16 x = over2(__builtin_convertvector(d8, v8u16), __builtin_convertvector(s8, v8u16));
        v8u8 o = __builtin_convertvector(x, v8u8);
        __builtin_memcpy(dst + i, &o, 8);
    }
    if (i < n)
        dst[i] = px_over(dst[i], src[i]);
}

/* Four pixels as sixteen 16-bit lanes. */
AVX2 static inline v16u16 over4(v16u16 d, v16u16 s)
{
    static const v16u16 alpha = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };
    static const v16u16 rgb = { 0xffff, 0xffff, 0xffff, 0, 0xffff, 0xffff, 0xffff, 0,
                                0xffff, 0xffff, 0xffff, 0, 0xffff, 0xffff, 0xffff, 0 };
    v16u16 x = d * (255 - __builtin_shuffle(s, alpha)) + 128;
    x = ((x + (x >> 8)) >> 8) + s;
    v16u16 big = (v16u16)(x > 255);
    return ((x & ~big) | (big & 255)) & rgb;
}

AVX2 void px_over_row_avx2(uint32_t *dst, const uint32_t *src, int n)
{
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        v16u8 d8, s8;
        __builtin_memcpy(&d8, dst + i, 16);
        __builtin_memcpy(&s8, src + i, 16);
        v16u16 x = over4(__builtin_convertvector(d8, v16u16), __builtin_convertvector(s8, v16u16));
        v16u8 o = __builtin_convertvector(x, v16u8);
        __builtin_memcpy(dst + i, &o, 16);
    }
    for (; i < n; i++)
        dst[i] = px_over(dst[i], src[i]);
}

/* 0: not decided yet; 1: SSE2; 2: AVX2. */
static int over_kind;

void px_over_row(uint32_t *dst, const uint32_t *src, int n)
{
    int k = __atomic_load_n(&over_kind, __ATOMIC_RELAXED);
    if (!k) {
        k = fun_has_avx2() && !fun_is_tcg() ? 2 : 1;
        __atomic_store_n(&over_kind, k, __ATOMIC_RELAXED);
    }
    if (k == 2)
        px_over_row_avx2(dst, src, n);
    else
        px_over_row_sse2(dst, src, n);
}
