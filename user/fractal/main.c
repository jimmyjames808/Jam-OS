/* fractal: an interactive Mandelbrot / Julia explorer drawn by every CPU.
 *
 *   run fractal [threads=N] [noavx|avx]   explore
 *   run fractal --selftest            check the maths and the parallel render, exit 0 if right
 *
 * Real pixels at the screen's full resolution (borrowed from the console).
 * The screen is cut into 16 x 16 tiles; tiles are handed to the pool's
 * threads (one per CPU) through an atomic counter, the middle of the
 * screen first. Rendering is progressive: a first pass computes one pixel
 * in 16 x 16 (each drawn as a block), then 1 in 8 x 8, 4 x 4, 2 x 2 and
 * every pixel, and then anti-aliasing: pixels on an edge (a big step in
 * colour to a neighbour, or the set's border) get 2x2 .. 4x4 samples.
 * The work is done in slices of ~30 ms between which the keys are read and
 * the picture shown, so a key always gets an answer at once. What was
 * computed is reused: panning moves it (only the uncovered strip is new),
 * zooming stretches it as a first guess that the passes then replace, so
 * the tour glides.
 *
 * Every pixel keeps its smooth (continuous) iteration count, so colouring
 * (palettes, colour cycling, colour density) needs no new iterations. The
 * counts are measured from the smallest in view and put through a square
 * root, so deep views are as colourful as shallow ones.
 *
 * Deep zoom: past 1e12x doubles can't tell neighbouring pixels apart, so
 * the Mandelbrot set switches to perturbation around a double-double
 * reference orbit and Julia sets to double-double pixels (fractal.h); the
 * HUD says DEEP. Both go to 1e28x, the tour too.
 *
 * Keys: arrows move, + / - zoom (also Page Up / Page Down, Enter), j Julia
 * set of the point in the middle (j again: back), c colours, d colour
 * density, space colour cycling, a anti-aliasing (off, 2x2, 3x3, 4x4), [ ]
 * fewer / more iterations (0: automatic), 1-6 famous places, t tour (keeps
 * zooming in), b benchmark (1 CPU against all), r reset, h the help line,
 * q or Esc quits. */
#include "fractal.h"

#define TS        16             /* tile size */
#define LV_NONE   32             /* a tile with nothing computed */
#define PAL       1024
#define MAXSS     4
#define DEEP_ZOOM 1e12
#define MAX_ZOOM  1e28
#define SLICE_NS  30000000ull

static int PW, PH;               /* pixels */
static int TW, TH, NT;           /* tiles */
static float *nu, *nu2;          /* per pixel: smooth count, < 0 inside, -2 unknown */
static uint8_t *tlev;            /* per tile: the finest level done (16..1; 0: + anti-aliased) */
static uint8_t *tapprox;         /* per tile: holds a stretched old picture (not blocks) */
static uint8_t *tdirty;          /* per tile: to colour again */
static uint32_t *order;          /* the tiles, middle first */
static uint32_t *aa_idx, *aa_idx2;   /* per pixel: 1 + its block of extra samples, or 0 */
static float *aa_pool;
static uint32_t aa_cap, aa_used; /* blocks */
static bool aa_full;

static struct {
    bool   julia;
    dd     cx, cy;               /* the centre */
    double zoom;                 /* 1: the whole set */
    double jr, ji;               /* the Julia constant */
    int    maxit;                /* 0: automatic */
    int    ss;                   /* samples per pixel on edges: ss x ss (1: off) */
} v;

static struct kview kv;
static struct ref ref;
static uint64_t iters_by[FUN_MAX_THREADS];

/* ---- colours ------------------------------------------------------------------------------ */

static const char *const pal_names[] = { "classic", "twilight", "fire", "ocean", "electric",
                                         "forest", "rainbow", "mono" };
#define NPALS 8
static int pal_kind, dens_i = 1;
static const float dens_v[] = { 48, 96, 192, 384 };
static double pal_shift;
static uint32_t pal[PAL], inside_c;
static float nu_min, nu_min_shown;

static void make_palette(int kind)
{
    static const struct stop { double at; uint32_t c; } stops[][8] = {
        { { 0.0, 0x000764 }, { 0.16, 0x206bcb }, { 0.42, 0xedffff }, { 0.6425, 0xffaa00 },
          { 0.8575, 0x000200 }, { 1.0, 0x000764 } },
        { { 0.0, 0x0d0221 }, { 0.22, 0x3d1a78 }, { 0.45, 0xc23b8c }, { 0.66, 0xf6a55c },
          { 0.82, 0xfff0c8 }, { 1.0, 0x0d0221 } },
        { { 0.0, 0x140000 }, { 0.25, 0xc81e00 }, { 0.5, 0xffa000 }, { 0.7, 0xffff78 },
          { 0.85, 0xff7814 }, { 1.0, 0x140000 } },
        { { 0.0, 0x000a28 }, { 0.3, 0x0050a0 }, { 0.55, 0x28c8dc }, { 0.75, 0xdcffff },
          { 0.9, 0x1478a0 }, { 1.0, 0x000a28 } },
        { { 0.0, 0x000000 }, { 0.2, 0x0b1a6b }, { 0.42, 0x1ad1ff }, { 0.55, 0xffffff },
          { 0.7, 0xff2fb5 }, { 0.88, 0x3a0050 }, { 1.0, 0x000000 } },
        { { 0.0, 0x03140c }, { 0.3, 0x0f5132 }, { 0.55, 0x46a36b }, { 0.75, 0xe9d985 },
          { 0.9, 0x7a4b1e }, { 1.0, 0x03140c } },
        { { 0, 0 } },
        { { 0.0, 0x0a0a14 }, { 0.5, 0xf0f0f0 }, { 1.0, 0x0a0a14 } },
    };
    for (int i = 0; i < PAL; i++) {
        double t = (double)i / PAL;
        if (kind == 6) {   /* rainbow: three sines a third apart */
            double a = t * 6.283185307179586;
            pal[i] = rgb((uint32_t)(127.5 + 127 * sind(a)), (uint32_t)(127.5 + 127 * sind(a + 2.0944)),
                         (uint32_t)(127.5 + 127 * sind(a + 4.1888)));
            continue;
        }
        const struct stop *s = stops[kind];
        int k = 0;
        while (t > s[k + 1].at)
            k++;
        double f = (t - s[k].at) / (s[k + 1].at - s[k].at);
        f = f * f * (3 - 2 * f);
        pal[i] = mixc(s[k].c, s[k + 1].c, (uint32_t)(f * 256));
    }
    inside_c = 0x000000;
}

static inline uint32_t colour_of(float n)
{
    if (n < 0)
        return inside_c;
    float t = n - nu_min_shown;
    if (t < 0)
        t = 0;
    int idx = (int)(sqrtf_(t + 1.0f) * dens_v[dens_i] + (float)pal_shift);
    return pal[idx & (PAL - 1)];
}

/* The tiles to colour this frame (all, or the dirty ones). */
static uint32_t *clist, clen;

static void colour_item(uint32_t i, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    uint32_t t = clist[i];
    int x0 = (int)(t % (uint32_t)TW) * TS, y0 = (int)(t / (uint32_t)TW) * TS;
    int x1 = x0 + TS < PW ? x0 + TS : PW, y1 = y0 + TS < PH ? y0 + TS : PH;
    int n = v.ss * v.ss;
    for (int y = y0; y < y1; y++) {
        uint32_t *out = scr.s.px + (uint64_t)y * scr.s.stride;
        const float *src = nu + (uint64_t)y * PW;
        const uint32_t *aai = aa_idx + (uint64_t)y * PW;
        for (int x = x0; x < x1; x++) {
            uint32_t a = aai[x];
            if (!a) {
                out[x] = colour_of(src[x]);
                continue;
            }
            /* Anti-aliased: the samples' average (of squares: closer to how
             * light adds up than averaging the 0..255 values). */
            const float *s = aa_pool + (uint64_t)(a - 1) * (uint32_t)n;
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < n; k++) {
                uint32_t c = colour_of(s[k]);
                r += (c >> 16 & 0xff) * (c >> 16 & 0xff);
                g += (c >> 8 & 0xff) * (c >> 8 & 0xff);
                b += (c & 0xff) * (c & 0xff);
            }
            out[x] = rgb((uint32_t)sqrtf_((float)r / n), (uint32_t)sqrtf_((float)g / n),
                         (uint32_t)sqrtf_((float)b / n));
        }
    }
}

static void colour_tiles(bool all)
{
    clen = 0;
    for (int t = 0; t < NT; t++)
        if (all || tdirty[t]) {
            clist[clen++] = (uint32_t)t;
            tdirty[t] = 0;
        }
    if (clen)
        pool_run(colour_item, NULL, clen);
}

static void dirty_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int tx0 = x < 0 ? 0 : x / TS, ty0 = y < 0 ? 0 : y / TS;
    int tx1 = (x + w - 1) / TS, ty1 = (y + h - 1) / TS;
    for (int ty = ty0; ty <= ty1 && ty < TH; ty++)
        for (int tx = tx0; tx <= tx1 && tx < TW; tx++)
            tdirty[ty * TW + tx] = 1;
}

/* ---- the view ----------------------------------------------------------------------------- */

static double pixel_size(void) { return (v.julia ? 3.2 : 2.6) / PH / v.zoom; }

static int auto_iterations(void)
{
    if (v.maxit)
        return v.maxit;
    double z = v.zoom > 1 ? v.zoom : 1;
    int n = v.julia ? 400 + (int)(60 * log2d(z)) : 200 + (int)(90 * log2d(z));
    return n > 50000 ? 50000 : n;
}

static bool deep(void) { return v.zoom > DEEP_ZOOM; }

/* The kernels' view of v (and the reference orbit, deep). */
static void apply_view(void)
{
    kv.julia = v.julia;
    kv.cx = v.cx;
    kv.cy = v.cy;
    kv.px = pixel_size();
    kv.jr = v.jr;
    kv.ji = v.ji;
    kv.maxit = auto_iterations();
    kv.mode = !deep() ? M_DOUBLE : v.julia ? M_DD : M_PERTURB;
    if (kv.mode == M_PERTURB && !ref_build(&ref, &kv))
        kv.mode = M_DD;
}

/* ---- the passes --------------------------------------------------------------------------- */

static int pass_target;          /* the level being computed: 16..1, 0 anti-aliasing, -1 done */
static uint32_t *plist, plen, pnext, pbase;
static uint64_t view_t0, view_ns, view_iters;
static double tile_ns;           /* measured: ns a tile takes one thread, this pass */
static bool serial;              /* the self-test: one thread does it all */

static void build_list(void)
{
    plen = 0;
    for (int i = 0; i < NT; i++) {
        uint32_t t = order[i];
        if (pass_target > 0 ? tlev[t] > pass_target : tlev[t] == 1)
            plist[plen++] = t;
    }
    pnext = 0;
    tile_ns = 0;
}

static void restart_passes(void)
{
    pass_target = 16;
    view_t0 = now_ns();
    view_ns = 0;
    view_iters = 0;
    build_list();
}

static void tile_item(uint32_t i, uint32_t me, void *arg)
{
    (void)arg;
    uint32_t t = plist[pbase + i];
    int x0 = (int)(t % (uint32_t)TW) * TS, y0 = (int)(t / (uint32_t)TW) * TS;
    int w = PW - x0 < TS ? PW - x0 : TS, h = PH - y0 < TS ? PH - y0 : TS;
    double ox[TS * TS], oy[TS * TS];
    float out[TS * TS];
    uint64_t it = 0;
    int L = pass_target;
    if (L > 0) {
        int cur = tlev[t], n = 0;
        uint8_t xs[TS * TS], ys[TS * TS];
        for (int y = 0; y < h; y += L)
            for (int x = 0; x < w; x += L) {
                if (cur <= 16 && x % cur == 0 && y % cur == 0)
                    continue;   /* done by an earlier pass */
                xs[n] = (uint8_t)x;
                ys[n] = (uint8_t)y;
                ox[n] = x0 + x + 0.5 - PW / 2.0;
                oy[n] = y0 + y + 0.5 - PH / 2.0;
                n++;
            }
        if (n)
            eval_points(&kv, &ref, n, ox, oy, out, &it);
        bool apx = tapprox[t];
        for (int k = 0; k < n; k++) {
            int x = xs[k], y = ys[k];
            if (apx || L == 1) {
                nu[(uint64_t)(y0 + y) * PW + x0 + x] = out[k];
                continue;
            }
            for (int yy = y; yy < y + L && yy < h; yy++) {   /* a block, until finer passes */
                float *row = nu + (uint64_t)(y0 + yy) * PW + x0;
                for (int xx = x; xx < x + L && xx < w; xx++)
                    row[xx] = out[k];
            }
        }
        tlev[t] = (uint8_t)L;
        if (L == 1)
            tapprox[t] = 0;
    } else {
        /* Anti-aliasing: more samples where a pixel differs from a neighbour. */
        int ss = v.ss, n = ss * ss;
        float step = dens_v[dens_i];
        static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int y = 0; y < h && !aa_full; y++)
            for (int x = 0; x < w; x++) {
                int px = x0 + x, py = y0 + y;
                uint64_t p = (uint64_t)py * PW + px;
                float c = nu[p];
                bool edge = false;
                for (int k = 0; k < 4 && !edge; k++) {
                    int qx = px + d[k][0], qy = py + d[k][1];
                    if (qx < 0 || qy < 0 || qx >= PW || qy >= PH)
                        continue;
                    float o = nu[(uint64_t)qy * PW + qx];
                    if ((c < 0) != (o < 0)) {
                        edge = true;
                    } else if (c >= 0) {
                        float a = sqrtf_((c > nu_min ? c - nu_min : 0) + 1.0f);
                        float b = sqrtf_((o > nu_min ? o - nu_min : 0) + 1.0f);
                        edge = (a - b) * step > 10 || (b - a) * step > 10;
                    }
                }
                if (!edge)
                    continue;
                uint32_t blk = __atomic_fetch_add(&aa_used, 1, __ATOMIC_RELAXED);
                if (blk >= aa_cap) {
                    aa_full = true;
                    break;
                }
                for (int sy = 0; sy < ss; sy++)
                    for (int sx = 0; sx < ss; sx++) {
                        ox[sy * ss + sx] = px + (sx + 0.5) / ss - PW / 2.0;
                        oy[sy * ss + sx] = py + (sy + 0.5) / ss - PH / 2.0;
                    }
                eval_points(&kv, &ref, n, ox, oy, aa_pool + (uint64_t)blk * (uint32_t)n, &it);
                aa_idx[p] = blk + 1;
            }
        tlev[t] = 0;
    }
    tdirty[t] = 1;
    iters_by[me] += it;
}

/* The smallest count in view: at every tile's corner (all computed by the
 * first pass). */
static void find_min(void)
{
    float mn = 1e30f;
    for (int t = 0; t < NT; t++) {
        float c = nu[(uint64_t)(t / TW) * TS * PW + (uint64_t)(t % TW) * TS];
        if (c >= 0 && c < mn)
            mn = c;
    }
    if (mn < 1e30f)
        nu_min = mn;
}

/* Work on the passes until `until` (ns; at least one slice) or until
 * everything is done. */
static void work(uint64_t until)
{
    uint32_t threads = serial ? 1 : pool_threads();
    bool did = false;
    while (pass_target >= 0) {
        if (pnext >= plen) {   /* the next pass */
            if (pass_target == 16)
                find_min();
            pass_target = pass_target > 1 ? pass_target / 2
                        : pass_target == 1 && v.ss > 1 && !aa_full ? 0 : -1;
            if (pass_target < 0) {
                view_ns = now_ns() - view_t0;
                break;
            }
            build_list();
            continue;
        }
        uint64_t t0 = now_ns();
        if (did && t0 >= until)
            break;
        uint32_t left = plen - pnext, n = threads;
        if (tile_ns > 0) {
            double fit = (double)(until > t0 ? until - t0 : 0) * threads / tile_ns;
            n = fit < threads ? threads : fit > left ? left : (uint32_t)fit;
        }
        if (n > left)
            n = left;
        for (uint32_t i = 0; i < threads; i++)
            iters_by[i] = 0;
        pbase = pnext;
        if (serial)
            for (uint32_t i = 0; i < n; i++)
                tile_item(i, 0, NULL);
        else
            pool_run(tile_item, NULL, n);
        pnext += n;
        did = true;
        double per = (double)(now_ns() - t0) * threads / n;
        tile_ns = tile_ns > 0 ? (tile_ns + per) / 2 : per;
        for (uint32_t i = 0; i < threads; i++)
            view_iters += iters_by[i];
    }
}

/* How far the passes are, 0..1. */
static double progress(void)
{
    if (pass_target < 0)
        return 1;
    static const int lv[] = { 16, 8, 4, 2, 1, 0 };
    int k = 0;
    while (k < 5 && lv[k] != pass_target)
        k++;
    double per = 1.0 / (v.ss > 1 ? 6 : 5);
    return per * k + per * (plen ? (double)pnext / plen : 1);
}

static void aa_clear(void)
{
    memset(aa_idx, 0, (uint64_t)PW * PH * 4);
    aa_used = 0;
    aa_full = false;
}

/* Start the view again (a new place, a new kind of view); keep_picture:
 * show the old picture until the passes replace it (same place). */
static void reset_tiles(bool keep_picture)
{
    for (int t = 0; t < NT; t++) {
        tlev[t] = LV_NONE;
        tapprox[t] = keep_picture;
        tdirty[t] = 1;
    }
    aa_clear();
    apply_view();
    restart_passes();
}

/* ---- reusing what was computed ------------------------------------------------------------- */

static double rp_f;              /* reproject: the zoom factor */
static int rp_l;                 /* ... and the old picture's finest complete level */

/* Where new pixel x (of n) was in the old picture: the nearest point of
 * the old level-rp_l grid (points computed exactly, not stretched ones). */
static inline int old_px(int x, int n)
{
    double q = ((double)x + 0.5 - n / 2.0) / rp_f + n / 2.0;
    int i = (int)floord((q - 0.5) / rp_l + 0.5) * rp_l;
    if (i >= n)
        i -= rp_l;
    return i < 0 && q >= 0 ? 0 : i;
}

static void reproject_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int yo = old_px((int)y, PH);
    float *out = nu2 + (uint64_t)y * PW;
    if (yo < 0 || yo >= PH) {
        for (int x = 0; x < PW; x++)
            out[x] = -2;
        return;
    }
    const float *src = nu + (uint64_t)yo * PW;
    for (int x = 0; x < PW; x++) {
        int xo = old_px(x, PW);
        out[x] = xo >= 0 && xo < PW ? src[xo] : -2;
    }
}

/* The view was zoomed by f about its centre: stretch the old picture as a
 * first guess (tiles it doesn't cover start empty). */
static void reproject(double f)
{
    rp_f = f;
    rp_l = pass_target < 0 || pass_target == 0 ? 1 : pass_target == 16 ? 16 : pass_target * 2;
    pool_run(reproject_row, NULL, (uint32_t)PH);
    float *t = nu;
    nu = nu2;
    nu2 = t;
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            int x0 = tx * TS, y0 = ty * TS, x1 = x0 + TS - 1 < PW ? x0 + TS - 1 : PW - 1;
            int y1 = y0 + TS - 1 < PH ? y0 + TS - 1 : PH - 1;
            /* stretched only if the old picture was good (every 4th pixel
             * computed); a stretch of a stretch is worse than blocks */
            bool cover = rp_l <= 4 && nu[(uint64_t)y0 * PW + x0] > -2 && nu[(uint64_t)y0 * PW + x1] > -2 &&
                         nu[(uint64_t)y1 * PW + x0] > -2 && nu[(uint64_t)y1 * PW + x1] > -2;
            int k = ty * TW + tx;
            tlev[k] = LV_NONE;
            tapprox[k] = cover;
            tdirty[k] = 1;
        }
    aa_clear();
    apply_view();
    restart_passes();
}

static int sh_dx, sh_dy;

static void shift_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int ys = (int)y + sh_dy;
    float *out = nu2 + (uint64_t)y * PW;
    uint32_t *ai = aa_idx2 + (uint64_t)y * PW;
    for (int x = 0; x < PW; x++) {
        int xs = x + sh_dx;
        bool in = ys >= 0 && ys < PH && xs >= 0 && xs < PW;
        out[x] = in ? nu[(uint64_t)ys * PW + xs] : -2;
        ai[x] = in ? aa_idx[(uint64_t)ys * PW + xs] : 0;
    }
}

/* The view moved by (dx, dy) pixels (whole tiles): keep what is still on screen. */
static void shift(int dx, int dy)
{
    sh_dx = dx;
    sh_dy = dy;
    pool_run(shift_row, NULL, (uint32_t)PH);
    float *t = nu;
    nu = nu2;
    nu2 = t;
    uint32_t *a = aa_idx;
    aa_idx = aa_idx2;
    aa_idx2 = a;
    int tdx = dx / TS, tdy = dy / TS;
    uint8_t *lv = (uint8_t *)nu2, *ap = lv + NT;   /* scratch: the old nu2 is free now */
    memcpy(lv, tlev, (size_t)NT);
    memcpy(ap, tapprox, (size_t)NT);
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            int sx = tx + tdx, sy = ty + tdy, k = ty * TW + tx;
            /* the last row / column of tiles may be partial: redo those */
            bool in = sx >= 0 && sy >= 0 && sx < TW && sy < TH && (sx + 1) * TS <= PW &&
                      (sy + 1) * TS <= PH && (tx + 1) * TS <= PW && (ty + 1) * TS <= PH;
            tlev[k] = in ? lv[sy * TW + sx] : LV_NONE;
            tapprox[k] = in ? ap[sy * TW + sx] : 0;
            tdirty[k] = 1;
        }
    apply_view();
    restart_passes();
}

/* ---- the self-test ------------------------------------------------------------------------ */

static int failures;

static void check(bool ok, const char *what)
{
    say("fractal: selftest: %-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static bool view_alloc(int w, int h)
{
    PW = w;
    PH = h;
    TW = (w + TS - 1) / TS;
    TH = (h + TS - 1) / TS;
    NT = TW * TH;
    uint64_t px = (uint64_t)w * h;
    nu = big_alloc(px * 4);
    nu2 = big_alloc(px * 4);
    aa_idx = big_alloc(px * 4);
    aa_idx2 = big_alloc(px * 4);
    aa_cap = (uint32_t)((64ull << 20) / (MAXSS * MAXSS * 4));
    if (aa_cap > px)
        aa_cap = (uint32_t)px;
    aa_pool = big_alloc((uint64_t)aa_cap * MAXSS * MAXSS * 4);
    tlev = big_alloc((uint64_t)NT);
    tapprox = big_alloc((uint64_t)NT);
    tdirty = big_alloc((uint64_t)NT);
    order = big_alloc((uint64_t)NT * 4);
    plist = big_alloc((uint64_t)NT * 4);
    clist = big_alloc((uint64_t)NT * 4);
    if (!nu || !nu2 || !aa_idx || !aa_idx2 || !aa_pool || !tlev || !tapprox || !tdirty ||
        !order || !plist || !clist)
        return false;
    /* The middle first: tiles by distance from the centre (a counting sort). */
    int maxd = TW + TH, n = 0;
    for (int d = 0; d <= maxd; d++)
        for (int ty = 0; ty < TH; ty++)
            for (int tx = 0; tx < TW; tx++) {
                int dx = 2 * tx + 1 - TW, dy = 2 * ty + 1 - TH;
                if ((int)sqrtd((double)dx * dx + (double)dy * dy) / 2 == d)
                    order[n++] = (uint32_t)(ty * TW + tx);
            }
    return n == NT;
}

/* Render the whole view, all passes, on the pool (or one thread). */
static uint64_t render_all(bool parallel)
{
    reset_tiles(false);
    serial = !parallel;
    work(~0ull);
    serial = false;
    return view_iters;
}

/* An n x n grid of points around the centre in two modes: how many agree
 * (both in, or both out within 0.05 of an iteration); the worst difference
 * among the rest; how many different values mode a gave. */
static int compare_modes(int ma, int mb, int n, int maxit, double *worst, int *distinct_a)
{
    struct kview a = kv, b = kv;
    a.mode = ma;
    b.mode = mb;
    a.maxit = b.maxit = maxit;
    a.avx2 = b.avx2 = false;
    if (ma == M_PERTURB || mb == M_PERTURB)
        ref_build(&ref, &a);
    static float seen[64 * 64];
    int same = 0, distinct = 0;
    *worst = 0;
    for (int j = 0; j < n; j++) {
        double ox[64], oy[64];
        float ra[64], rb[64];
        uint64_t it = 0;
        for (int i = 0; i < n; i++) {
            ox[i] = (i - n / 2) * 7.3;   /* spread over a few hundred pixels */
            oy[i] = (j - n / 2) * 7.3;
        }
        eval_points(&a, &ref, n, ox, oy, ra, &it);
        eval_points(&b, &ref, n, ox, oy, rb, &it);
        for (int i = 0; i < n; i++) {
            /* the same, or escaping at the same step within 0.05 of a step */
            double d = ra[i] > rb[i] ? ra[i] - rb[i] : rb[i] - ra[i];
            if ((ra[i] < 0 && rb[i] < 0) || (ra[i] >= 0 && rb[i] >= 0 && d < 0.05))
                same++;
            else if (ra[i] >= 0 && rb[i] >= 0 && d > *worst)
                *worst = d;
            bool dup = false;
            for (int k = 0; k < distinct && !dup; k++)
                dup = seen[k] == ra[i];
            if (!dup)
                seen[distinct++] = ra[i];
        }
    }
    *distinct_a = distinct;
    return same;
}

/* The tour's target: a Misiurewicz point (preperiod 24, period 1) near
 * -0.77568377 + 0.13646737i, a spiral at every scale (found by Newton's
 * method to 60 digits; here to double-double's 32). */
static const dd tour_x = { -0.7756837680090538, 1.1497740127108711e-17 };
static const dd tour_y = { 0.1364673682946901, 1.3525418034032015e-17 };

static int selftest(void)
{
    uint32_t n = pool_start(0);
    bool avx2 = fun_has_avx2();
    say("fractal: selftest on %u threads (%u CPUs by CPUID), AVX2 %s\n", n, fun_cpu_count(),
        avx2 ? "yes" : "no");
    uint64_t it = 0;
    char what[128], s1[64];

    /* Known points. */
    check(it_double(0, 0, false, 0, 0, 1000, &it) < 0 && it_double(-1, 0, false, 0, 0, 1000, &it) < 0 &&
              it_double(-2, 0, false, 0, 0, 1000, &it) < 0 &&
              it_double(0.25, 0, false, 0, 0, 1000, &it) < 0,
          "0, -1, -2, 1/4 are in the set");
    float e1 = it_double(1, 0, false, 0, 0, 1000, &it), e2 = it_double(0.26, 0, false, 0, 0, 1000, &it);
    check(e1 >= 0 && e1 < 6 && e2 > 10 && e2 < 100, "1 escapes at once, 0.26 after a while");
    check(it_double(-0.75, 0.1, false, 0, 0, 5000, &it) > 20,
          "-0.75 + 0.1i (near the neck) escapes slowly");
    check(it_double(0, 0, true, -1, 0, 2000, &it) < 0 && it_double(1.5, 1.5, true, -1, 0, 2000, &it) >= 0,
          "Julia c = -1 (the basilica): 0 stays, 1.5 + 1.5i escapes");

    /* The 4-lane kernel gives exactly the plain one's answers. */
    if (avx2) {
        bool same = true;
        uint64_t s = 99, i1 = 0, i2 = 0;
        for (int k = 0; k < 4000; k += 4) {
            double pr[4], pi[4];
            float o4[4];
            bool jul = k >= 2000;
            for (int l = 0; l < 4; l++) {
                pr[l] = (double)(rng_next(&s) % 100000) / 100000 * 3 - 2.2;
                pi[l] = (double)(rng_next(&s) % 100000) / 100000 * 2.4 - 1.2;
            }
            it_double4(pr, pi, jul, -0.8, 0.156, 3000, o4, &i1);
            for (int l = 0; l < 4; l++)
                same &= it_double(pr[l], pi[l], jul, -0.8, 0.156, 3000, &i2) == o4[l];
        }
        check(same && i1 == i2, "AVX2 kernel == plain kernel (4000 points, bit for bit)");
    } else {
        say("fractal: selftest: (no AVX2 here: the 4-lane kernel isn't tested)\n");
    }

    /* Double-double arithmetic. */
    dd third = dd_div(dd_d(1), dd_d(3)), one = dd_add_d(dd_mul_d(third, 3), -1);
    dd sq = dd_sqr(dd_add_d(dd_d(1), 0x1p-40));   /* (1 + 2^-40)^2 = 1 + 2^-39 + 2^-80 */
    dd rest = dd_add_d(dd_add_d(sq, -1), -0x1p-39);
    dd tenth = dd_div(dd_d(1), dd_d(10)), back = dd_add_d(dd_mul_d(tenth, 10), -1);
    snprintf(what, sizeof(what), "double-double: 3/3 - 1 = %d e-33, 10/10 - 1 = %d e-33, 2^-80 kept",
             (int)(one.hi * 1e33), (int)(back.hi * 1e33));
    check(one.hi < 1e-31 && one.hi > -1e-31 && back.hi < 1e-31 && back.hi > -1e-31 &&
              rest.hi == 0x1p-80,
          what);
    dd_str(s1, sizeof(s1), tour_x, 30);
    snprintf(what, sizeof(what), "double-double prints %s", s1);
    check(!strncmp(s1, "-0.7756837680090537974694835039", 31), what);

    /* The three kernels against each other, where both are valid. */
    double worst;
    int distinct, dist_d;
    PH = 1440;
    v.julia = false;
    v.cx = dd_d(-0.743643887037151);
    v.cy = dd_d(0.131825904205330);
    v.zoom = 1e6;
    v.maxit = 0;
    apply_view();
    int same = compare_modes(M_DOUBLE, M_DD, 40, 4000, &worst, &distinct);
    snprintf(what, sizeof(what), "zoom 1e6: double == double-double at %d of 1600 points", same);
    check(same >= 1600 * 97 / 100, what);
    same = compare_modes(M_DOUBLE, M_PERTURB, 40, 4000, &worst, &distinct);
    snprintf(what, sizeof(what), "zoom 1e6: double == perturbation at %d of 1600 points", same);
    check(same >= 1600 * 97 / 100, what);
    v.julia = true;
    v.jr = -0.8;
    v.ji = 0.156;
    v.cx = dd_d(0.3);
    v.cy = dd_d(0.1);
    v.zoom = 1e5;
    apply_view();
    same = compare_modes(M_DOUBLE, M_DD, 40, 3000, &worst, &distinct);
    snprintf(what, sizeof(what), "Julia, zoom 1e5: double == double-double at %d of 1600", same);
    check(same >= 1600 * 97 / 100, what);
    /* Deep: where double can't go, perturbation and double-double agree. */
    v.julia = false;
    v.cx = tour_x;
    v.cy = tour_y;
    v.zoom = 1e20;
    apply_view();
    uint64_t t0 = now_ns();
    same = compare_modes(M_PERTURB, M_DD, 24, 6000, &worst, &distinct);
    uint64_t ms = (now_ns() - t0) / 1000000;
    compare_modes(M_DOUBLE, M_DOUBLE, 24, 6000, &worst, &dist_d);
    snprintf(what, sizeof(what), "zoom 1e20: perturbation == double-double at %d of 576 (%lu ms)",
             same, (unsigned long)ms);
    check(same >= 576 * 95 / 100, what);
    snprintf(what, sizeof(what), "zoom 1e20: %d different values in double-double, %d in double",
             distinct, dist_d);
    check(distinct > 300 && dist_d < distinct / 4, what);
    v.zoom = 1e27;
    apply_view();
    same = compare_modes(M_PERTURB, M_DD, 24, 9000, &worst, &distinct);
    snprintf(what, sizeof(what), "zoom 1e27: perturbation == double-double at %d of 576 (%d values)",
             same, distinct);
    check(same >= 576 * 90 / 100 && distinct > 300, what);

    /* All CPUs render exactly what one does (all passes, anti-aliasing too). */
    if (!view_alloc(160, 96))
        return 1;
    v.julia = false;
    v.cx = dd_d(-0.5);
    v.cy = dd_d(0);
    v.zoom = 1;
    v.ss = 2;
    v.maxit = 500;
    kv.avx2 = avx2;
    render_all(true);
    uint64_t bytes = (uint64_t)PW * PH * 4;
    float *par = big_alloc(bytes);
    memcpy(par, nu, bytes);
    uint32_t aa_par = aa_used;
    render_all(false);
    snprintf(what, sizeof(what), "render: all CPUs == one CPU (160 x 96, all passes, %u AA)", aa_used);
    check(!memcmp(par, nu, bytes) && aa_par == aa_used && aa_used > 100, what);
    bool sym = true;
    for (int y = 0; y < PH / 2; y++)
        sym &= !memcmp(nu + (uint64_t)y * PW, nu + (uint64_t)(PH - 1 - y) * PW, (size_t)PW * 4);
    check(sym, "render: the set is mirror-symmetric about the real axis");
    uint32_t inside = 0;
    for (uint64_t i = 0; i < (uint64_t)PW * PH; i++)
        inside += nu[i] < 0;
    uint32_t pct = inside * 100 / (uint32_t)(PW * PH);
    snprintf(what, sizeof(what), "render: %u%% of the full view is inside (expect 10-20)", pct);
    check(pct >= 10 && pct <= 20, what);
    /* Panning keeps what it can: the moved picture == a fresh one. */
    v.ss = 1;
    render_all(true);
    v.cx = dd_add_d(v.cx, 32 * kv.px);
    v.cy = dd_add_d(v.cy, 16 * kv.px);
    shift(32, 16);
    work(~0ull);
    memcpy(par, nu, bytes);
    render_all(true);
    uint32_t agree = 0;   /* (not bit for bit: the centre moved by a rounded amount) */
    for (uint64_t i = 0; i < (uint64_t)PW * PH; i++) {
        float d = par[i] - nu[i];
        agree += (par[i] < 0 && nu[i] < 0) || (d < 1e-3f && d > -1e-3f);
    }
    snprintf(what, sizeof(what), "render: a panned picture == the view drawn afresh (%u%%)",
             agree * 100 / (uint32_t)(PW * PH));
    check(agree >= (uint32_t)(PW * PH) * 99 / 100, what);

    /* How much faster all CPUs are (information only). */
    if (!view_alloc(320, 176))
        return 1;
    v.cx = dd_d(-0.743643887037151);
    v.cy = dd_d(0.131825904205330);
    v.zoom = 2000;
    v.maxit = 2000;
    kv.avx2 = false;
    t0 = now_ns();
    uint64_t iters = render_all(false);
    uint64_t one_ns = now_ns() - t0;
    t0 = now_ns();
    render_all(true);
    uint64_t all = now_ns() - t0, all4 = 0;
    if (avx2) {
        kv.avx2 = true;
        t0 = now_ns();
        render_all(true);
        all4 = now_ns() - t0;
    }
    uint64_t x10 = all ? one_ns * 10 / all : 0;
    say("fractal: selftest: seahorse valley 320x176, %lu M iterations: %lu ms on 1 CPU, %lu ms "
        "on %u threads (%lu.%lux)",
        (unsigned long)(iters / 1000000), (unsigned long)(one_ns / 1000000),
        (unsigned long)(all / 1000000), n, (unsigned long)(x10 / 10), (unsigned long)(x10 % 10));
    if (avx2)
        say(", %lu ms with AVX2\n", (unsigned long)(all4 / 1000000));
    else
        say("\n");

    say("fractal: selftest %s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ---- exploring ---------------------------------------------------------------------------- */

static const struct { const char *name; double cx, cy, zoom; } places[] = {
    { "the whole set", -0.6, 0, 1 },
    { "seahorse valley", -0.743643887037151, 0.131825904205330, 3000 },
    { "elephant valley", 0.2925, 0.0164, 60 },
    { "a mini Mandelbrot", -1.7548776662466927, 0, 90 },
    { "spiral", -0.7746806106269039, -0.1374168856037867, 8000 },
    { "dendrite at c = i", 0, 1, 20 },
};

static void go(double cx, double cy, double zoom)
{
    v.julia = false;
    v.cx = dd_d(cx);
    v.cy = dd_d(cy);
    v.zoom = zoom;
}

/* "1.2e15" (or a plain number when small) */
static char *fmt_sci(char *buf, size_t size, double x)
{
    int e = 0;
    while (x >= 10) {
        x /= 10;
        e++;
    }
    if (e < 4)
        snprintf(buf, size, "%d", (int)(x * (e == 0 ? 1 : e == 1 ? 10 : e == 2 ? 100 : 1000) + 0.5));
    else
        snprintf(buf, size, "%d.%de%d", (int)x, (int)((x - (int)x) * 10), e);
    return buf;
}

static char note[200];
static bool show_help = true;
static struct fps fps;
static int hud_last[2][4];       /* the HUD's rectangles last frame */

/* The HUD: a panel at the top left, the help line at the bottom. With
 * draw false, only marks the tiles under it (now and last frame) to be
 * coloured again. */
static void hud(bool draw, bool touring, bool cycling)
{
    const struct surf *s = &scr.s;
    int u = scr.ui, pad = 10 * u;
    char l1[160], l2[160], l3[220], l4[220], a[64], b[64], z[32];
    int digits = 8;
    for (double zz = v.zoom; zz >= 10 && digits < 34; zz /= 10)
        digits++;
    if (v.julia)
        snprintf(l1, sizeof(l1), "Julia set    c = %s %si", dd_str(a, sizeof(a), dd_d(v.jr), 10),
                 dd_str(b, sizeof(b), dd_d(v.ji), 10));
    else
        snprintf(l1, sizeof(l1), "Mandelbrot set");
    snprintf(l2, sizeof(l2), "\a%s  %si\a", dd_str(a, sizeof(a), v.cx, digits),
             dd_str(b, sizeof(b), v.cy, digits));
    snprintf(l3, sizeof(l3),
             "zoom \a%sx\a    \a%d\a iterations%s    anti-aliasing \a%s\a    colours \a%s\a%s%s",
             fmt_sci(z, sizeof(z), v.zoom), kv.maxit, v.maxit ? "" : " (auto)",
             v.ss > 1 ? (v.ss == 2 ? "2x2" : v.ss == 3 ? "3x3" : "4x4") : "off", pal_names[pal_kind],
             cycling ? " (cycling)" : "", touring ? "    \aTOUR\a" : "");
    uint64_t ns = pass_target < 0 ? view_ns : now_ns() - view_t0;
    uint64_t mips = ns ? view_iters * 1000 / ns : 0;
    const char *kern = kv.mode == M_PERTURB ? "perturbation" : kv.mode == M_DD ? "double-double"
                     : kv.avx2 ? "AVX2" : "double";
    if (pass_target >= 0)
        snprintf(l4, sizeof(l4), "rendering \a%d%%\a    \a%s\a M iterations/s on \a%u\a CPUs (%s)    %u.%u fps",
                 (int)(progress() * 100), commas(a, sizeof(a), mips), pool_threads(), kern,
                 fps.x10 / 10, fps.x10 % 10);
    else
        snprintf(l4, sizeof(l4), "done in \a%lu.%02lu s\a:  %s M iterations,  \a%s\a M/s on \a%u\a CPUs (%s)",
                 (unsigned long)(ns / 1000000000), (unsigned long)(ns / 10000000 % 100),
                 commas(a, sizeof(a), view_iters / 1000000), commas(b, sizeof(b), mips),
                 pool_threads(), kern);
    int w = text_width(2 * u, "FRACTAL") + pad + text_width(u, l1);
    const char *lines[] = { l2, l3, l4, note };
    for (int i = 0; i < 4; i++) {
        int lw = text_width(u, lines[i]);
        w = lw > w ? lw : w;
    }
    int lh = TEXT_H(u) + 4 * u, badge = text_width(u, "DEEP") + 2 * pad;
    int ph = TEXT_H(2 * u) + pad / 2 + lh * (note[0] ? 4 : 3) + pad * 2;
    int px = pad, py = pad, pw = w + 2 * pad + (deep() ? badge + pad : 0);
    const char *help = "arrows move    + - zoom    j Julia    c colours    d density    space cycle"
                       "    a anti-alias    [ ] iterations    0 auto    1-6 places    t tour"
                       "    b benchmark    r reset    h help    q quit";
    int hw = text_width(u, help) + 2 * pad, hh = TEXT_H(u) + pad;
    if (!draw) {
        for (int i = 0; i < 2; i++)
            dirty_rect(hud_last[i][0], hud_last[i][1], hud_last[i][2], hud_last[i][3]);
        int now[2][4] = { { px, 0, pw + 8 * u, py + ph + 8 * u },
                          { (s->w - hw) / 2, s->h - hh - pad, show_help ? hw : 0, hh } };
        memcpy(hud_last, now, sizeof(now));
        for (int i = 0; i < 2; i++)
            dirty_rect(now[i][0], now[i][1], now[i][2], now[i][3]);
        dirty_rect(0, 0, s->w, 3 * u);
        return;
    }
    /* progress: a thin line along the top while the passes run */
    if (pass_target >= 0)
        fill(s, 0, 0, (int)(s->w * progress()), 2 * u, 0xffd060);
    panel(s, px, py, pw, ph, 8 * u, 0x000000, 165);
    int x = text_shadow(s, px + pad, py + pad, 2 * u, 0xffd060, "FRACTAL");
    text(s, x + pad, py + pad + TEXT_H(2 * u) - TEXT_H(u) - 2 * u, u, 0xe8ecff, l1);
    if (deep()) {
        int bx = px + pw - badge - pad;
        panel(s, bx, py + pad, badge, TEXT_H(u) + pad / 2, 5 * u, 0xd03060, 256);
        text(s, bx + pad, py + pad + pad / 4, u, 0xffffff, "DEEP");
    }
    int y = py + pad + TEXT_H(2 * u) + pad / 2;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, l2);
    y += lh;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, l3);
    y += lh;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, l4);
    y += lh;
    if (note[0])
        text(s, px + pad, y, u, 0xffe070, note);
    if (show_help) {
        panel(s, (s->w - hw) / 2, s->h - hh - pad, hw, hh, 6 * u, 0x000000, 160);
        text(s, (s->w - hw) / 2 + pad, s->h - hh - pad + pad / 2, u, 0xb0b8d8, help);
    }
}

/* b: the view's points on a grid (every 4th pixel each way), 1 CPU then all. */
static void bench_row(uint32_t j, uint32_t me, void *arg)
{
    (void)arg;
    double ox[64], oy[64];
    float out[64];
    uint64_t it = 0;
    int n = 0;
    for (int x = 0; x < PW; x += 4) {
        ox[n] = x + 0.5 - PW / 2.0;
        oy[n] = (double)j * 4 + 0.5 - PH / 2.0;
        if (++n == 64 || x + 4 >= PW) {
            eval_points(&kv, &ref, n, ox, oy, out, &it);
            n = 0;
        }
    }
    iters_by[me] += it;
}

static void benchmark(void)
{
    uint32_t rows = (uint32_t)PH / 4;
    for (int i = 0; i < FUN_MAX_THREADS; i++)
        iters_by[i] = 0;
    uint64_t t0 = now_ns();
    for (uint32_t j = 0; j < rows; j++)
        bench_row(j, 0, NULL);
    uint64_t one = now_ns() - t0, iters = iters_by[0];
    t0 = now_ns();
    pool_run(bench_row, NULL, rows);
    uint64_t all = now_ns() - t0, x10 = all ? one * 10 / all : 0;
    char a[32], b[32], c[32];
    snprintf(note, sizeof(note),
             "benchmark, %d x %d points, %s M iterations:  1 CPU %lu ms (%s M/s),  %u CPUs %lu ms "
             "(%s M/s):  %lu.%lux",
             PW / 4, PH / 4, commas(a, sizeof(a), iters / 1000000), (unsigned long)(one / 1000000),
             commas(b, sizeof(b), one ? iters * 1000 / one : 0), pool_threads(),
             (unsigned long)(all / 1000000), commas(c, sizeof(c), all ? iters * 1000 / all : 0),
             (unsigned long)(x10 / 10), (unsigned long)(x10 % 10));
    printf("fractal: %s\n", note);   /* the kernel log */
}

static void reset(void)
{
    go(places[0].cx, places[0].cy, 1);
    reset_tiles(false);
}

static int play(int argc, char **argv)
{
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    status_t st = gfx_open();
    if (st != OK) {
        say("fractal: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    if (!view_alloc(scr.w, scr.h)) {
        gfx_close();
        say("fractal: out of memory\n");
        return 1;
    }
    /* AVX2 when the CPU has it (not under QEMU's emulator by default: it
     * emulates AVX2 more slowly than plain SSE2; `avx` forces it). */
    kv.avx2 = fun_has_avx2() && !has_arg(argc, argv, "noavx") &&
              (!fun_is_tcg() || has_arg(argc, argv, "avx"));
    make_palette(pal_kind);
    v.ss = pool_threads() >= 16 ? 3 : 2;   /* more samples when there are CPUs to spare */
    reset();
    double saved_zoom = 1;
    dd saved_cx = v.cx, saved_cy = v.cy;
    bool quit = false, cycling = false, touring = false, recolour = true;
    uint64_t renders = 0, last_frame = 0, tour_step_at = 0, frames = 0;
    const int step_px = ((PH / 8 + TS / 2) / TS) * TS;   /* an arrow: 1/8 of the height, whole tiles */
    while (!quit) {
        uint64_t t0 = now_ns();
        bool busy = pass_target >= 0 || touring;
        /* Keys: whatever came; wait for one only when there's nothing to do. */
        int k = gfx_key(busy ? 0 : cycling ? last_frame + 16666667 : DEADLINE_NEVER);
        for (; k != KEY_NONE && !quit; k = gfx_key(0)) {
            note[0] = '\0';
            switch (k) {
            case KEY_QUIT: case 'q': case 'Q': quit = true; break;
            case KEY_LEFT:  v.cx = dd_add_d(v.cx, -step_px * kv.px); shift(-step_px, 0); break;
            case KEY_RIGHT: v.cx = dd_add_d(v.cx, step_px * kv.px);  shift(step_px, 0); break;
            case KEY_UP:    v.cy = dd_add_d(v.cy, -step_px * kv.px); shift(0, -step_px); break;
            case KEY_DOWN:  v.cy = dd_add_d(v.cy, step_px * kv.px);  shift(0, step_px); break;
            case '+': case '=': case KEY_PGUP: case KEY_ENTER:
                if (v.zoom * 2 <= MAX_ZOOM) {
                    v.zoom *= 2;
                    reproject(2);
                } else {
                    snprintf(note, sizeof(note), "1e28x is as deep as double-double goes");
                }
                break;
            case '-': case '_': case KEY_PGDN:
                if (v.zoom > 0.25) {
                    v.zoom /= 2;
                    reproject(0.5);
                }
                break;
            case 'j': case 'J':
                if (!v.julia) {
                    saved_cx = v.cx, saved_cy = v.cy, saved_zoom = v.zoom;
                    v.jr = v.cx.hi + v.cx.lo;
                    v.ji = v.cy.hi + v.cy.lo;
                    v.cx = v.cy = dd_d(0);
                    v.zoom = 1;
                    v.julia = true;
                } else {
                    v.julia = false;
                    v.cx = saved_cx, v.cy = saved_cy, v.zoom = saved_zoom;
                }
                touring = false;
                reset_tiles(false);
                break;
            case 'c': case 'C':
                pal_kind = (pal_kind + 1) % NPALS;
                make_palette(pal_kind);
                snprintf(note, sizeof(note), "colours: %s", pal_names[pal_kind]);
                recolour = true;
                break;
            case 'd': case 'D':
                dens_i = (dens_i + 1) % 4;
                snprintf(note, sizeof(note), "colour density %d", (int)dens_v[dens_i]);
                recolour = true;
                break;
            case ' ': cycling = !cycling; break;
            case 'a': case 'A':
                v.ss = v.ss % MAXSS + 1;
                aa_clear();
                for (int t = 0; t < NT; t++)
                    if (tlev[t] == 0)
                        tlev[t] = 1;
                if (pass_target <= 0) {   /* (else it comes after the passes still going) */
                    pass_target = v.ss > 1 ? 0 : -1;
                    if (pass_target == 0) {
                        view_t0 = now_ns();
                        build_list();
                    }
                }
                recolour = true;
                break;
            case '[': case ']': case '0':
                v.maxit = k == '0' ? 0 : (v.maxit ? v.maxit : kv.maxit) * (k == '[' ? 1 : 4) / 2;
                if (k != '0' && v.maxit < 50)
                    v.maxit = 50;
                if (v.maxit > 200000)
                    v.maxit = 200000;
                reset_tiles(true);   /* same place: keep showing it while it's redone */
                break;
            case 't': case 'T':
                touring = !touring;
                if (touring && !v.julia && v.zoom < 2) {   /* somewhere worth diving into */
                    v.cx = tour_x;
                    v.cy = tour_y;
                    v.zoom = 1;
                    reset_tiles(false);
                }
                snprintf(note, sizeof(note), touring ? "tour: zooming in, down to 1e28x (t stops it)"
                                                     : "tour stopped");
                break;
            case 'r': case 'R': case KEY_HOME: touring = false; reset(); break;
            case 'b': case 'B': benchmark(); break;
            case 'h': case 'H': show_help = !show_help; break;
            default:
                if (k >= '1' && k <= '6') {
                    int p = k - '1';
                    touring = false;
                    go(places[p].cx, places[p].cy, places[p].zoom);
                    reset_tiles(false);
                    snprintf(note, sizeof(note), "%d: %s", p + 1, places[p].name);
                }
            }
        }
        if (quit)
            break;
        /* The tour: one step in once the picture is good enough (every
         * other pixel computed; every fourth when deep, where it's slower). */
        if (touring) {
            int good = deep() ? 4 : 2;
            if (pass_target < 0 || (pass_target < good && t0 - tour_step_at > 40000000ull)) {
                double f = deep() ? 1.08 : 1.04;
                if (v.zoom * f > MAX_ZOOM) {
                    touring = false;
                    snprintf(note, sizeof(note), "tour: the end of double-double (1e28x)");
                } else {
                    v.zoom *= f;
                    reproject(f);
                    tour_step_at = t0;
                }
            }
        }
        bool was_busy = pass_target >= 0;
        if (pass_target >= 0)
            work(t0 + (touring ? SLICE_NS / 2 : SLICE_NS));
        if (was_busy && pass_target < 0)
            renders++;
        /* Show it. */
        if (touring) {
            nu_min_shown += (nu_min - nu_min_shown) * 0.25f;
            recolour = true;
        } else if (nu_min_shown != nu_min) {
            nu_min_shown = nu_min;
            recolour = true;
        }
        if (cycling) {
            pal_shift += 4;
            recolour = true;
        }
        hud(false, touring, cycling);
        colour_tiles(recolour);
        recolour = false;
        fps_frame(&fps);
        hud(true, touring, cycling);
        gfx_present();
        frames++;
        last_frame = t0;
    }
    gfx_close();
    char z[32];
    say("fractal: %lu renders on %u threads (the last %lu ms), %lu frames, zoom %sx; %lu MB to "
        "the screen\n",
        (unsigned long)renders, pool_threads(), (unsigned long)(view_ns / 1000000),
        (unsigned long)frames, fmt_sci(z, sizeof(z), v.zoom), (unsigned long)(scr.bytes >> 20));
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    return play(argc, argv);
}
