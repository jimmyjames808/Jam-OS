/* fbbench: the lines. Each is one piece of work the compositor will do,
 * timed whole (a sample is the whole screen, or the whole rectangle,
 * done once by the crew), with its rate: GB/s of the bytes it wrote at
 * the median, or microseconds per megapixel for a blend.
 *
 *   window       libfun's present into a full-screen window (64-pixel
 *                pieces compared with a copy of what the window shows,
 *                then the compositor's frame) with every piece changed
 *                and with none
 *   RAM copy     memory to memory, for comparison
 *   blend        premultiplied argb over xrgb, streamed from a client's
 *                buffer into a cache-hot band: scalar, SSE2, AVX2
 *   vmo_read     its copy rate (Q2's fallback reads every damaged pixel
 *                through it)
 * Thread counts above the CPU count are left out. The framebuffer's own
 * lines (its stores, tiles, whole frames written to it) went with the
 * screen the console lent (M12's Q14): the compositor's `stats` has its
 * paint times and pixels. */
#include "fbbench.h"


static bool vmo_failed;                /* a vmo_read in a line failed */

/* One line: the work, how many threads, and what its rate counts. */
struct line {
    const char *what;
    uint32_t    threads;
    void      (*item)(uint32_t item, uint32_t thread, void *arg);
    void       *arg;
    uint32_t    items;
    uint64_t    bytes;    /* the rate is GB/s of these bytes; 0: */
    uint64_t    pixels;   /* ... microseconds per megapixel of these */
    unsigned    samples;  /* 0: B.samples */
};

static void run_once(void *arg)
{
    const struct line *l = arg;
    crew_run(l->item, l->arg, l->items);
}

static void measure(const struct line *l)
{
    uint32_t n = crew_start(l->threads);
    uint64_t med, p99;
    time_line(run_once, (void *)l, l->samples ? l->samples : B.samples, &med, &p99);
    crew_stop();
    char what[96], rate[32];
    snprintf(what, sizeof(what), "%s, %u thread%s", l->what, n, n == 1 ? "" : "s");
    if (l->bytes)
        rate_gbs(rate, sizeof(rate), l->bytes, med);
    else
        rate_mpx(rate, sizeof(rate), l->pixels, med);
    out_line(what, med, p99, rate);
}

/* ---- bands of a rectangle ------------------------------------------------------------- */

/* A rectangle's rows in bands of BAND: r covers all of it (dst and src at
 * its top left, r.rows its height); a band is its rows item * BAND on. */
struct job {
    kernel_fn   k;
    struct rows r;
};

static struct rows band_of(const struct rows *all, uint32_t item)
{
    struct rows r = *all;
    int y0 = (int)item * BAND;
    r.rows = all->rows - y0 < BAND ? all->rows - y0 : BAND;
    r.dst += (uint64_t)y0 * r.dpitch;
    if (r.src)
        r.src += (uint64_t)y0 * r.spitch;
    return r;
}

static void band_item(uint32_t item, uint32_t thread, void *arg)
{
    (void)thread;
    const struct job *j = arg;
    struct rows r = band_of(&j->r, item);
    j->k(&r);
}

static uint32_t bands(int rows) { return (uint32_t)(rows + BAND - 1) / BAND; }

/* ---- the window --------------------------------------------------------------------------- */

/* libfun's present into a full-screen window (user/apps/fun/gfx.c,
 * wlpaint.c): the changed 64-pixel pieces copied into the window's free
 * buffer, damaged and committed, one commit per frame callback, so a
 * sample is a frame of the compositor's too. `flip` alternates the
 * picture between two that differ everywhere, so every piece changes
 * every time. */
struct present {
    bool      flip;     /* change the picture before each present */
    unsigned  runs;     /* presents so far */
};

static void present_run(void *arg)
{
    struct present *p = arg;
    if (p->flip)
        memcpy(scr.s.px, p->runs % 2 ? B.win : B.opaque, B.frame);
    gfx_present();
    p->runs++;
}

bool lines_window(void)
{
    if (!B.windowed)
        return true;
    for (int flip = 1; flip >= 0; flip--) {
        struct present p = { flip, 0 };
        uint64_t med, p99;
        time_line(present_run, &p, B.samples, &med, &p99);
        char what[96], rate[32];
        snprintf(what, sizeof(what), "window: libfun present, %s, %u threads",
                 flip ? "every piece changed" : "nothing changed", pool_threads());
        rate_gbs(rate, sizeof(rate), flip ? B.frame : 0, med);
        out_line(what, med, p99, flip ? rate : "(compare only)");
    }
    return true;
}

/* ---- RAM -------------------------------------------------------------------------------- */

void lines_ram(void)
{
    static const int ks[] = { K_16, K_REP, K_NT };
    char what[80];
    for (unsigned i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        struct job copy = { kernel_copy[ks[i]],
                            { B.copy, B.opaque, (uint32_t)B.w, (uint32_t)B.w, B.w, B.h, 0 } };
        snprintf(what, sizeof(what), "RAM copy: %s, whole screen's bytes", kernel_name[ks[i]]);
        if (ks[i] != K_NT)
            measure(&(struct line){ what, 1, band_item, &copy, bands(B.h), B.frame, 0, 0 });
        measure(&(struct line){ what, B.ncpu, band_item, &copy, bands(B.h), B.frame, 0, 0 });
    }
}

/* ---- blends ------------------------------------------------------------------------------ */

/* A client's band blended into the thread's own band buffer (the
 * compositor composes a tile in cache, then writes it). */
static void blend_item(uint32_t item, uint32_t thread, void *arg)
{
    const kernel_fn *k = arg;
    struct rows all = { tile_buf[thread], B.win, (uint32_t)B.w, (uint32_t)B.w, B.w, B.h, 0 };
    struct rows r = band_of(&all, item);
    r.dst = tile_buf[thread];
    (*k)(&r);
}

void lines_blend(void)
{
    char what[80];
    uint64_t px = (uint64_t)B.w * (uint64_t)B.h;
    int best = B.avx2 ? B_AVX2 : B_SSE2;
    for (int k = 0; k < B_COUNT; k++) {
        if (k == B_AVX2 && !B.avx2)
            continue;
        snprintf(what, sizeof(what), "blend: argb over xrgb, %s%s, whole screen", blend_name[k],
                 k == B_SCALAR ? " (px_over)" : "");
        measure(&(struct line){ what, 1, blend_item, (void *)&kernel_blend[k], bands(B.h), 0, px,
                                0 });
        if (k == best)
            measure(&(struct line){ what, B.ncpu, blend_item, (void *)&kernel_blend[k],
                                    bands(B.h), 0, px, 0 });
    }
}

/* ---- vmo_read ----------------------------------------------------------------------------- */

struct vread {
    uint64_t size;   /* bytes a call reads */
};

static void vread_one(uint32_t item, uint32_t thread, void *arg)
{
    (void)item;
    (void)thread;
    const struct vread *v = arg;
    if (jam_vmo_read(B.vmo, 0, B.copy, v->size) != OK)
        __atomic_store_n(&vmo_failed, true, __ATOMIC_RELAXED);
}

/* Band item of the whole screen through vmo_read into B.copy. */
static void vread_band(uint32_t item, uint32_t thread, void *arg)
{
    (void)thread;
    (void)arg;
    int y0 = (int)item * BAND, rows = B.h - y0 < BAND ? B.h - y0 : BAND;
    uint64_t off = (uint64_t)y0 * (uint64_t)B.w * 4;
    if (jam_vmo_read(B.vmo, off, B.copy + off / 4, (uint64_t)rows * (uint64_t)B.w * 4) != OK)
        __atomic_store_n(&vmo_failed, true, __ATOMIC_RELAXED);
}

bool lines_vmo(void)
{
    static const uint64_t sizes[] = { 4096, 65536, 1u << 20 };
    char what[80];
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct vread v = { sizes[i] };
        snprintf(what, sizeof(what), "vmo_read: %lu KiB in one call",
                 (unsigned long)(sizes[i] >> 10));
        measure(&(struct line){ what, 1, vread_one, &v, 1, sizes[i], 0, B.short_samples });
    }
    struct vread whole = { B.frame };
    measure(&(struct line){ "vmo_read: whole screen's bytes in one call", 1, vread_one, &whole, 1,
                            B.frame, 0, 0 });
    measure(&(struct line){ "vmo_read: whole screen's bytes in 16-row bands", B.ncpu, vread_band,
                            NULL, bands(B.h), B.frame, 0, 0 });
    if (__atomic_load_n(&vmo_failed, __ATOMIC_RELAXED)) {
        out_text("fbbench: vmo_read: FAILED (a call returned an error)\n");
        return false;
    }
    return true;
}
