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

/* The tour's target: a Misiurewicz point (preperiod 24, period 1) near
 * -0.77568377 + 0.13646737i, a spiral at every scale (found by Newton's
 * method to 60 digits; here to double-double's 32). */
const dd tour_x = { -0.7756837680090538, 1.1497740127108711e-17 };
const dd tour_y = { 0.1364673682946901, 1.3525418034032015e-17 };

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
    view.julia = false;
    view.cx = dd_d(cx);
    view.cy = dd_d(cy);
    view.zoom = zoom;
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
        snprintf(buf, size, "%d",
                 (int)(x * (e == 0 ? 1 : e == 1 ? 10 : e == 2 ? 100 : 1000) + 0.5));
    else
        snprintf(buf, size, "%d.%de%d", (int)x, (int)((x - (int)x) * 10), e);
    return buf;
}

#define SLICE_NS 30000000ull   /* work between frames (a key gets an answer at once) */

/* Exploring: what the keys change between frames. */
static struct {
    bool     quit;                /* q */
    bool     cycling, touring;    /* space: the palette cycles; t: the tour runs */
    bool     recolour;            /* the palette changed: colour every tile again */
    dd       saved_cx, saved_cy;  /* where j left the Mandelbrot set (j again goes back) */
    double   saved_zoom;          /* and its zoom */
    int      step_px;             /* an arrow: 1/8 of the height, whole tiles */
    uint64_t tour_step_at;        /* the tour's last step in */
} ex = { .saved_zoom = 1, .recolour = true };

static char note[200];
static bool show_help = true;
static struct fps fps;
static int hud_last[2][4];       /* the HUD's rectangles last frame */

/* ---- the HUD: a panel at the top left, the help line at the bottom ---- */

static const char *const help_line =
    "arrows move    + - zoom    j Julia    c colours    d density    space cycle"
    "    a anti-alias    [ ] iterations    0 auto    1-6 places    t tour"
    "    b benchmark    r reset    h help    q quit";

struct hud {
    char l1[160], l2[160], l3[220], l4[220];   /* the panel's four lines */
    int  u, pad, lh, badge;      /* scale, padding, line height, the DEEP badge's width */
    int  px, py, pw, ph;         /* the panel */
    int  hw, hh;                 /* the help line's panel */
};

/* The HUD's text and where it goes, for the view as it is now. */
static void hud_layout(struct hud *h)
{
    int u = h->u = scr.ui, pad = h->pad = 10 * u;
    char a[64], b[64], z[32];
    int digits = 8;
    for (double zz = view.zoom; zz >= 10 && digits < 34; zz /= 10)
        digits++;
    if (view.julia)
        snprintf(h->l1, sizeof(h->l1), "Julia set    c = %s %si",
                 dd_str(a, sizeof(a), dd_d(view.jr), 10), dd_str(b, sizeof(b), dd_d(view.ji), 10));
    else
        snprintf(h->l1, sizeof(h->l1), "Mandelbrot set");
    snprintf(h->l2, sizeof(h->l2), "\a%s  %si\a", dd_str(a, sizeof(a), view.cx, digits),
             dd_str(b, sizeof(b), view.cy, digits));
    snprintf(h->l3, sizeof(h->l3),
             "zoom \a%sx\a    \a%d\a iterations%s    anti-aliasing \a%s\a    colours \a%s\a%s%s",
             fmt_sci(z, sizeof(z), view.zoom), kv.maxit, view.maxit ? "" : " (auto)",
             view.ss > 1 ? (view.ss == 2 ? "2x2" : view.ss == 3 ? "3x3" : "4x4") : "off",
             pal_names[pal_kind], ex.cycling ? " (cycling)" : "", ex.touring ? "    \aTOUR\a" : "");
    uint64_t ns = pass_target < 0 ? view_ns : now() - view_t0;
    uint64_t mips = ns ? view_iters * 1000 / ns : 0;
    const char *kern = kv.mode == M_PERTURB ? "perturbation" : kv.mode == M_DD ? "double-double"
                     : kv.avx2 ? "AVX2" : "double";
    if (pass_target >= 0)
        snprintf(h->l4, sizeof(h->l4),
                 "rendering \a%d%%\a    \a%s\a M iterations/s on \a%u\a CPUs (%s)    %u.%u fps",
                 (int)(progress() * 100), commas(a, sizeof(a), mips), pool_threads(), kern,
                 fps.x10 / 10, fps.x10 % 10);
    else
        snprintf(h->l4, sizeof(h->l4),
                 "done in \a%lu.%02lu s\a:  %s M iterations,  \a%s\a M/s on \a%u\a CPUs (%s)",
                 (unsigned long)(ns / 1000000000), (unsigned long)(ns / 10000000 % 100),
                 commas(a, sizeof(a), view_iters / 1000000), commas(b, sizeof(b), mips),
                 pool_threads(), kern);
    int w = text_width(2 * u, "FRACTAL") + pad + text_width(u, h->l1);
    const char *lines[] = { h->l2, h->l3, h->l4, note };
    for (int i = 0; i < 4; i++) {
        int lw = text_width(u, lines[i]);
        w = lw > w ? lw : w;
    }
    h->lh = TEXT_H(u) + 4 * u;
    h->badge = text_width(u, "DEEP") + 2 * pad;
    h->ph = TEXT_H(2 * u) + pad / 2 + h->lh * (note[0] ? 4 : 3) + pad * 2;
    h->px = pad;
    h->py = pad;
    h->pw = w + 2 * pad + (deep() ? h->badge + pad : 0);
    h->hw = text_width(u, help_line) + 2 * pad;
    h->hh = TEXT_H(u) + pad;
}

/* Mark the tiles under the HUD, now and last frame, to be coloured again. */
static void hud_mark(const struct hud *h)
{
    const struct surf *s = &scr.s;
    int u = h->u, pad = h->pad;
    for (int i = 0; i < 2; i++)
        dirty_rect(hud_last[i][0], hud_last[i][1], hud_last[i][2], hud_last[i][3]);
    int cur[2][4] = { { h->px, 0, h->pw + 8 * u, h->py + h->ph + 8 * u },
                      { (s->w - h->hw) / 2, s->h - h->hh - pad, show_help ? h->hw : 0, h->hh } };
    memcpy(hud_last, cur, sizeof(cur));
    for (int i = 0; i < 2; i++)
        dirty_rect(cur[i][0], cur[i][1], cur[i][2], cur[i][3]);
    dirty_rect(0, 0, s->w, 3 * u);
}

static void hud_draw(const struct hud *h)
{
    const struct surf *s = &scr.s;
    int u = h->u, pad = h->pad, px = h->px, py = h->py;
    /* progress: a thin line along the top while the passes run */
    if (pass_target >= 0)
        fill(s, 0, 0, (int)(s->w * progress()), 2 * u, 0xffd060);
    panel(s, &(struct rect){ px, py, h->pw, h->ph }, 8 * u, 0x000000, 165);
    int x = text_shadow(s, px + pad, py + pad, 2 * u, 0xffd060, "FRACTAL");
    text(s, x + pad, py + pad + TEXT_H(2 * u) - TEXT_H(u) - 2 * u, u, 0xe8ecff, h->l1);
    if (deep()) {
        int bx = px + h->pw - h->badge - pad;
        panel(s, &(struct rect){ bx, py + pad, h->badge, TEXT_H(u) + pad / 2 }, 5 * u, 0xd03060,
              256);
        text(s, bx + pad, py + pad + pad / 4, u, 0xffffff, "DEEP");
    }
    int y = py + pad + TEXT_H(2 * u) + pad / 2;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, h->l2);
    y += h->lh;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, h->l3);
    y += h->lh;
    text2(s, px + pad, y, u, 0x8890b0, 0xffffff, false, h->l4);
    y += h->lh;
    if (note[0])
        text(s, px + pad, y, u, 0xffe070, note);
    if (show_help) {
        panel(s, &(struct rect){ (s->w - h->hw) / 2, s->h - h->hh - pad, h->hw, h->hh }, 6 * u,
              0x000000, 160);
        text(s, (s->w - h->hw) / 2 + pad, s->h - h->hh - pad + pad / 2, u, 0xb0b8d8, help_line);
    }
}

/* ---- b: the benchmark ---- */

/* The view's points on a grid (every 4th pixel each way), 1 CPU then all. */
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
    uint64_t t0 = now();
    for (uint32_t j = 0; j < rows; j++)
        bench_row(j, 0, NULL);
    uint64_t one = now() - t0, iters = iters_by[0];
    t0 = now();
    pool_run(bench_row, NULL, rows);
    uint64_t all = now() - t0, x10 = all ? one * 10 / all : 0;
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

/* ---- the keys ---- */

static void reset(void)
{
    go(places[0].cx, places[0].cy, 1);
    reset_tiles(false);
}

/* An arrow: the view moves by dx, dy pixels (whole tiles). */
static void pan(int dx, int dy)
{
    if (dx)
        view.cx = dd_add_d(view.cx, dx * kv.px);
    if (dy)
        view.cy = dd_add_d(view.cy, dy * kv.px);
    shift(dx, dy);
}

/* j: the Julia set of the point in the middle, or back from it. */
static void julia_toggle(void)
{
    if (!view.julia) {
        ex.saved_cx = view.cx, ex.saved_cy = view.cy, ex.saved_zoom = view.zoom;
        view.jr = view.cx.hi + view.cx.lo;
        view.ji = view.cy.hi + view.cy.lo;
        view.cx = view.cy = dd_d(0);
        view.zoom = 1;
        view.julia = true;
    } else {
        view.julia = false;
        view.cx = ex.saved_cx, view.cy = ex.saved_cy, view.zoom = ex.saved_zoom;
    }
    ex.touring = false;
    reset_tiles(false);
}

/* [ ] 0: half or twice the iterations, or automatic. */
static void iterations(int k)
{
    view.maxit = k == '0' ? 0 : (view.maxit ? view.maxit : kv.maxit) * (k == '[' ? 1 : 4) / 2;
    if (k != '0' && view.maxit < 50)
        view.maxit = 50;
    if (view.maxit > 200000)
        view.maxit = 200000;
    reset_tiles(true);   /* same place: keep showing it while it's redone */
}

/* t: the tour on or off (from the whole set: to the tour's target). */
static void tour_toggle(void)
{
    ex.touring = !ex.touring;
    if (ex.touring && !view.julia && view.zoom < 2) {   /* somewhere worth diving into */
        view.cx = tour_x;
        view.cy = tour_y;
        view.zoom = 1;
        reset_tiles(false);
    }
    snprintf(note, sizeof(note), ex.touring ? "tour: zooming in, down to 1e28x (t stops it)"
                                            : "tour stopped");
}

static void handle_key(int k)
{
    int step = ex.step_px;
    note[0] = '\0';
    switch (k) {
    case KEY_QUIT: case 'q': case 'Q': ex.quit = true; break;
    case KEY_LEFT:  pan(-step, 0); break;
    case KEY_RIGHT: pan(step, 0); break;
    case KEY_UP:    pan(0, -step); break;
    case KEY_DOWN:  pan(0, step); break;
    case '+': case '=': case KEY_PGUP: case KEY_ENTER:
        if (view.zoom * 2 <= MAX_ZOOM) {
            view.zoom *= 2;
            reproject(2);
        } else {
            snprintf(note, sizeof(note), "1e28x is as deep as double-double goes");
        }
        break;
    case '-': case '_': case KEY_PGDN:
        if (view.zoom > 0.25) {
            view.zoom /= 2;
            reproject(0.5);
        }
        break;
    case 'j': case 'J': julia_toggle(); break;
    case 'c': case 'C':
        pal_kind = (pal_kind + 1) % NPALS;
        make_palette(pal_kind);
        snprintf(note, sizeof(note), "colours: %s", pal_names[pal_kind]);
        ex.recolour = true;
        break;
    case 'd': case 'D':
        dens_i = (dens_i + 1) % 4;
        snprintf(note, sizeof(note), "colour density %d", (int)dens_v[dens_i]);
        ex.recolour = true;
        break;
    case ' ': ex.cycling = !ex.cycling; break;
    case 'a': case 'A': aa_next(); ex.recolour = true; break;
    case '[': case ']': case '0': iterations(k); break;
    case 't': case 'T': tour_toggle(); break;
    case 'r': case 'R': case KEY_HOME: ex.touring = false; reset(); break;
    case 'b': case 'B': benchmark(); break;
    case 'h': case 'H': show_help = !show_help; break;
    default:
        if (k >= '1' && k <= '6') {
            int p = k - '1';
            ex.touring = false;
            go(places[p].cx, places[p].cy, places[p].zoom);
            reset_tiles(false);
            snprintf(note, sizeof(note), "%d: %s", p + 1, places[p].name);
        }
    }
}

/* ---- the frame ---- */

/* The tour: one step in once the picture is good enough (every other pixel
 * computed; every fourth when deep, where it's slower). */
static void tour_step(uint64_t t0)
{
    int good = deep() ? 4 : 2;
    if (pass_target >= 0 && (pass_target >= good || t0 - ex.tour_step_at <= 40000000ull))
        return;
    double f = deep() ? 1.08 : 1.04;
    if (view.zoom * f > MAX_ZOOM) {
        ex.touring = false;
        snprintf(note, sizeof(note), "tour: the end of double-double (1e28x)");
    } else {
        view.zoom *= f;
        reproject(f);
        ex.tour_step_at = t0;
    }
}

/* Colour what changed, the HUD over it, to the screen. */
static void show(void)
{
    if (ex.touring) {
        nu_min_shown += (nu_min - nu_min_shown) * 0.25f;
        ex.recolour = true;
    } else if (nu_min_shown != nu_min) {
        nu_min_shown = nu_min;
        ex.recolour = true;
    }
    if (ex.cycling) {
        pal_shift += 4;
        ex.recolour = true;
    }
    struct hud h;
    hud_layout(&h);
    hud_mark(&h);
    colour_tiles(ex.recolour);
    ex.recolour = false;
    fps_frame(&fps);
    hud_layout(&h);   /* again: the fps just changed */
    hud_draw(&h);
    gfx_present();
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
    view.ss = pool_threads() >= 16 ? 3 : 2;   /* more samples when there are CPUs to spare */
    reset();
    ex.saved_cx = view.cx;
    ex.saved_cy = view.cy;
    ex.step_px = ((PH / 8 + TS / 2) / TS) * TS;
    uint64_t renders = 0, last_frame = 0, frames = 0;
    while (!ex.quit) {
        uint64_t t0 = now();
        bool busy = pass_target >= 0 || ex.touring;
        /* Keys: whatever came; wait for one only when there's nothing to do. */
        int k = gfx_key(busy ? 0 : ex.cycling ? last_frame + 16666667 : DEADLINE_NEVER);
        for (; k != KEY_NONE && !ex.quit; k = gfx_key(0))
            handle_key(k);
        if (ex.quit)
            break;
        if (ex.touring)
            tour_step(t0);
        bool was_busy = pass_target >= 0;
        if (pass_target >= 0)
            work(t0 + (ex.touring ? SLICE_NS / 2 : SLICE_NS));
        if (was_busy && pass_target < 0)
            renders++;
        show();
        frames++;
        last_frame = t0;
    }
    gfx_close();
    char z[32];
    say("fractal: %lu renders on %u threads (the last %lu ms), %lu frames, zoom %sx; %lu MB to "
        "the screen\n",
        (unsigned long)renders, pool_threads(), (unsigned long)(view_ns / 1000000),
        (unsigned long)frames, fmt_sci(z, sizeof(z), view.zoom), (unsigned long)(scr.bytes >> 20));
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return fractal_selftest();
    return play(argc, argv);
}
