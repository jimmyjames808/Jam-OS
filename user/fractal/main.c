/* fractal: an interactive Mandelbrot / Julia explorer drawn by every CPU.
 *
 *   run fractal [threads=N]    explore
 *   run fractal --selftest     check the maths and the parallel render, exit 0 if right
 *
 * The picture is (screen columns) x (2 x screen rows) square pixels in the
 * console's alternate screen, each an average of up to 4 x 4 samples
 * (anti-aliasing). Rows of pixels are handed to the pool's threads (one per
 * CPU) one at a time, so fast rows (outside the set) and slow rows (along
 * its edge) even out over the CPUs. Every sample's smooth iteration count
 * is kept, so recolouring (palettes, colour cycling) doesn't iterate again.
 *
 * The console has 16 colours, so each text cell becomes whichever is
 * closer to its two pixels: an upper half block (two of the 16 colours) or
 * a 25 / 50 / 75 % shade of one colour over another (~800 mixes, looked up
 * in a 4096-entry table built at start).
 *
 * Keys: arrows move, + / - zoom (also Page Up / Page Down), j Julia set of
 * the point in the middle (j again: back), c colours, space colour cycling,
 * a anti-aliasing, [ ] fewer / more iterations (0: automatic), 1-6 famous
 * places, t tour (keeps zooming in), b benchmark (1 CPU against all), r
 * reset, q or Esc quits. */
#include "../fun/fun.h"

#define MAXSS 4
#define PAL 1024

static struct term T;
static uint32_t PW, PH;          /* pixels: cols x 2 * (rows - 2) */
static float *nu;                /* PH * PW * ss * ss smooth iteration counts; < 0 inside */
static uint32_t *img;            /* PH * PW averaged colours (0xRRGGBB) */

static struct {
    bool   julia;
    double cx, cy;               /* the centre */
    double zoom;                 /* 1: the whole set */
    double jr, ji;               /* the Julia constant */
    int    maxit;                /* 0: automatic */
    int    ss;                   /* samples per pixel: ss x ss */
} v;

static int      it_used;         /* the iteration limit of the last render */
static int      pal_kind;
static double   pal_shift;
static uint32_t pal[PAL];
static uint64_t iters_by[FUN_MAX_THREADS];

/* ---- a little maths (no libm) ------------------------------------------------------- */

static double log2d(double x)   /* x > 0 */
{
    union { double d; uint64_t u; } u = { x };
    int e = (int)((u.u >> 52) & 0x7ff) - 1023;
    u.u = (u.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m = u.d, t = (m - 1) / (m + 1), t2 = t * t;
    double ln = 2 * t * (1 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 * (1.0 / 7 + t2 / 9))));
    return e + ln * 1.4426950408889634;
}

static double sind(double x)
{
    const double pi = 3.141592653589793, tau = 2 * pi;
    x -= tau * (double)(int64_t)(x / tau);
    if (x > pi)
        x -= tau;
    if (x < -pi)
        x += tau;
    double x2 = x * x, term = x, sum = x;
    for (int k = 1; k < 10; k++) {
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

/* "-0.7436438870" with n decimals. */
static char *fmt_fixed(char *buf, size_t size, double x, int n)
{
    bool neg = x < 0;
    if (neg)
        x = -x;
    uint64_t scale = 1;
    for (int i = 0; i < n; i++)
        scale *= 10;
    uint64_t whole = (uint64_t)x, frac = (uint64_t)((x - (double)whole) * (double)scale + 0.5);
    if (frac >= scale) {
        whole++;
        frac -= scale;
    }
    snprintf(buf, size, "%s%lu.%0*lu", neg ? "-" : "+", (unsigned long)whole, n,
             (unsigned long)frac);
    return buf;
}

/* "1.2e5" */
static char *fmt_sci(char *buf, size_t size, double x)
{
    int e = 0;
    while (x >= 10) {
        x /= 10;
        e++;
    }
    int whole = (int)x, tenth = (int)((x - whole) * 10);
    if (e < 3)
        snprintf(buf, size, "%d", (int)(x * (e == 0 ? 1 : e == 1 ? 10 : 100) + 0.5));
    else
        snprintf(buf, size, "%d.%de%d", whole, tenth, e);
    return buf;
}

/* ---- the iteration --------------------------------------------------------------------- */

/* The smooth iteration count of one point, or -1 if it stays bounded. */
static float escape(double pr, double pi, int maxit, uint64_t *iters)
{
    double zr, zi, kr, ki;
    if (v.julia) {
        zr = pr, zi = pi, kr = v.jr, ki = v.ji;
    } else {
        zr = 0, zi = 0, kr = pr, ki = pi;
        /* The main cardioid and the period-2 bulb never escape. */
        double q = (pr - 0.25) * (pr - 0.25) + pi * pi;
        if (q * (q + (pr - 0.25)) < 0.25 * pi * pi || (pr + 1) * (pr + 1) + pi * pi < 0.0625)
            return -1;
    }
    double r2 = zr * zr, i2 = zi * zi, sr = zr, si = zi;
    int i = 0, check = 8;
    while (i < maxit && r2 + i2 < 256.0) {
        zi = 2 * zr * zi + ki;
        zr = r2 - i2 + kr;
        r2 = zr * zr;
        i2 = zi * zi;
        i++;
        if (i == check) {   /* a cycle (checked at doubling steps): inside */
            if ((zr - sr) * (zr - sr) + (zi - si) * (zi - si) < 1e-28) {
                *iters += (uint64_t)i;
                return -1;
            }
            sr = zr;
            si = zi;
            check *= 2;
        }
    }
    *iters += (uint64_t)i;
    if (i >= maxit)
        return -1;
    return (float)(i + 1 - log2d(log2d(r2 + i2) * 0.5));
}

static int auto_iterations(void)
{
    if (v.maxit)
        return v.maxit;
    if (v.julia)
        return 400;
    int n = 200 + (int)(90 * log2d(v.zoom > 1 ? v.zoom : 1));
    return n > 50000 ? 50000 : n;
}

static double pixel_size(void)
{
    return (v.julia ? 3.2 : 2.6) / PH / v.zoom;
}

static void render_row(uint32_t y, uint32_t me, void *arg)
{
    (void)arg;
    double px = pixel_size(), inv = 1.0 / v.ss;
    uint64_t iters = 0;
    int ss = v.ss;
    float *out = nu + (uint64_t)y * PW * ss * ss;
    for (uint32_t x = 0; x < PW; x++)
        for (int sy = 0; sy < ss; sy++)
            for (int sx = 0; sx < ss; sx++) {
                double ox = (sx + 0.5) * inv - 0.5, oy = (sy + 0.5) * inv - 0.5;
                double pr = v.cx + ((double)x - PW / 2.0 + 0.5 + ox) * px;
                double pi = v.cy + ((double)y - PH / 2.0 + 0.5 + oy) * px;
                *out++ = escape(pr, pi, it_used, &iters);
            }
    iters_by[me] += iters;
}

/* Render the view on the pool (or one thread); returns the iterations done. */
static uint64_t render(bool parallel)
{
    it_used = auto_iterations();
    for (int i = 0; i < FUN_MAX_THREADS; i++)
        iters_by[i] = 0;
    if (parallel)
        pool_run(render_row, NULL, PH);
    else
        for (uint32_t y = 0; y < PH; y++)
            render_row(y, 0, NULL);
    uint64_t n = 0;
    for (int i = 0; i < FUN_MAX_THREADS; i++)
        n += iters_by[i];
    return n;
}

/* ---- colours ------------------------------------------------------------------------------ */

static const char *const pal_names[] = { "classic", "fire", "ocean", "rainbow", "mono" };
#define NPALS 5

static void make_palette(int kind)
{
    static const struct stop { double at; uint8_t r, g, b; } stops[][7] = {
        { { 0.0, 0, 7, 100 }, { 0.16, 32, 107, 203 }, { 0.42, 237, 255, 255 },
          { 0.6425, 255, 170, 0 }, { 0.8575, 0, 2, 0 }, { 1.0, 0, 7, 100 } },
        { { 0.0, 20, 0, 0 }, { 0.25, 200, 30, 0 }, { 0.5, 255, 160, 0 }, { 0.7, 255, 255, 120 },
          { 0.85, 255, 120, 20 }, { 1.0, 20, 0, 0 } },
        { { 0.0, 0, 10, 40 }, { 0.3, 0, 80, 160 }, { 0.55, 40, 200, 220 }, { 0.75, 220, 255, 255 },
          { 0.9, 20, 120, 160 }, { 1.0, 0, 10, 40 } },
        { { 0, 0, 0, 0 } },
        { { 0.0, 10, 10, 20 }, { 0.5, 240, 240, 240 }, { 1.0, 10, 10, 20 } },
    };
    for (int i = 0; i < PAL; i++) {
        double t = (double)i / PAL;
        uint32_t r, g, b;
        if (kind == 3) {   /* rainbow: three sines a third apart */
            double a = t * 6.283185307179586;
            r = (uint32_t)(127.5 + 127 * sind(a));
            g = (uint32_t)(127.5 + 127 * sind(a + 2.0944));
            b = (uint32_t)(127.5 + 127 * sind(a + 4.1888));
        } else {
            const struct stop *s = stops[kind];
            int k = 0;
            while (t > s[k + 1].at)
                k++;
            double f = (t - s[k].at) / (s[k + 1].at - s[k].at);
            f = f * f * (3 - 2 * f);
            r = (uint32_t)(s[k].r + f * (s[k + 1].r - s[k].r));
            g = (uint32_t)(s[k].g + f * (s[k + 1].g - s[k].g));
            b = (uint32_t)(s[k].b + f * (s[k + 1].b - s[k].b));
        }
        pal[i] = r << 16 | g << 8 | b;
    }
}

static inline float sqrtf_(float x)
{
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* Each pixel's colour: the average of its samples'. */
static void colour_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int n = v.ss * v.ss;
    const float *s = nu + (uint64_t)y * PW * n;
    for (uint32_t x = 0; x < PW; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int k = 0; k < n; k++, s++) {
            uint32_t c = 0x000000;
            if (*s >= 0) {
                /* sqrt: bands stay wide where the counts climb fast (deep zooms) */
                int idx = (int)(sqrtf_(*s + 1.0f) * 96.0f + (float)pal_shift) % PAL;
                c = pal[idx < 0 ? idx + PAL : idx];
            }
            r += c >> 16;
            g += c >> 8 & 0xff;
            b += c & 0xff;
        }
        img[(uint64_t)y * PW + x] = (r / n) << 16 | (g / n) << 8 | (b / n);
    }
}

/* The console's colours as cells: flat (16) and shaded mixes. */
struct mix { uint8_t ch, fg, bg; uint8_t r, g, b; };
static struct mix mixes[16 + 16 * 15 * 3];
static unsigned nmixes;
static uint16_t lut_mix[4096];   /* 4-bit-a-channel colour -> the closest mix */
static uint8_t lut_flat[4096];   /* -> the closest of the 16 colours */

static inline int dist(int r1, int g1, int b1, int r2, int g2, int b2)
{
    int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return 2 * dr * dr + 4 * dg * dg + 3 * db * db;
}

static void build_mixes(void)
{
    nmixes = 0;
    for (int bg = 0; bg < 16; bg++) {
        uint32_t c = fun_palette[bg];
        mixes[nmixes++] = (struct mix){ ' ', (uint8_t)bg, (uint8_t)bg, (uint8_t)(c >> 16),
                                        (uint8_t)(c >> 8), (uint8_t)c };
    }
    static const uint8_t glyphs[3] = { G_LIGHT, G_MEDIUM, G_DARK };
    for (int fg = 0; fg < 16; fg++)
        for (int bg = 0; bg < 16; bg++) {
            if (fg == bg)
                continue;
            uint32_t f = fun_palette[fg], b = fun_palette[bg];
            for (int s = 0; s < 3; s++) {
                int a = s + 1;   /* quarters of fg */
                mixes[nmixes++] = (struct mix){
                    glyphs[s], (uint8_t)fg, (uint8_t)bg,
                    (uint8_t)(((f >> 16 & 0xff) * a + (b >> 16 & 0xff) * (4 - a)) / 4),
                    (uint8_t)(((f >> 8 & 0xff) * a + (b >> 8 & 0xff) * (4 - a)) / 4),
                    (uint8_t)(((f & 0xff) * a + (b & 0xff) * (4 - a)) / 4),
                };
            }
        }
}

static void lut_item(uint32_t i, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int r = (int)(i >> 8) * 17, g = (int)(i >> 4 & 15) * 17, b = (int)(i & 15) * 17;
    int best = 1 << 30, bi = 0;
    for (unsigned k = 0; k < nmixes; k++) {
        int d = dist(r, g, b, mixes[k].r, mixes[k].g, mixes[k].b);
        if (d < best)
            best = d, bi = (int)k;
    }
    lut_mix[i] = (uint16_t)bi;
    best = 1 << 30;
    for (int k = 0; k < 16; k++) {
        uint32_t c = fun_palette[k];
        int d = dist(r, g, b, (int)(c >> 16), (int)(c >> 8 & 0xff), (int)(c & 0xff));
        if (d < best)
            best = d, bi = k;
    }
    lut_flat[i] = (uint8_t)bi;
}

static inline unsigned key12(uint32_t c)
{
    return (c >> 20 & 15) << 8 | (c >> 12 & 15) << 4 | (c >> 4 & 15);
}

#define R(c) ((int)((c) >> 16 & 0xff))
#define G(c) ((int)((c) >> 8 & 0xff))
#define B(c) ((int)((c) & 0xff))

/* One text row: two pixel rows -> cells. */
static void cell_row(uint32_t row, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    const uint32_t *top = img + (uint64_t)(2 * row) * PW, *bot = top + PW;
    for (uint32_t x = 0; x < PW; x++) {
        uint32_t t = top[x], b = bot[x];
        /* A half block: the nearest flat colour for each half. */
        uint8_t ft = lut_flat[key12(t)], fb = lut_flat[key12(b)];
        uint32_t pt = fun_palette[ft], pb = fun_palette[fb];
        int eh = dist(R(t), G(t), B(t), R(pt), G(pt), B(pt)) +
                 dist(R(b), G(b), B(b), R(pb), G(pb), B(pb));
        /* A shaded mix: the nearest to the two halves' average. */
        uint32_t avg = (uint32_t)((R(t) + R(b)) / 2) << 16 | (uint32_t)((G(t) + G(b)) / 2) << 8 |
                       (uint32_t)((B(t) + B(b)) / 2);
        const struct mix *m = &mixes[lut_mix[key12(avg)]];
        int em = dist(R(t), G(t), B(t), m->r, m->g, m->b) + dist(R(b), G(b), B(b), m->r, m->g, m->b);
        if (eh <= em)
            term_put(&T, (int)x, (int)(1 + row), ft == fb ? ' ' : G_UPPER, ft, fb);
        else
            term_put(&T, (int)x, (int)(1 + row), m->ch, m->fg, m->bg);
    }
}

static void recolour(void)
{
    pool_run(colour_row, NULL, PH);
    pool_run(cell_row, NULL, PH / 2);
}

/* ---- the self-test ------------------------------------------------------------------------ */

static int failures;

static void check(bool ok, const char *what)
{
    say("fractal: selftest: %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static bool view_alloc(uint32_t w, uint32_t h)
{
    PW = w;
    PH = h;
    nu = big_alloc((uint64_t)PW * PH * MAXSS * MAXSS * sizeof(float));
    img = big_alloc((uint64_t)PW * PH * 4);
    return nu && img;
}

static int selftest(void)
{
    uint32_t n = pool_start(0);
    say("fractal: selftest on %u threads (%u CPUs by CPUID)\n", n, fun_cpu_count());
    uint64_t it = 0;
    v.julia = false;

    /* Known points. */
    check(escape(0, 0, 1000, &it) < 0 && escape(-1, 0, 1000, &it) < 0 &&
              escape(-2, 0, 1000, &it) < 0 && escape(0.25, 0, 1000, &it) < 0,
          "0, -1, -2, 1/4 are in the set");
    float e1 = escape(1, 0, 1000, &it), e2 = escape(0.26, 0, 1000, &it);
    check(e1 >= 0 && e1 < 6 && e2 > 10 && e2 < 100, "1 escapes at once, 0.26 after a while");
    check(escape(-0.75, 0.1, 5000, &it) > 20, "-0.75 + 0.1i (near the neck) escapes slowly");
    /* The cardioid shortcut agrees with plain iteration. */
    bool agree = true;
    uint64_t s = 77;
    for (int i = 0; i < 2000; i++) {
        double pr = (double)(rng_next(&s) % 10000) / 10000 * 0.6 - 0.5;
        double pi = (double)(rng_next(&s) % 10000) / 10000 * 1.0 - 0.5;
        double q = (pr - 0.25) * (pr - 0.25) + pi * pi;
        if (!(q * (q + (pr - 0.25)) < 0.25 * pi * pi))
            continue;
        double zr = 0, zi = 0;
        int k = 0;
        while (k < 3000 && zr * zr + zi * zi < 4) {
            double t = zr * zr - zi * zi + pr;
            zi = 2 * zr * zi + pi;
            zr = t;
            k++;
        }
        agree &= k == 3000;
    }
    check(agree, "points the cardioid test calls inside don't escape");
    v.julia = true;
    v.jr = -1;
    v.ji = 0;
    check(escape(0, 0, 2000, &it) < 0 && escape(1.5, 1.5, 2000, &it) >= 0,
          "Julia c = -1 (the basilica): 0 stays, 1.5 + 1.5i escapes");
    v.julia = false;

    /* All CPUs render exactly what one does, and the set is symmetric. */
    view_alloc(160, 96);
    v.cx = -0.5;
    v.cy = 0;
    v.zoom = 1;
    v.ss = 1;
    v.maxit = 500;
    render(true);
    uint64_t bytes = (uint64_t)PW * PH * sizeof(float);
    float *par = big_alloc(bytes);
    memcpy(par, nu, bytes);
    render(false);
    check(!memcmp(par, nu, bytes), "render: all CPUs == one CPU (160 x 96)");
    bool sym = true;
    for (uint32_t y = 0; y < PH / 2; y++)
        sym &= !memcmp(nu + (uint64_t)y * PW, nu + (uint64_t)(PH - 1 - y) * PW, PW * sizeof(float));
    check(sym, "render: the set is mirror-symmetric about the real axis");
    uint32_t inside = 0;
    for (uint64_t i = 0; i < (uint64_t)PW * PH; i++)
        inside += nu[i] < 0;
    char what[96];
    snprintf(what, sizeof(what), "render: %u%% of the full view is inside (expect 10-20)",
             inside * 100 / (PW * PH));
    check(inside * 100 / (PW * PH) >= 10 && inside * 100 / (PW * PH) <= 20, what);

    /* The colour tables: each console colour comes back as itself. */
    build_mixes();
    pool_run(lut_item, NULL, 4096);
    bool self = nmixes == 16 + 16 * 15 * 3;
    for (int k = 0; k < 16; k++) {
        uint32_t c = fun_palette[k];
        self &= fun_palette[lut_flat[key12(c)]] == c || k == C_BLACK;
    }
    check(self, "colours: 736 mixes; the 16 colours map to themselves");

    /* How much faster all CPUs are (information only). */
    view_alloc(320, 176);
    v.cx = -0.743643887037151;
    v.cy = 0.131825904205330;
    v.zoom = 2000;
    v.ss = 1;
    v.maxit = 2000;
    uint64_t t0 = now_ns();
    uint64_t iters = render(false);
    uint64_t one = now_ns() - t0;
    t0 = now_ns();
    render(true);
    uint64_t all = now_ns() - t0;
    uint64_t x10 = all ? one * 10 / all : 0;
    say("fractal: selftest: seahorse valley 320x176, %lu M iterations: %lu ms on 1 CPU, %lu ms "
        "on %u threads (%lu.%lux)\n",
        (unsigned long)(iters / 1000000), (unsigned long)(one / 1000000),
        (unsigned long)(all / 1000000), n, (unsigned long)(x10 / 10), (unsigned long)(x10 % 10));

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

static void reset(void)
{
    v.julia = false;
    v.cx = places[0].cx;
    v.cy = places[0].cy;
    v.zoom = 1;
}

static void hud(uint64_t render_ns, uint64_t iters, const char *note)
{
    char a[40], b[40], z[24];
    term_fill(&T, 0, 0, (int)T.cols, 1, ' ', C_WHITE, C_BLUE);
    int x = term_text(&T, 1, 0, "JAM OS FRACTAL", C_BYELLOW, C_BLUE);
    if (v.julia)
        x = term_textf(&T, x + 2, 0, C_WHITE, C_BLUE, "Julia  c = %s %si", fmt_fixed(a, sizeof(a), v.jr, 6),
                       fmt_fixed(b, sizeof(b), v.ji, 6));
    else
        x = term_textf(&T, x + 2, 0, C_WHITE, C_BLUE, "Mandelbrot  %s %si",
                       fmt_fixed(a, sizeof(a), v.cx, 12), fmt_fixed(b, sizeof(b), v.cy, 12));
    x = term_textf(&T, x + 3, 0, C_BCYAN, C_BLUE, "zoom %sx", fmt_sci(z, sizeof(z), v.zoom));
    x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "%d iterations%s", it_used, v.maxit ? "" : " (auto)");
    x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "AA %dx%d", v.ss, v.ss);
    uint64_t us = render_ns / 1000, mips = render_ns ? iters * 1000 / render_ns : 0;
    x = term_textf(&T, x + 3, 0, C_BMAGENTA, C_BLUE, "%lu.%lu ms on %u CPUs = %lu M iterations/s",
                   (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 100), pool_threads(),
                   (unsigned long)mips);
    x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "colours: %s", pal_names[pal_kind]);
    if (v.zoom > 5e13)
        term_text(&T, x + 3, 0, "(the end of double precision)", C_BYELLOW, C_BLUE);
    int y = (int)T.rows - 1;
    term_fill(&T, 0, y, (int)T.cols, 1, ' ', C_GREY, C_BLACK);
    if (note)
        term_text(&T, 1, y, note, C_BYELLOW, C_BLACK);
    else
        term_text(&T, 1, y,
                  "arrows move   + - zoom   j julia   c colours   space cycle   a anti-alias   "
                  "[ ] iterations   1-6 places   t tour   b benchmark   r reset   q quit",
                  C_GREY, C_BLACK);
}

static int play(int argc, char **argv)
{
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    status_t st = term_open(&T);
    if (st != OK) {
        say("fractal: no console screen (%s)\n", status_str(st));
        return 1;
    }
    if (!view_alloc(T.cols, (T.rows - 2) * 2)) {
        term_close(&T);
        say("fractal: out of memory\n");
        return 1;
    }
    build_mixes();
    pool_run(lut_item, NULL, 4096);
    make_palette(pal_kind);
    reset();
    v.ss = pool_threads() >= 16 ? 3 : 2;   /* 3 x 3 samples a pixel when there are CPUs to spare */
    double saved_cx = 0, saved_cy = 0, saved_zoom = 1;
    bool quit = false, need = true, cycling = false, touring = false;
    uint64_t render_ns = 0, iters = 0, renders = 0;
    char note[160] = "";
    while (!quit) {
        uint64_t t0 = now_ns();
        if (touring) {
            v.zoom *= 1.06;
            if (v.zoom > 5e13)
                touring = false;
            need = true;
        }
        if (need) {
            iters = render(true);
            render_ns = now_ns() - t0;
            renders++;
            need = false;
        }
        if (cycling)
            pal_shift += 6;
        recolour();
        hud(render_ns, iters, note[0] ? note : NULL);
        term_flush(&T);
        uint64_t deadline = cycling || touring ? t0 + 33000000ull : DEADLINE_NEVER;
        int k;
        while ((k = term_key(&T, deadline)) != KEY_NONE) {
            double step = pixel_size() * PH / 8;
            note[0] = '\0';
            switch (k) {
            case KEY_QUIT: case 'q': case 'Q': quit = true; break;
            case KEY_LEFT:  v.cx -= step; need = true; break;
            case KEY_RIGHT: v.cx += step; need = true; break;
            case KEY_UP:    v.cy -= step; need = true; break;
            case KEY_DOWN:  v.cy += step; need = true; break;
            case '+': case '=': case KEY_PGUP: case KEY_ENTER:
                if (v.zoom < 1e14)
                    v.zoom *= 2;
                need = true;
                break;
            case '-': case '_': case KEY_PGDN:
                if (v.zoom > 0.25)
                    v.zoom /= 2;
                need = true;
                break;
            case 'j': case 'J':
                if (!v.julia) {
                    saved_cx = v.cx, saved_cy = v.cy, saved_zoom = v.zoom;
                    v.jr = v.cx;
                    v.ji = v.cy;
                    v.cx = v.cy = 0;
                    v.zoom = 1;
                    v.julia = true;
                } else {
                    v.julia = false;
                    v.cx = saved_cx, v.cy = saved_cy, v.zoom = saved_zoom;
                }
                need = true;
                break;
            case 'c': case 'C':
                pal_kind = (pal_kind + 1) % NPALS;
                make_palette(pal_kind);
                break;
            case ' ': cycling = !cycling; break;
            case 'a': case 'A': v.ss = v.ss % MAXSS + 1; need = true; break;
            case '[': v.maxit = (v.maxit ? v.maxit : it_used) / 2; if (v.maxit < 50) v.maxit = 50; need = true; break;
            case ']': v.maxit = (v.maxit ? v.maxit : it_used) * 2; if (v.maxit > 100000) v.maxit = 100000; need = true; break;
            case '0': v.maxit = 0; need = true; break;
            case 't': case 'T':
                touring = !touring;
                if (touring && !v.julia && v.zoom < 2) {   /* somewhere worth diving into */
                    v.cx = places[1].cx;
                    v.cy = places[1].cy;
                }
                break;
            case 'r': case 'R': case KEY_HOME: reset(); need = true; break;
            case 'b': case 'B': {
                uint64_t b0 = now_ns();
                render(false);
                uint64_t one = now_ns() - b0;
                b0 = now_ns();
                render(true);
                uint64_t all = now_ns() - b0, x10 = all ? one * 10 / all : 0;
                snprintf(note, sizeof(note),
                         "benchmark: 1 CPU %lu ms, %u CPUs %lu ms: %lu.%lux faster   (any key: help)",
                         (unsigned long)(one / 1000000), pool_threads(),
                         (unsigned long)(all / 1000000), (unsigned long)(x10 / 10),
                         (unsigned long)(x10 % 10));
                printf("fractal: %s\n", note);   /* the kernel log: the screen is ours */
                break;
            }
            default:
                if (k >= '1' && k <= '6') {
                    int p = k - '1';
                    v.julia = false;
                    v.cx = places[p].cx;
                    v.cy = places[p].cy;
                    v.zoom = places[p].zoom;
                    snprintf(note, sizeof(note), "%d: %s", p + 1, places[p].name);
                    need = true;
                }
            }
            if (quit || need)
                break;
            /* Recolour-only changes: show them at once. */
            recolour();
            hud(render_ns, iters, note[0] ? note : NULL);
            term_flush(&T);
        }
    }
    term_close(&T);
    say("fractal: %lu renders on %u threads, the last %lu ms\n", (unsigned long)renders,
        pool_threads(), (unsigned long)(render_ns / 1000000));
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    return play(argc, argv);
}
