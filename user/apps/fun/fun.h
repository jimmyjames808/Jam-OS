/* libfun: the fun apps' shared code (user/apps/fun).
 *
 * libfun.a, linked into the programs that use it (bin/life, bin/tetris,
 * bin/fractal, bin/demo), one object per job: gfx.c (the screen and
 * drawing), text.c, keys.c, pool.c (CPUs and the thread pool), util.c
 * (maths, memory, output, arguments, self-tests).
 *
 * The screen: the apps draw real pixels. gfx_open borrows the framebuffer
 * from the console (console.lend_screen through SR_CONSOLE: a
 * write-combining VMO of the framebuffer, its geometry and a lease
 * channel) and takes the keyboard focus (console.open_keys). The app draws
 * into a full-resolution RAM back buffer (scr.s: 0xRRGGBB pixels);
 * gfx_present copies the parts that changed since the last present to the
 * screen, compared against a RAM copy of what the screen shows, in 64-pixel
 * row pieces, on every CPU. The framebuffer itself is never read (reads of
 * write-combining memory are very slow). gfx_close closes the lease and
 * the key channel: the console redraws its text screen and the keys go
 * back to the shell. If the app dies instead (a crash, a kill, Ctrl+C: the
 * shell kills the job), the kernel closes those handles and the console
 * does the same. All of the screen and key hand-off is in gfx_open /
 * gfx_close.
 *
 * Also here: drawing (rectangles, blending, gradients, lines, blits), a
 * proportional text renderer made from the 8x16 console font (each glyph's
 * ink width, fixed-width digits, integer scaling smoothed with scale2x),
 * an FPS counter, a little maths without libm, the CPU count (CPUID; there
 * is no system call for it), a thread pool that runs `items` of work on
 * every CPU (workers spin briefly between jobs, then sleep on an event),
 * and key decoding for both key sources (a USB keyboard: HID usages; a
 * serial terminal: codepoints). */
#pragma once

#include <os.h>

/* ---- colours: 0xRRGGBB ------------------------------------------------------------ */

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
/* a + (b - a) * t / 256, per channel (t 0..256). */
static inline uint32_t mixc(uint32_t a, uint32_t b, uint32_t t)
{
    uint32_t rb = (a & 0xff00ff) * (256 - t) + (b & 0xff00ff) * t;
    uint32_t g = (a & 0x00ff00) * (256 - t) + (b & 0x00ff00) * t;
    return ((rb >> 8) & 0xff00ff) | ((g >> 8) & 0x00ff00);
}
/* Each channel * t / 256 (t 0..256: darker; up to 512: brighter, saturating). */
static inline uint32_t scalec(uint32_t c, uint32_t t)
{
    return rgb((c >> 16 & 0xff) * t >> 8, (c >> 8 & 0xff) * t >> 8, (c & 0xff) * t >> 8);
}

/* ---- surfaces and the screen ------------------------------------------------------- */

struct surf {
    uint32_t *px;
    int w, h, stride;   /* stride in pixels */
};

struct screen {
    struct surf s;      /* the back buffer: draw here, then gfx_present */
    int w, h;           /* screen pixels */
    int ui;             /* a UI scale for text: 1 up to 1080 lines, 2 above */
    /* private */
    uint32_t *shown;    /* what the screen shows (RAM copy) */
    uint32_t *fb;       /* the framebuffer (write-combining: written, never read) */
    uint32_t pitch;     /* framebuffer bytes per line */
    uint8_t  rs, gs, bs;
    bool     native;    /* the framebuffer is 0xRRGGBB too: copied as it is */
    handle_t con, keys, lease;
    bool     open;
    uint64_t presents, bytes;   /* stats: presents, bytes written to the screen */
};
extern struct screen scr;

/* Take the keys and borrow the screen; fills scr. The pool (pool_start)
 * should be started first: gfx_present runs on it. */
status_t gfx_open(void);
/* Give the screen and the keys back (the console redraws its text). */
void     gfx_close(void);
/* Copy what changed in scr.s to the screen. */
void     gfx_present(void);
/* Copy all of it (after something else may have drawn on the screen). */
void     gfx_present_all(void);

/* Drawing (all clipped to the surface). */
void fill(const struct surf *s, int x, int y, int w, int h, uint32_t c);
/* Blend c over the rectangle at alpha a (0..256). */
void blend(const struct surf *s, int x, int y, int w, int h, uint32_t c, uint32_t a);
/* A filled rectangle with rounded corners, blended at alpha a; r: corner radius. */
void panel(const struct surf *s, int x, int y, int w, int h, int r, uint32_t c, uint32_t a);
/* A rectangle's outline, t pixels thick. */
void frame(const struct surf *s, int x, int y, int w, int h, int t, uint32_t c);
/* A vertical gradient from c0 (top) to c1 (bottom). */
void vgrad(const struct surf *s, int x, int y, int w, int h, uint32_t c0, uint32_t c1);
void line(const struct surf *s, int x0, int y0, int x1, int y1, uint32_t c);
/* src (w x h at sx, sy) to dst at x, y; blit_key skips pixels == key. */
void blit(const struct surf *dst, int x, int y, const struct surf *src, int sx, int sy, int w,
          int h);
void blit_key(const struct surf *dst, int x, int y, const struct surf *src, uint32_t key);
/* A surface of its own memory (zeroed); px NULL on failure. */
struct surf surf_new(int w, int h);

/* Text: the 8x16 font, proportional (each glyph as wide as its ink, digits
 * all the same width), scale 1..8 (smoothed above 1). text() returns the x
 * after the text; text_shadow draws a soft dark shadow first (for text over
 * pictures). A '\a' in the text switches to the colour `alt` until the next
 * '\a' (textf takes it as a normal character). */
int  text(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str);
int  text_shadow(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str);
int  text2(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt, bool shadow,
           const char *str);
int  textf(const struct surf *s, int x, int y, int scale, uint32_t c, const char *fmt, ...)
         __attribute__((format(printf, 6, 7)));
int  text_width(int scale, const char *str);
#define TEXT_H(scale) (16 * (scale))

/* Frames per second over the last half second or so, x10. */
struct fps {
    uint64_t t0;
    uint32_t n, x10;
};
void fps_frame(struct fps *f);

/* ---- keys ---------------------------------------------------------------------------- */

enum {
    KEY_NONE = 0,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_PGUP, KEY_PGDN, KEY_HOME,
    KEY_QUIT,   /* Esc or Ctrl+C */
};
/* The next key press (DOWN or REPEAT) before deadline: a KEY_* code or a
 * character; KEY_NONE on timeout (deadline 0: don't wait). KEY_QUIT too if
 * the key channel broke. */
int      gfx_key(uint64_t deadline);
/* One key event decoded (KEY_NONE for a release or a bare modifier). */
int      key_decode(const struct input_key_event *ev);
/* The next key event itself (down, repeat or up) before deadline: OK;
 * ERR_TIMED_OUT when none came (deadline 0: don't wait); another error
 * when the key channel broke. */
status_t gfx_key_event(uint64_t deadline, struct input_key_event *ev);

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

#define FUN_MAX_THREADS 64

/* Logical CPUs in the package, from CPUID leaf 0x1F/0xB (1..64). */
uint32_t fun_cpu_count(void);
/* AVX2 + FMA usable here (CPUID and the OS's XCR0). */
bool     fun_has_avx2(void);
/* Running under QEMU's emulator (TCG), which is slow at AVX. */
bool     fun_is_tcg(void);
/* Start n - 1 workers (n = 0: one per CPU); the calling thread is worker
 * 0. Returns how many threads the pool has. */
uint32_t pool_start(uint32_t n);
uint32_t pool_threads(void);
/* Run fn(item, worker, arg) for item = 0 .. items - 1 on every pool
 * thread (items handed out one at a time), and wait for all of them. */
void     pool_run(void (*fn)(uint32_t item, uint32_t worker, void *arg), void *arg, uint32_t items);
/* Work items each thread did in the last pool_run. */
extern uint32_t pool_items_by[FUN_MAX_THREADS];

/* ---- maths without libm ------------------------------------------------------------------ */

static inline double sqrtd(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline float sqrtf_(float x)
{
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
double log2d(double x);   /* x > 0 */
double exp2d(double x);
double sind(double x);
static inline double cosd(double x) { return sind(x + 1.5707963267948966); }
static inline double floord(double x)
{
    double t = (double)(int64_t)x;
    return t > x ? t - 1 : t;
}

/* ---- odds and ends ---------------------------------------------------------------------- */

/* xorshift64*: a fast deterministic generator. */
static inline uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545f4914f6cdd1dull;
}

static inline uint32_t popcount64(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (uint32_t)((x * 0x0101010101010101ull) >> 56);
}

/* A zeroed, page-aligned block of memory of its own VMO (for buffers
 * bigger than the heap likes); NULL on failure. */
void    *big_alloc(uint64_t bytes);
/* Text to the console (also mirrored to COM1, so the QEMU tests see it)
 * and to the kernel log. Not while the screen is borrowed (it isn't
 * drawn then, but it is kept and shown when the screen comes back). */
void     say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "1,234,567" */
char    *commas(char *buf, size_t n, uint64_t v);
bool     has_arg(int argc, char **argv, const char *name);
/* The number after "name=" in argv, or def. */
uint64_t arg_num(int argc, char **argv, const char *name, uint64_t def);

/* Self-tests (`--selftest`): fun_selftest_begin names the app and how wide
 * a check's description is padded; fun_check says one line, "<app>:
 * selftest: <what> ok" (or FAILED); fun_selftest_end says the verdict and
 * returns the exit code (0: every check passed). */
void     fun_selftest_begin(const char *app, int width);
void     fun_check(bool ok, const char *what);
int      fun_selftest_end(void);
