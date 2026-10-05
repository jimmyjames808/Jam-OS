/* fbbench: the lines. Each is one piece of work the compositor will do,
 * timed whole (a sample is the whole screen, or the whole rectangle,
 * done once by the crew), with its rate: GB/s of the bytes it wrote at
 * the median, or microseconds per megapixel for a blend.
 *
 *   fb write     the framebuffer's write-combining stores: a fill (pure
 *                writes) and a copy from RAM, at 1, 2, 4, 8, 16 and all
 *                threads; every store width at 1 and all threads; tiles
 *                of several shapes written from a small cache-hot buffer
 *                (the compositor's way: compose a tile, then write it);
 *                libfun's present (64-pixel pieces compared with a copy
 *                of what the screen shows) with every piece changed and
 *                with none
 *   RAM copy     memory to memory, for comparison
 *   blend        premultiplied argb over xrgb, streamed from a client's
 *                buffer into a cache-hot band: scalar, SSE2, AVX2
 *   vmo_read     its copy rate (Q2's fallback reads every damaged pixel
 *                through it)
 *   frame        whole frames as the compositor will paint them: an
 *                opaque full-screen window both ways (mapped, Q2's A;
 *                vmo_read, Q2's B), a blended full-screen window both
 *                ways, a 1280x800 window over another, one line of text,
 *                the pointer moving
 * Thread counts above the CPU count are left out. */
#include "fbbench.h"

#define BG            0x1e1a1d   /* the splash's background (<splash.h> SPLASH_BG) */
#define WIN_W         1280   /* the typical window */
#define WIN_H         800
#define PTR           32     /* a pointer square's side */

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

/* The thread counts of a sweep: 1, 2, 4, 8, 16 and all, up to all. */
static unsigned sweep(uint32_t *t)
{
    static const uint32_t want[] = { 1, 2, 4, 8, 16 };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        if (want[i] < B.ncpu)
            t[n++] = want[i];
    t[n++] = B.ncpu;
    return n;
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

/* The whole screen: to the framebuffer from src (NULL: a fill). */
static struct job screen_job(kernel_fn k, const uint32_t *src)
{
    return (struct job){ k, { B.fb, src, B.fbpitch, (uint32_t)B.w, B.w, B.h, 0x335577 } };
}

/* ---- framebuffer writes ------------------------------------------------------------- */

static void fb_sweeps(void)
{
    uint32_t t[8];
    unsigned nt = sweep(t);
    struct job fill = screen_job(kernel_fill[K_16], NULL);
    struct job copy = screen_job(kernel_copy[K_16], B.opaque);
    for (unsigned i = 0; i < nt; i++)
        measure(&(struct line){ "fb write: fill, 16-byte stores, whole screen", t[i], band_item,
                                &fill, bands(B.h), B.frame, 0, 0 });
    for (unsigned i = 0; i < nt; i++)
        measure(&(struct line){ "fb write: copy from RAM, 16-byte, whole screen", t[i],
                                band_item, &copy, bands(B.h), B.frame, 0, 0 });
}

static void fb_widths(void)
{
    char what[80];
    for (int k = 0; k < K_COUNT; k++) {
        if (k == K_16 || (k == K_32 && !B.avx2))
            continue;   /* 16-byte: the sweeps have it */
        struct job fill = screen_job(kernel_fill[k], NULL);
        struct job copy = screen_job(kernel_copy[k], B.opaque);
        snprintf(what, sizeof(what), "fb write: fill, %s stores, whole screen", kernel_name[k]);
        measure(&(struct line){ what, 1, band_item, &fill, bands(B.h), B.frame, 0, 0 });
        measure(&(struct line){ what, B.ncpu, band_item, &fill, bands(B.h), B.frame, 0, 0 });
        snprintf(what, sizeof(what), "fb write: copy from RAM, %s, whole screen", kernel_name[k]);
        measure(&(struct line){ what, B.ncpu, band_item, &copy, bands(B.h), B.frame, 0, 0 });
    }
}

/* Tiles of tw x th, each written from the thread's own tile buffer. */
struct tiles {
    int tw, th;    /* a tile's size */
    int across;    /* tiles in a row of them */
};

static void tile_item(uint32_t item, uint32_t thread, void *arg)
{
    const struct tiles *t = arg;
    int x = (int)(item % (uint32_t)t->across) * t->tw;
    int y = (int)(item / (uint32_t)t->across) * t->th;
    struct rows r = { B.fb + (uint64_t)y * B.fbpitch + x, tile_buf[thread], B.fbpitch,
                      (uint32_t)t->tw, t->tw, t->th, 0 };
    r.w = B.w - x < t->tw ? B.w - x : t->tw;
    r.rows = B.h - y < t->th ? B.h - y : t->th;
    kernel_copy[K_16](&r);
}

static void fb_tiles(void)
{
    static const int shape[][2] = { { 64, 16 }, { 256, 16 }, { 64, 64 }, { 0, BAND } };
    char what[80];
    for (unsigned i = 0; i < sizeof(shape) / sizeof(shape[0]); i++) {
        struct tiles t = { shape[i][0] ? shape[i][0] : B.w, shape[i][1], 0 };
        t.across = (B.w + t.tw - 1) / t.tw;
        uint32_t n = (uint32_t)(t.across * ((B.h + t.th - 1) / t.th));
        snprintf(what, sizeof(what), "fb write: %dx%d tiles from a cache-hot buffer", t.tw, t.th);
        measure(&(struct line){ what, B.ncpu, tile_item, &t, n, B.frame, 0, 0 });
    }
}

/* libfun's present (user/apps/fun/gfx.c): each row in 64-pixel pieces,
 * a piece that differs from the shown copy written to it and to the
 * framebuffer. `flip` alternates the source between two buffers that
 * differ everywhere, so every piece changes every time. */
struct present {
    bool      flip;     /* change the source before each run */
    unsigned  runs;     /* runs so far */
};

static bool same64(const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t diff = 0;
    for (int i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return !diff;
}

static void present_item(uint32_t item, uint32_t thread, void *arg)
{
    (void)thread;
    const struct present *p = arg;
    const uint32_t *src = p->flip && p->runs % 2 ? B.win : B.opaque;
    int y0 = (int)item * BAND, y1 = y0 + BAND < B.h ? y0 + BAND : B.h;
    for (int y = y0; y < y1; y++) {
        uint64_t at = (uint64_t)y * (uint64_t)B.w;
        for (int x = 0; x < B.w; x += 64) {
            int n = B.w - x < 64 ? B.w - x : 64;
            if (same64(src + at + x, B.shown + at + x, n))
                continue;
            struct rows r = { B.shown + at + x, src + at + x, 0, 0, n, 1, 0 };
            kernel_copy[K_16](&r);
            r.dst = B.fb + (uint64_t)y * B.fbpitch + x;
            kernel_copy[K_16](&r);
        }
    }
}

static void present_run(void *arg)
{
    struct present *p = arg;
    crew_run(present_item, p, bands(B.h));
    p->runs++;
}

static void fb_present(void)
{
    uint32_t n = crew_start(B.ncpu);
    for (int flip = 1; flip >= 0; flip--) {
        struct present p = { flip, 0 };
        uint64_t med, p99;
        time_line(present_run, &p, B.samples, &med, &p99);
        char what[96], rate[32];
        snprintf(what, sizeof(what), "fb write: libfun present, %s, %u threads",
                 flip ? "every piece changed" : "nothing changed", n);
        rate_gbs(rate, sizeof(rate), flip ? B.frame : 0, med);
        out_line(what, med, p99, flip ? rate : "(compare only)");
    }
    crew_stop();
}

bool lines_fb(void)
{
    if (!B.fb)
        return true;
    fb_sweeps();
    fb_widths();
    fb_tiles();
    fb_present();
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

/* ---- frames -------------------------------------------------------------------------------- */

/* A frame's band: how it is made. */
struct frame {
    bool via_vmo;    /* the client's pixels through vmo_read first (Q2's B) */
    bool blended;    /* argb over the background, composed in the band buffer */
    int  x, y, w, h; /* the rectangle painted (screen pixels) */
    int  bx, by;     /* where the opaque window under it is (blended frames) */
};

/* The opaque window under the band's rows of f, or the background. */
static void under(const struct frame *f, uint32_t *buf, int y0, int rows)
{
    struct rows r = { buf, NULL, (uint32_t)f->w, 0, f->w, rows, BG };
    kernel_fill[K_16](&r);
    int x0 = f->x > f->bx ? f->x : f->bx, x1 = f->x + f->w < f->bx + WIN_W ? f->x + f->w
                                                                          : f->bx + WIN_W;
    for (int y = y0; y < y0 + rows && x0 < x1; y++) {
        if (y < f->by || y >= f->by + WIN_H)
            continue;
        struct rows c = { buf + (uint64_t)(y - y0) * (uint64_t)f->w + (x0 - f->x),
                          B.opaque + (uint64_t)(y - f->by) * (uint64_t)B.w + (x0 - f->bx),
                          0, 0, x1 - x0, 1, 0 };
        kernel_copy[K_16](&c);
    }
}

static void frame_item(uint32_t item, uint32_t thread, void *arg)
{
    const struct frame *f = arg;
    int y0 = f->y + (int)item * BAND, rows = f->y + f->h - y0 < BAND ? f->y + f->h - y0 : BAND;
    /* The client's buffer is screen-sized with the window at its top left. */
    const uint32_t *src = (f->blended ? B.win : B.opaque) + (uint64_t)(y0 - f->y) * (uint64_t)B.w;
    if (f->via_vmo) {
        uint64_t off = (uint64_t)(y0 - f->y) * (uint64_t)B.w * 4;
        if (jam_vmo_read(B.vmo, off, B.copy + off / 4, (uint64_t)rows * (uint64_t)B.w * 4) != OK)
            __atomic_store_n(&vmo_failed, true, __ATOMIC_RELAXED);
        src = B.copy + off / 4;
    }
    struct rows out = { B.fb + (uint64_t)y0 * B.fbpitch + f->x, src, B.fbpitch, (uint32_t)B.w,
                        f->w, rows, 0 };
    if (f->blended) {
        uint32_t *buf = tile_buf[thread];
        under(f, buf, y0, rows);
        struct rows bl = { buf, src, (uint32_t)f->w, (uint32_t)B.w, f->w, rows, 0 };
        kernel_blend[B.avx2 ? B_AVX2 : B_SSE2](&bl);
        out.src = buf;
        out.spitch = (uint32_t)f->w;
    }
    kernel_copy[K_16](&out);
}

static void frame_line(const char *what, uint32_t threads, struct frame f, unsigned samples)
{
    measure(&(struct line){ what, threads, frame_item, &f, bands(f.h),
                            (uint64_t)f.w * (uint64_t)f.h * 4, 0, samples });
}

/* The pointer moving: two PTR x PTR squares (where it was, where it is),
 * each composed (the window under it, the arrow blended) and written. */
static void pointer_item(uint32_t item, uint32_t thread, void *arg)
{
    (void)arg;
    int x = B.w / 2 + (int)item * 8, y = B.h / 2 + (int)item * 5;
    uint32_t *buf = tile_buf[thread];
    struct rows r = { buf, B.opaque + (uint64_t)y * (uint64_t)B.w + x, PTR, (uint32_t)B.w, PTR,
                      PTR, 0 };
    kernel_copy[K_16](&r);
    if (item == 1) {   /* the arrow is drawn where it is now */
        r.src = B.win;
        kernel_blend[B_SSE2](&r);
    }
    struct rows out = { B.fb + (uint64_t)y * B.fbpitch + x, buf, B.fbpitch, PTR, PTR, PTR, 0 };
    kernel_copy[K_16](&out);
}

bool lines_frames(void)
{
    if (!B.fb)
        return true;
    struct frame full = { false, false, 0, 0, B.w, B.h, 0, 0 };
    frame_line("frame: full screen, opaque, mapped buffer (Q2 A)", B.ncpu, full, 0);
    full.via_vmo = true;
    frame_line("frame: full screen, opaque, vmo_read first (Q2 B)", B.ncpu, full, 0);
    full.blended = true;
    frame_line("frame: full screen, argb blended, vmo_read (Q2 B)", B.ncpu, full, 0);
    full.via_vmo = false;
    frame_line("frame: full screen, argb blended, mapped (Q2 A)", B.ncpu, full, 0);
    struct frame win = { false, true, B.w / 2 - WIN_W / 2 + 160, B.h / 2 - WIN_H / 2 + 100,
                         WIN_W, WIN_H, B.w / 2 - WIN_W / 2 - 160, B.h / 2 - WIN_H / 2 - 100 };
    if (win.x >= 0 && win.y >= 0 && win.x + WIN_W <= B.w && win.y + WIN_H <= B.h && win.bx >= 0 &&
        win.by >= 0)
        frame_line("frame: 1280x800 argb window over an opaque one", B.ncpu, win, 0);
    struct frame text = { false, false, 0, B.h / 2, B.w, BAND, 0, 0 };
    frame_line("frame: one line of text (full width, 16 rows)", 1, text, B.short_samples);
    measure(&(struct line){ "frame: the pointer moved (two 32x32 squares)", 1, pointer_item, NULL,
                            2, 2 * PTR * PTR * 4, 0, B.short_samples });
    if (__atomic_load_n(&vmo_failed, __ATOMIC_RELAXED)) {
        out_text("fbbench: frame: FAILED (a vmo_read returned an error)\n");
        return false;
    }
    return true;
}
