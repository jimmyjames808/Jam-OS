/* demo: the visual demo, fractals drawn by every CPU at once.
 *
 * The shell's `demo [seconds]` runs it with argv "cpus=<n>" "seconds=<s>"
 * (the shell asks the kernel for the CPU count; default 76 s). Like the
 * fun apps it borrows the screen and the keys from the console through
 * libfun (gfx_open), and any key stops it.
 *
 * The pool (libfun: one thread per CPU, this one included) renders each
 * frame at half resolution, rows handed out a few at a time (so fast rows
 * and slow rows even out); then this thread draws the overlay, the pool
 * doubles every pixel into the screen's back buffer, and gfx_present_all
 * shows it. Three scenes:
 *
 *   1. a deep zoom into the Mandelbrot set's Seahorse Valley, 1.5 -> ~1e-13
 *   2. a Julia set whose constant walks round a circle
 *   3. the Mandelbrot set with every row tinted by the thread that drew it,
 *      so you can watch the work spread over the CPUs */
#include <font.h>
#include <fun.h>

#define PALETTE 1024
#define CHUNK   8     /* rows per work item: visible bands in scene 3 */

static struct surf half;                    /* the frame, half resolution */
static uint32_t palette[PALETTE];
static uint32_t tint[FUN_MAX_THREADS];      /* scene 3: a colour per thread */
static uint32_t rows_by[FUN_MAX_THREADS];   /* rows each thread drew this frame */
static uint32_t ncpu;                       /* from the shell: the CPUs to use */
static bool quit;                           /* a key was pressed */

/* The frame being drawn, set by this thread between frames. */
enum { MANDEL, JULIA };
static struct {
    int      kind;            /* MANDEL or JULIA */
    double   cx, cy, px;      /* centre and the size of one pixel */
    double   jr, ji;          /* Julia constant */
    int      maxit;           /* iterations before a point counts as inside */
    double   shift;           /* palette rotation */
    bool     tinted;          /* scene 3: colour rows by thread */
} fr;

/* The classic deep-blue / white / gold gradient, cyclic. */
static void make_palette(void)
{
    static const struct { double at; uint8_t r, g, b; } stops[] = {
        { 0.0, 0, 7, 100 },     { 0.16, 32, 107, 203 }, { 0.42, 237, 255, 255 },
        { 0.6425, 255, 170, 0 }, { 0.8575, 0, 2, 0 },    { 1.0, 0, 7, 100 },
    };
    for (int i = 0; i < PALETTE; i++) {
        double t = (double)i / PALETTE;
        int k = 0;
        while (t > stops[k + 1].at)
            k++;
        double f = (t - stops[k].at) / (stops[k + 1].at - stops[k].at);
        f = f * f * (3 - 2 * f);   /* smooth between stops */
        uint32_t r = (uint32_t)(stops[k].r + f * (stops[k + 1].r - stops[k].r));
        uint32_t g = (uint32_t)(stops[k].g + f * (stops[k + 1].g - stops[k].g));
        uint32_t b = (uint32_t)(stops[k].b + f * (stops[k + 1].b - stops[k].b));
        palette[i] = rgb(r, g, b);
    }
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++) {   /* one hue per thread */
        double a = i * 2.399963;                        /* golden angle */
        tint[i] = rgb((uint32_t)(128 + 127 * sind(a)), (uint32_t)(128 + 127 * sind(a + 2.094)),
                      (uint32_t)(128 + 127 * sind(a + 4.189)));
    }
}

/* ---- rendering --------------------------------------------------------------- */

static inline uint32_t half_mix(uint32_t a, uint32_t b)   /* 50/50, per channel */
{
    return ((a >> 1) & 0x7f7f7f7fu) + ((b >> 1) & 0x7f7f7f7fu);
}

static void render_row(uint32_t y, uint32_t me)
{
    uint32_t W = (uint32_t)half.w, H = (uint32_t)half.h;
    uint32_t *out = half.px + (uint64_t)y * half.stride;
    double ci0 = fr.cy + ((double)y - H / 2.0) * fr.px;
    for (uint32_t x = 0; x < W; x++) {
        double cr = fr.cx + ((double)x - W / 2.0) * fr.px, ci = ci0;
        double zr, zi, kr, ki;
        if (fr.kind == JULIA) {
            zr = cr, zi = ci, kr = fr.jr, ki = fr.ji;
        } else {
            zr = 0, zi = 0, kr = cr, ki = ci;
            /* Inside the main cardioid or the period-2 bulb: never escapes. */
            double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
            if (q * (q + (cr - 0.25)) < 0.25 * ci * ci ||
                (cr + 1) * (cr + 1) + ci * ci < 0.0625) {
                out[x] = rgb(0, 0, 12);
                continue;
            }
        }
        int i = 0;
        double r2 = zr * zr, i2 = zi * zi;
        while (i < fr.maxit && r2 + i2 < 256.0) {
            zi = 2 * zr * zi + ki;
            zr = r2 - i2 + kr;
            r2 = zr * zr;
            i2 = zi * zi;
            i++;
        }
        uint32_t c;
        if (i >= fr.maxit) {
            c = rgb(0, 0, 12);
        } else {
            double nu = i + 1 - log2d(log2d(r2 + i2) * 0.5);   /* smooth iteration count */
            int idx = (int)((nu * 12 + fr.shift)) % PALETTE;
            if (idx < 0)
                idx += PALETTE;
            c = palette[idx];
        }
        out[x] = fr.tinted ? half_mix(c, tint[me]) : c;
    }
}

/* Pool items: CHUNK rows of the frame each. */
static void render_item(uint32_t item, uint32_t me, void *arg)
{
    (void)arg;
    for (uint32_t y = item * CHUNK; y < (item + 1) * CHUNK && y < (uint32_t)half.h; y++) {
        render_row(y, me);
        rows_by[me]++;
    }
}

/* Pool items: row y of the frame, each pixel doubled, into the back buffer. */
static void double_item(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    const uint32_t *src = half.px + (uint64_t)y * half.stride;
    for (int k = 0; k < 2; k++) {
        uint32_t *dst = scr.s.px + (uint64_t)(2 * y + k) * scr.s.stride;
        for (int x = 0; x < half.w; x++) {
            dst[2 * x] = src[x];
            dst[2 * x + 1] = src[x];
        }
    }
}

/* ---- the overlay ------------------------------------------------------------- */

/* Fixed-width text in the console font, on the half-resolution frame. */
static void mono(int x0, int y0, const char *s, uint32_t fg)
{
    for (; *s; s++, x0 += 8) {
        const uint8_t *g = font_8x16[(uint8_t)*s & 0x7f];
        for (int y = 0; y < 16 && y0 + y < half.h; y++)
            for (int x = 0; x < 8 && x0 + x < half.w; x++)
                if (g[y] & (0x80 >> x))
                    half.px[(uint64_t)(y0 + y) * half.stride + x0 + x] = fg;
    }
}

static void fmt_sci(char *buf, size_t n, double v)   /* 1.2e9 */
{
    int e = 0;
    while (v >= 10) {
        v /= 10;
        e++;
    }
    int whole = (int)v, tenth = (int)((v - whole) * 10);
    snprintf(buf, n, "%d.%de%d", whole, tenth, e);
}

static void overlay(const char *scene, double fps, double zoom)
{
    char line[160], z[32];
    fmt_sci(z, sizeof(z), zoom);
    int bw = 8 * 58, bh = 16 * 4 + 12 + 40;
    blend(&half, 8, 8, bw, bh, 0x000000, 192);   /* a quarter as bright */
    mono(16, 12, "JAM OS  -  VISUAL DEMO", rgb(255, 210, 90));
    mono(16, 30, scene, rgb(230, 230, 240));
    int f10 = (int)(fps * 10);
    if (fr.kind == JULIA)
        snprintf(line, sizeof(line), "%u CPUs   %d.%d fps   %d iterations", ncpu, f10 / 10,
                 f10 % 10, fr.maxit);
    else
        snprintf(line, sizeof(line), "%u CPUs   %d.%d fps   zoom %sx   %d iterations", ncpu,
                 f10 / 10, f10 % 10, z, fr.maxit);
    mono(16, 48, line, rgb(170, 200, 255));
    mono(16, 66, "rows drawn per thread this frame:", rgb(150, 150, 170));
    /* A bar per thread: how many rows it took (dynamic balancing at work). */
    uint32_t n = pool_threads(), maxr = 1;
    for (uint32_t t = 0; t < n; t++)
        if (rows_by[t] > maxr)
            maxr = rows_by[t];
    int bx = 16, by = 86, barw = (bw - 16) / (int)n;
    for (uint32_t t = 0; t < n; t++) {
        int hgt = (int)(rows_by[t] * 28 / maxr);
        fill(&half, bx + (int)t * barw + 1, by + 29 - hgt, barw - 2, hgt, tint[t]);
    }
}

/* ---- scenes -------------------------------------------------------------------- */

struct result { const char *name; uint32_t frames; uint64_t ns; };

static double now_s(uint64_t t0) { return (double)(now() - t0) / 1e9; }

/* Any key down (or the console going away) ends the demo. */
static bool key_pressed(void)
{
    struct input_key_event ev;
    status_t st;
    while (!quit && (st = gfx_key_event(0, &ev)) != ERR_TIMED_OUT)
        if (st != OK || ev.state != INPUT_KEY_UP)
            quit = true;
    return quit;
}

/* Where scene `which` is at t of `seconds`; the zoom for the overlay. */
static double place(int which, double t, double seconds)
{
    uint32_t W = (uint32_t)half.w;
    fr.tinted = which == 3;
    fr.shift = t * 60;
    if (which == 1) {           /* Seahorse Valley, 1.5 -> ~1e-13 across the scene */
        double scale = 1.5 * exp2d(-t / seconds * 43.0), zoom = 1.5 / scale;
        fr.kind = MANDEL;
        fr.cx = -0.743643887037158704752191506114774;
        fr.cy = 0.131825904205311970493132056385139;
        fr.px = 2 * scale / W;
        fr.maxit = 250 + (int)(55 * log2d(zoom));
        return zoom;
    }
    if (which == 2) {           /* Julia: c walks round a circle */
        double a = t / seconds * 2 * 3.141592653589793 + 0.4;
        fr.kind = JULIA;
        fr.jr = 0.7885 * cosd(a);
        fr.ji = 0.7885 * sind(a);
        fr.cx = 0;
        fr.cy = 0;
        fr.px = 3.2 / W;
        fr.maxit = 300;
        return 1;
    }
    double scale = 0.012 * exp2d(-2 * sind(t * 0.5));   /* rows tinted by their thread */
    fr.kind = MANDEL;
    fr.cx = -0.7453;
    fr.cy = 0.1127;
    fr.px = 2 * scale / W;
    fr.maxit = 700;
    return 1.5 / scale;
}

static struct result scene(const char *name, double seconds, int which)
{
    static const char *const titles[] = {
        "", "1/3  Mandelbrot deep zoom: Seahorse Valley", "2/3  Julia set, c = 0.7885 e^(ia)",
        "3/3  who drew what: each colour band is one thread",
    };
    uint64_t t0 = now(), last = t0;
    uint32_t frames = 0;
    double fps = 0;
    while (now_s(t0) < seconds && !key_pressed()) {
        for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
            rows_by[i] = 0;
        double zoom = place(which, now_s(t0), seconds);
        pool_run(render_item, NULL, ((uint32_t)half.h + CHUNK - 1) / CHUNK);
        uint64_t n = now();
        if (n > last)
            fps = fps ? fps * 0.8 + 0.2 * (1e9 / (double)(n - last)) : 1e9 / (double)(n - last);
        last = n;
        overlay(titles[which], fps, zoom);
        pool_run(double_item, NULL, (uint32_t)half.h);
        gfx_present_all();   /* every pixel changed: no need to compare */
        frames++;
    }
    return (struct result){ name, frames, now() - t0 };
}

int main(int argc, char **argv)
{
    ncpu = (uint32_t)arg_num(argc, argv, "cpus", 1);
    uint32_t seconds = (uint32_t)arg_num(argc, argv, "seconds", 76);   /* 40 + 22 + 14 */
    if (!ncpu)
        ncpu = 1;
    if (!seconds)
        seconds = 76;
    uint32_t threads = pool_start(ncpu > FUN_MAX_THREADS ? FUN_MAX_THREADS : ncpu);
    status_t st = gfx_open();
    if (st != OK) {
        printf("demo: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    half = surf_new(scr.w / 2, scr.h / 2);
    if (!half.px) {
        gfx_close();
        printf("demo: no back buffer (%s)\n", status_str(ERR_NO_MEMORY));
        return 1;
    }
    make_palette();

    struct result r[3];
    r[0] = scene("Mandelbrot deep zoom", seconds * 40.0 / 76, 1);
    r[1] = scene("Julia set", seconds * 22.0 / 76, 2);
    r[2] = scene("who drew what", seconds * 14.0 / 76, 3);
    gfx_close();   /* the console redraws its screen */

    printf("demo: %ux%u screen, %ux%u rendered, %u threads on %u CPUs%s\n", scr.w, scr.h, half.w,
           half.h, threads, ncpu, quit ? " (stopped by a key)" : "");
    for (int i = 0; i < 3; i++) {
        uint64_t f10 = r[i].ns ? (uint64_t)r[i].frames * 10000000000ull / r[i].ns : 0;
        printf("demo: %-20s %u frames, %lu.%lu fps\n", r[i].name, r[i].frames,
               (unsigned long)(f10 / 10), (unsigned long)(f10 % 10));
    }
    return 0;
}
