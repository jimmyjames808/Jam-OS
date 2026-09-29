/* The "Visual demo" boot entry: fractals drawn by every CPU at once.
 *
 * A normal user process. The kernel describes the screen in argv[1]
 * ("demo:<phys hex>:<width>:<height>:<pitch>:<r>:<g>:<b shift>:<cpus>"),
 * init hands us the root resource, and we map the framebuffer ourselves as
 * a write-combining physical VMO. One thread per CPU (this one included)
 * renders a half-resolution frame into a RAM back buffer, rows handed out
 * one at a time through an atomic counter (so fast rows and slow rows even
 * out); then this thread draws the overlay and all of them copy the frame
 * to the screen, each pixel doubled. Three scenes:
 *
 *   1. a deep zoom into the Mandelbrot set's Seahorse Valley, 1.5 -> ~1e-13
 *   2. a Julia set whose constant walks round a circle
 *   3. the Mandelbrot set with every row tinted by the thread that drew it,
 *      so you can watch the work spread over the CPUs
 *
 * No libm here: log2 / exp2 / sin / cos are small approximations, which is
 * plenty for colours and camera paths. */
#include <os.h>
#include <jam_syscalls.h>

extern const uint8_t font_8x16[128][16];

#define STACK        (64u << 10)
#define MAX_THREADS  64
#define PALETTE      1024

static struct {
    uint64_t phys;
    uint32_t w, h, pitch, rs, gs, bs, ncpu;
} scr;

static volatile uint8_t *fbmem;   /* the screen (WC) */
static uint32_t *back;            /* half resolution, W x H */
static uint32_t W, H;
static uint32_t palette[PALETTE];
static uint32_t tint[MAX_THREADS];

/* The frame being drawn, set by the main thread between frames. */
enum { MANDEL, JULIA };
static struct {
    int      kind;
    double   cx, cy, px;      /* centre and the size of one pixel */
    double   jr, ji;          /* Julia constant */
    int      maxit;
    double   shift;           /* palette rotation */
    bool     tinted;          /* scene 3: colour rows by thread */
} fr;

static volatile uint32_t phase;        /* bumped to start a render (odd) or copy (even) */
static volatile uint32_t next_row;
static volatile uint32_t done;
static volatile bool     stop;
static uint32_t nthreads;
static uint32_t rows_by[MAX_THREADS];

/* ---- a little maths ---------------------------------------------------------- */

static double log2d(double x)   /* x > 0 */
{
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;   /* m in [1, 2) */
    double m = v.d, t = (m - 1) / (m + 1), t2 = t * t;
    /* ln(m) = 2 atanh(t); log2 = ln / ln 2 */
    double ln = 2 * t * (1 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 * (1.0 / 7 + t2 / 9))));
    return e + ln * 1.4426950408889634;
}

static double exp2d(double x)
{
    double fl = (double)(int64_t)x;
    if (fl > x)
        fl -= 1;
    double f = (x - fl) * 0.6931471805599453, term = 1, sum = 1;
    for (int k = 1; k < 12; k++) {
        term *= f / k;
        sum += term;
    }
    union { double d; uint64_t u; } v = { sum };
    v.u += (uint64_t)(int64_t)fl << 52;
    return v.d;
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

static double cosd(double x) { return sind(x + 1.5707963267948966); }

static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (r << scr.rs) | (g << scr.gs) | (b << scr.bs);
}

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
    for (uint32_t i = 0; i < MAX_THREADS; i++) {   /* one hue per thread */
        double a = i * 2.399963;                    /* golden angle */
        tint[i] = rgb((uint32_t)(128 + 127 * sind(a)), (uint32_t)(128 + 127 * sind(a + 2.094)),
                      (uint32_t)(128 + 127 * sind(a + 4.189)));
    }
}

/* ---- rendering --------------------------------------------------------------- */

static uint32_t blend(uint32_t a, uint32_t b)   /* 50/50, per 8-bit channel */
{
    return ((a >> 1) & 0x7f7f7f7fu) + ((b >> 1) & 0x7f7f7f7fu);
}

static void render_row(uint32_t y, uint32_t me)
{
    uint32_t *out = back + (uint64_t)y * W;
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
        out[x] = fr.tinted ? blend(c, tint[me]) : c;
    }
}

static void copy_row(uint32_t y)
{
    const uint32_t *src = back + (uint64_t)y * W;
    for (int k = 0; k < 2; k++) {
        volatile uint32_t *dst = (volatile uint32_t *)(fbmem + (uint64_t)(2 * y + k) * scr.pitch);
        for (uint32_t x = 0; x < W; x++) {
            dst[2 * x] = src[x];
            dst[2 * x + 1] = src[x];
        }
    }
}

/* One phase of work on this thread: rows from the shared counter. */
#define CHUNK 8   /* rows per grab: visible bands in scene 3, fewer atomics */

static void work(uint32_t ph, uint32_t me)
{
    uint32_t y0;
    while ((y0 = __atomic_fetch_add(&next_row, CHUNK, __ATOMIC_RELAXED)) < H) {
        for (uint32_t y = y0; y < y0 + CHUNK && y < H; y++) {
            if (ph & 1) {
                render_row(y, me);
                rows_by[me]++;
            } else {
                copy_row(y);
            }
        }
    }
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}

static void worker(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg, seen = 0;
    for (;;) {
        uint32_t ph;
        while ((ph = __atomic_load_n(&phase, __ATOMIC_ACQUIRE)) == seen && !stop)
            __builtin_ia32_pause();
        if (stop)
            return;
        seen = ph;
        work(ph, me);
    }
}

/* Run one phase on every thread (this one too) and wait for all of them. */
static void run_phase(void)
{
    next_row = 0;
    done = 0;
    uint32_t ph = __atomic_add_fetch(&phase, 1, __ATOMIC_RELEASE);
    work(ph, 0);
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < nthreads)
        __builtin_ia32_pause();
}

/* ---- the overlay ------------------------------------------------------------- */

static void text(uint32_t x0, uint32_t y0, const char *s, uint32_t fg)
{
    for (; *s; s++, x0 += 8) {
        const uint8_t *g = font_8x16[(uint8_t)*s & 0x7f];
        for (uint32_t y = 0; y < 16 && y0 + y < H; y++)
            for (uint32_t x = 0; x < 8 && x0 + x < W; x++)
                if (g[y] & (0x80 >> x))
                    back[(uint64_t)(y0 + y) * W + x0 + x] = fg;
    }
}

static void shade(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    for (uint32_t y = y0; y < y0 + h && y < H; y++)
        for (uint32_t x = x0; x < x0 + w && x < W; x++) {
            uint32_t *p = &back[(uint64_t)y * W + x];
            *p = (*p >> 2) & 0x3f3f3f3fu;   /* a quarter as bright */
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
    uint32_t bw = 8 * 58, bh = 16 * 4 + 12 + 40;
    shade(8, 8, bw, bh);
    text(16, 12, "JAM OS  -  VISUAL DEMO", rgb(255, 210, 90));
    snprintf(line, sizeof(line), "%s", scene);
    text(16, 30, line, rgb(230, 230, 240));
    int f10 = (int)(fps * 10);
    if (fr.kind == JULIA)
        snprintf(line, sizeof(line), "%u CPUs   %d.%d fps   %d iterations", scr.ncpu, f10 / 10,
                 f10 % 10, fr.maxit);
    else
        snprintf(line, sizeof(line), "%u CPUs   %d.%d fps   zoom %sx   %d iterations", scr.ncpu,
                 f10 / 10, f10 % 10, z, fr.maxit);
    text(16, 48, line, rgb(170, 200, 255));
    text(16, 66, "rows drawn per thread this frame:", rgb(150, 150, 170));
    /* A bar per thread: how many rows it took (dynamic balancing at work). */
    uint32_t maxr = 1;
    for (uint32_t t = 0; t < nthreads; t++)
        if (rows_by[t] > maxr)
            maxr = rows_by[t];
    uint32_t bx = 16, by = 86, barw = (bw - 16) / (nthreads ? nthreads : 1);
    for (uint32_t t = 0; t < nthreads; t++) {
        uint32_t hgt = rows_by[t] * 28 / maxr;
        for (uint32_t y = 0; y < hgt; y++)
            for (uint32_t x = 1; x + 1 < barw; x++)
                back[(uint64_t)(by + 28 - y) * W + bx + t * barw + x] = tint[t];
    }
}

/* ---- scenes -------------------------------------------------------------------- */

struct result { const char *name; uint32_t frames; uint64_t ns; };

static double now_s(uint64_t t0) { return (double)((uint64_t)jam_clock_get() - t0) / 1e9; }

static struct result scene(const char *name, double seconds, int which)
{
    uint64_t t0 = (uint64_t)jam_clock_get(), last = t0;
    uint32_t frames = 0;
    double fps = 0, zoom = 1;
    while (now_s(t0) < seconds) {
        double t = now_s(t0);
        for (uint32_t i = 0; i < nthreads; i++)
            rows_by[i] = 0;
        fr.tinted = which == 3;
        fr.shift = t * 60;
        if (which == 1) {           /* Seahorse Valley, 1.5 -> ~1e-13 across the scene */
            double scale = 1.5 * exp2d(-t / seconds * 43.0);
            fr.kind = MANDEL;
            fr.cx = -0.743643887037158704752191506114774;
            fr.cy = 0.131825904205311970493132056385139;
            fr.px = 2 * scale / W;
            zoom = 1.5 / scale;
            fr.maxit = 250 + (int)(55 * log2d(zoom));
        } else if (which == 2) {    /* Julia: c walks round a circle */
            double a = t / seconds * 2 * 3.141592653589793 + 0.4;
            fr.kind = JULIA;
            fr.jr = 0.7885 * cosd(a);
            fr.ji = 0.7885 * sind(a);
            fr.cx = 0;
            fr.cy = 0;
            fr.px = 3.2 / W;
            zoom = 1;
            fr.maxit = 300;
        } else {                    /* rows tinted by the thread that drew them */
            double scale = 0.012 * exp2d(-2 * sind(t * 0.5));
            fr.kind = MANDEL;
            fr.cx = -0.7453;
            fr.cy = 0.1127;
            fr.px = 2 * scale / W;
            zoom = 1.5 / scale;
            fr.maxit = 700;
        }
        run_phase();   /* render */
        uint64_t n = (uint64_t)jam_clock_get();
        if (n > last)
            fps = fps ? fps * 0.8 + 0.2 * (1e9 / (double)(n - last)) : 1e9 / (double)(n - last);
        last = n;
        overlay(which == 1   ? "1/3  Mandelbrot deep zoom: Seahorse Valley"
                : which == 2 ? "2/3  Julia set, c = 0.7885 e^(ia)"
                             : "3/3  who drew what: each colour band is one thread",
                fps, zoom);
        run_phase();   /* copy to the screen */
        frames++;
    }
    return (struct result){ name, frames, (uint64_t)jam_clock_get() - t0 };
}

static bool parse(const char *s)
{
    if (strncmp(s, "demo:", 5))
        return false;
    s += 5;
    uint64_t v[8] = { 0 };   /* phys, width, height, pitch, r, g, b shifts, cpus */
    for (int k = 0; k < 8; k++) {
        int base = k == 0 ? 16 : 10;
        if (!*s)
            return false;
        while (*s && *s != ':') {
            int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
            if (d < 0 || d >= base)
                return false;
            v[k] = v[k] * base + d;
            s++;
        }
        if (*s == ':')
            s++;
    }
    scr.phys = v[0];
    scr.w = v[1];
    scr.h = v[2];
    scr.pitch = v[3];
    scr.rs = v[4];
    scr.gs = v[5];
    scr.bs = v[6];
    scr.ncpu = v[7] ? v[7] : 1;
    return scr.w >= 64 && scr.h >= 64 && scr.pitch >= scr.w * 4 && !(scr.phys & 4095);
}

static status_t map(handle_t vmo, uint64_t len, void **out)
{
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ | VMAR_WRITE,
                               &addr);
    *out = (void *)(uintptr_t)addr;
    return st;
}

static void report_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report_line(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jam_debug_report(buf, n > 0 ? (uint64_t)n : 0);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !parse(argv[1])) {
        report_line("demo: bad screen description");
        return 2;
    }
    W = scr.w / 2;
    H = scr.h / 2;
    uint64_t fblen = ((uint64_t)scr.pitch * scr.h + 4095) & ~4095ull, blen = (uint64_t)W * H * 4;
    handle_t fbv, bv;
    status_t st = jam_vmo_create_physical(startup_handle(SR_RESOURCE), scr.phys, fblen,
                                          VMO_CACHE_WC, &fbv);
    void *p;
    if (st == OK)
        st = map(fbv, fblen, &p);
    if (st != OK) {
        report_line("demo: can't map the framebuffer (%s)", status_str(st));
        return 1;
    }
    fbmem = p;
    if ((st = jam_vmo_create((blen + 4095) & ~4095ull, 0, HANDLE_INVALID, &bv)) != OK ||
        (st = map(bv, (blen + 4095) & ~4095ull, &p)) != OK) {
        report_line("demo: no back buffer (%s)", status_str(st));
        return 1;
    }
    back = p;
    make_palette();

    nthreads = scr.ncpu > MAX_THREADS ? MAX_THREADS : scr.ncpu;
    for (uint32_t i = 1; i < nthreads; i++) {
        void *stack = malloc(STACK);
        handle_t th;
        if (!stack || thread_spawn("demo", worker, (void *)(uintptr_t)i, stack, STACK, &th) != OK) {
            nthreads = i;   /* fewer helpers, still works */
            break;
        }
        jam_handle_close(th);
    }

    struct result r[3];
    r[0] = scene("Mandelbrot deep zoom", 40, 1);
    r[1] = scene("Julia set", 22, 2);
    r[2] = scene("who drew what", 14, 3);
    stop = true;

    report_line("demo: %ux%u screen, %ux%u rendered, %u threads on %u CPUs", scr.w, scr.h, W, H,
                nthreads, scr.ncpu);
    for (int i = 0; i < 3; i++) {
        uint64_t f10 = r[i].ns ? (uint64_t)r[i].frames * 10000000000ull / r[i].ns : 0;
        report_line("demo: %-20s %u frames, %lu.%lu fps", r[i].name, r[i].frames,
                    (unsigned long)(f10 / 10), (unsigned long)(f10 % 10));
    }
    return 0;
}
