/* fbbench: what the compositor will depend on, measured on the real
 * framebuffer (docs/G1-PLAN.md, track P0). Its files: main.c (the screen
 * borrowed, the buffers, the order of the lines), timing.c (the TSC, the
 * samples and the output lines), crew.c (a set of worker threads of a
 * chosen size), kernels.c (the store, copy and blend loops being
 * compared), lines.c (each line's work).
 *
 * Every whole-screen line is cut into bands of BAND rows, the compositor's
 * tile height, and the bands are handed out one at a time to the crew's
 * threads, as libfun's present and the compositor's paint do. Pixels are
 * 32 bits; the framebuffer's rows are `pitch` bytes apart; nothing ever
 * reads the framebuffer (reads of write-combining memory are very slow,
 * and the compositor never does them). */
#pragma once

#include <fun.h>
#include <os.h>

#define BAND 16   /* rows of one work item, as the compositor's tiles */

/* The screen and the buffers the lines use. */
struct bench {
    uint32_t *fb;        /* the framebuffer, write-combining; NULL: no screen */
    uint32_t  fbpitch;   /* its pixels from one row to the next */
    int       w, h;      /* screen pixels (2560x1440 when there is no screen) */
    uint64_t  frame;     /* w * h * 4: bytes of one full-screen buffer */
    uint32_t *win;       /* a client's full-screen buffer, premultiplied argb */
    uint32_t *opaque;    /* another, xrgb: an opaque window, the background */
    uint32_t *copy;      /* the compositor's own copy (Q2's B), a RAM-to-RAM target */
    uint32_t *shown;     /* libfun's "what the screen shows" copy */
    handle_t  vmo;       /* the VMO `win` is mapped from: vmo_read reads it */
    uint32_t  ncpu;      /* logical CPUs (libfun's count): the most threads used */
    unsigned  samples;   /* samples of a whole-screen line */
    unsigned  short_samples;   /* of a line that takes microseconds */
    bool      avx2;      /* AVX2 usable: its lines are measured */
};
extern struct bench B;
/* Each crew thread's own band buffer (main.c): max(w * BAND, 64 * 64)
 * pixels it composes a tile in, so the tile is in its cache. */
extern uint32_t *tile_buf[];

/* ---- timing.c ------------------------------------------------------------------ */

/* The TSC's rate against the clock, and the cost of a timestamp. */
void     timing_init(void);
uint64_t timing_tsc_mhz(void);
uint64_t timing_stamp_ps(void);
/* The median and the 99th percentile of n samples of fn(arg), after a
 * warm-up (WARM_NS, at least two untimed runs), in picoseconds. */
void     time_line(void (*fn)(void *), void *arg, unsigned n, uint64_t *med, uint64_t *p99);
/* An output line, kept until out_flush (nothing is printed while
 * measuring): "fbbench: <what> median <t> p99 <t> <rate>". */
void     out_line(const char *what, uint64_t med, uint64_t p99, const char *rate);
/* Any other line, kept the same way. */
void     out_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Print what was kept, then forget it. */
void     out_flush(void);
/* "6.12 GB/s" for bytes in ps; "813 us/Mpx" for pixels in ps. */
void     rate_gbs(char *buf, size_t n, uint64_t bytes, uint64_t ps);
void     rate_mpx(char *buf, size_t n, uint64_t pixels, uint64_t ps);

/* ---- crew.c ---------------------------------------------------------------------- */

#define CREW_MAX FUN_MAX_THREADS

/* n threads in all (1 .. CREW_MAX), the caller's included: n - 1 workers
 * that spin between jobs. How many it has (fewer if a thread couldn't be
 * made). */
uint32_t crew_start(uint32_t n);
/* fn(item, thread, arg) for every item 0 .. items - 1, handed out one at a
 * time to the crew's threads; returns when all are done. */
void     crew_run(void (*fn)(uint32_t item, uint32_t thread, void *arg), void *arg, uint32_t items);
/* End the workers (and wait for them). */
void     crew_stop(void);

/* ---- kernels.c --------------------------------------------------------------------- */

/* A loop that writes or copies rows of pixels: `rows` rows of w pixels,
 * dst rows dpitch pixels apart, src rows spitch apart. */
struct rows {
    uint32_t       *dst;
    const uint32_t *src;      /* unused by a fill */
    uint32_t        dpitch, spitch;
    int             w, rows;
    uint32_t        colour;   /* a fill's */
};
typedef void (*kernel_fn)(const struct rows *r);

/* The store loops compared: 4-, 16- and 32-byte stores, 16-byte
 * non-temporal stores, and the string instructions (rep stosd, rep
 * movsb). */
enum { K_4, K_16, K_32, K_NT, K_REP, K_COUNT };
extern const char *const kernel_name[K_COUNT];
extern const kernel_fn kernel_fill[K_COUNT];
extern const kernel_fn kernel_copy[K_COUNT];
/* src (premultiplied argb) over dst (xrgb), in place: scalar (libfun's
 * px_over, a call a pixel), SSE2 (two pixels a step) and AVX2 (four). */
enum { B_SCALAR, B_SSE2, B_AVX2, B_COUNT };
extern const char *const blend_name[B_COUNT];
extern const kernel_fn kernel_blend[B_COUNT];
/* The SIMD blends give px_over's result exactly, on random pixels and
 * every tail length; false with why. */
bool     kernels_selftest(char *why, size_t n);
/* A pixel pattern into px (n pixels): opaque xrgb, or premultiplied argb
 * with every alpha. */
void     pattern_xrgb(uint32_t *px, uint64_t n, uint64_t seed);
void     pattern_argb(uint32_t *px, uint64_t n, uint64_t seed);

/* ---- lines.c ------------------------------------------------------------------------- */

/* Each section's lines (main.c runs them in this order). */
bool     lines_fb(void);       /* framebuffer writes: threads, store widths, tiles, present */
void     lines_ram(void);      /* RAM to RAM copies */
void     lines_blend(void);    /* the blend per megapixel */
bool     lines_vmo(void);      /* vmo_read's rate */
bool     lines_frames(void);   /* whole frames the compositor will paint */
