/* libfun: the fun apps' shared code (user/apps/fun).
 *
 * libfun.a, linked into the programs that use it (bin/life, bin/tetris,
 * bin/fractal, bin/demo, ...), one object per job: gfx.c (the screen and
 * drawing), text.c, keys.c (the key channel), mouse.c (the pointer and its
 * arrow), pool.c (CPUs and the thread pool), util.c (maths, memory, output,
 * arguments, self-tests).
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
 * Also here: drawing (rectangles, blending, gradients, lines, blits;
 * premultiplied alpha and anti-aliased shapes in alpha.c, scaling in
 * scale.c), a
 * proportional text renderer made from the 8x16 console font (each glyph's
 * ink width, fixed-width digits, integer scaling smoothed with scale2x),
 * an FPS counter, a little maths without libm, the CPU count (CPUID; there
 * is no system call for it), a thread pool that runs `items` of work on
 * every CPU (workers spin briefly between jobs, then sleep on an event;
 * at once when the app waits),
 * key decoding for both key sources (a USB keyboard: HID usages; a
 * serial terminal: codepoints), and the mouse for the apps that ask for it
 * (gfx_mouse_open): a pointer position, the buttons and the wheel, and an
 * arrow drawn over the app's frame. */
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
    uint32_t *px;       /* 0xRRGGBB pixels, row after row */
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
    uint8_t  rs, gs, bs;   /* the framebuffer's red, green, blue bit positions */
    bool     native;    /* the framebuffer is 0xRRGGBB too: copied as it is */
    handle_t con, keys, lease;   /* the console, our key channel, the screen's lease */
    bool     open;      /* gfx_open succeeded, gfx_close not called yet */
    uint64_t presents, bytes;   /* stats: presents, bytes written to the screen */
};
extern struct screen scr;

/* Take the keys and borrow the screen; fills scr. The pool (pool_start)
 * should be started first: gfx_present runs on it. */
status_t gfx_open(void);
/* The same with the back buffer, and so the screen, all colour bg at
 * first (gfx_open: black): no black frame on its way to bg. */
status_t gfx_open_on(uint32_t bg);
/* The screen only, the keys left with whoever has them (the boot splash:
 * what is typed meanwhile waits for the shell). gfx_key then returns
 * KEY_QUIT at once: don't wait with it. */
status_t gfx_open_screen(uint32_t bg);
/* Give the screen and the keys back (the console redraws its text). */
void     gfx_close(void);
/* Copy what changed in scr.s to the screen. */
void     gfx_present(void);
/* Copy all of it (after something else may have drawn on the screen). */
void     gfx_present_all(void);

/* A rectangle on a surface: what the drawing calls take, what a layout
 * hands out and what a click is tested against. */
struct rect {
    int x, y, w, h;     /* top-left corner and size, pixels */
};
static inline bool rect_has(const struct rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* Drawing (all clipped to the surface). */
void fill(const struct surf *s, int x, int y, int w, int h, uint32_t c);
static inline void fill_rect(const struct surf *s, const struct rect *r, uint32_t c)
{
    fill(s, r->x, r->y, r->w, r->h, c);
}
/* Blend c over r at alpha a (0..256). */
void blend(const struct surf *s, const struct rect *r, uint32_t c, uint32_t a);
/* r filled with rounded corners (of radius `radius`), blended at alpha a. */
void panel(const struct surf *s, const struct rect *r, int radius, uint32_t c, uint32_t a);
/* r's outline, t pixels thick. */
void frame(const struct surf *s, const struct rect *r, int t, uint32_t c);
/* r filled with a vertical gradient from c0 (top) to c1 (bottom). */
void vgrad(const struct surf *s, const struct rect *r, uint32_t c0, uint32_t c1);
void line(const struct surf *s, int x0, int y0, int x1, int y1, uint32_t c);
/* The part `from` of src to dst with its top left at x, y; blit_key: all
 * of src, skipping pixels == key. */
void blit(const struct surf *dst, int x, int y, const struct surf *src, const struct rect *from);
void blit_key(const struct surf *dst, int x, int y, const struct surf *src, uint32_t key);
/* A surface of its own memory (zeroed); px NULL on failure. */
struct surf surf_new(int w, int h);

/* Alpha blending with premultiplied alpha (alpha.c): a colour that is
 * drawn with alpha is 0xAARRGGBB with r, g and b already multiplied by
 * a / 255, and drawing it over a surface pixel d gives
 * src + d * (255 - a) / 255 per channel (exact, rounded). Surfaces stay
 * opaque 0xRRGGBB. */
/* rgb (0xRRGGBB) at alpha a (0..255) as a premultiplied colour. */
uint32_t argb_pm(uint32_t rgb, uint32_t a);
/* src (premultiplied) over the opaque pixel dst. */
uint32_t px_over(uint32_t dst, uint32_t src);
/* src (premultiplied) over every pixel of the rectangle (SSE2). */
void fill_pm(const struct surf *s, int x, int y, int w, int h, uint32_t src);
/* An image whose pixels are premultiplied 0xAARRGGBB, over dst at x, y. */
void blit_pm(const struct surf *dst, int x, int y, const struct surf *src);
/* Premultiplied pixels to read from: w x h of them, `stride` a row. */
struct picture {
    const uint32_t *px;
    int             w, h, stride;
};
/* src to dw x dh at dst (dw a row): area averaging to make smaller,
 * bilinear to make bigger (scale.c). */
void scale_pm(const struct picture *src, uint32_t *dst, int dw, int dh);
/* Anti-aliased (each edge pixel's coverage as alpha), in rgb at alpha a
 * (0..255), positions in pixels (a pixel's centre is at +0.5): a filled
 * circle of radius r, and a line `width` pixels wide with round ends. */
void disc_aa(const struct surf *s, float cx, float cy, float r, uint32_t rgb, uint32_t a);
void line_aa(const struct surf *s, float x0, float y0, float x1, float y1, float width,
             uint32_t rgb, uint32_t a);
/* The same: a circle's outline `width` pixels wide centred on radius r;
 * and a convex polygon of n (3..16) corners, xy[2 * i], xy[2 * i + 1],
 * either winding. */
void ring_aa(const struct surf *s, float cx, float cy, float r, float width, uint32_t rgb,
             uint32_t a);
void poly_aa(const struct surf *s, const float *xy, int n, uint32_t rgb, uint32_t a);

/* A block of colour c with an edge e pixels wide, lit from the top left so
 * it stands out of the surface (e < 0: lit from the bottom right, so it is
 * sunk into it), and a soft gradient down its face. */
void bevel(const struct surf *s, const struct rect *r, int e, uint32_t c);
/* A soft glow of colour c round r, fading out over `reach` pixels: what a
 * playing field sits in. Drawn before the field itself. */
void glow(const struct surf *s, const struct rect *r, int reach, uint32_t c);
/* A dark card with rounded corners and a lighter outline: the panel that
 * numbers and help sit on. */
void card(const struct surf *s, const struct rect *r, int radius);
/* A filled circle of radius rad around (cx, cy), blended at alpha a. */
static inline void disc(const struct surf *s, int cx, int cy, int rad, uint32_t c, uint32_t a)
{
    panel(s, &(struct rect){ cx - rad, cy - rad, 2 * rad, 2 * rad }, rad, c, a);
}

/* Text: the 8x16 font, proportional (each glyph as wide as its ink, digits
 * all the same width), scale 1..8 (smoothed above 1). The text is UTF-8:
 * ASCII and U+00A0 .. U+017F (Latin-1 and Latin Extended-A) have glyphs;
 * any other code point, and each malformed byte, draws one box. text()
 * returns the x after the text; text_shadow draws a soft dark shadow first
 * (for text over pictures). A '\a' in the text switches to the colour `alt`
 * until the next '\a' (textf takes it as a normal character). text_clip
 * draws it at r's top left, at most r->w pixels of it, cut short with
 * "..." if it is wider (r->h is not looked at: give it TEXT_H(scale)). */
int  text(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str);
int  text_shadow(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str);
int  text2(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt, bool shadow,
           const char *str);
int  textf(const struct surf *s, int x, int y, int scale, uint32_t c, const char *fmt, ...)
         __attribute__((format(printf, 6, 7)));
int  text_width(int scale, const char *str);
int  text_clip(const struct surf *s, const struct rect *r, int scale, uint32_t c,
               const char *str);
/* The code point at *s, moving *s past it (past one byte, with UTF8_BAD,
 * when it is malformed; 0 at the end, not moving). */
#define UTF8_BAD 0xfffdu
uint32_t utf8_next(const char **s);
#define TEXT_H(scale) (16 * (scale))
/* The text centred in r, both ways. */
void text_in(const struct surf *s, const struct rect *r, int scale, uint32_t c, const char *str);

/* Frames per second over the last half second or so, x10. */
struct fps {
    uint64_t t0;        /* the current count's start (uptime ns) */
    uint32_t n, x10;    /* frames counted since; the last rate, x10 */
};
void fps_frame(struct fps *f);

/* ---- keys ---------------------------------------------------------------------------- */

enum {
    KEY_NONE = 0,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_PGUP, KEY_PGDN, KEY_HOME,
    KEY_QUIT,   /* Esc or Ctrl+C */
    KEY_MOUSE,  /* not a key: the mouse did something (gfx_mouse says what) */
    KEY_BACKSPACE, KEY_TAB, KEY_END, KEY_DELETE,
};
/* The next key press (DOWN or REPEAT) before deadline: a KEY_* code or a
 * character; KEY_NONE on timeout (deadline 0: don't wait). KEY_QUIT too if
 * the key channel broke. After gfx_mouse_open it is also how the mouse is
 * waited for: KEY_MOUSE when it moved (the movement queued so far, merged)
 * or when a button or the wheel changed (one such change at a time). */
int      gfx_key(uint64_t deadline);
/* One key event decoded (KEY_NONE for a release or a bare modifier). */
int      key_decode(const struct input_key_event *ev);
/* The next key event itself (down, repeat or up) before deadline: OK;
 * ERR_TIMED_OUT when none came (deadline 0: don't wait); another error
 * when the key channel broke. Mouse reports that come meanwhile are kept
 * for gfx_mouse (and the next gfx_key says KEY_MOUSE). */
status_t gfx_key_event(uint64_t deadline, struct input_key_event *ev);

/* ---- the mouse ------------------------------------------------------------------------- */

enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 4 };   /* = INPUT_BTN_* */

struct mouse {
    int      x, y;       /* the pointer (the arrow's tip), screen pixels */
    uint8_t  buttons;    /* MOUSE_* held down now */
    uint8_t  pressed;    /* ... that went down since the last gfx_mouse */
    uint8_t  released;   /* ... that came up since */
    int      wheel;      /* notches turned since (+: away from the user) */
    bool     moved;      /* the pointer moved since */
    uint32_t reports;    /* mouse reports ever seen: 0 means no mouse has stirred yet */
};

/* Ask the console for the mouse (after gfx_open; gfx_close ends it). The
 * pointer starts at the middle of the screen; its arrow shows from the
 * first report on. accel: quick movement goes further (false: one pixel
 * a count whatever the speed, which the tests need). */
status_t gfx_mouse_open(bool accel);
/* The mouse now, and what it did since the last call. */
void     gfx_mouse(struct mouse *out);
/* Hide or show the arrow (shown by default once the mouse has moved). */
void     gfx_pointer_show(bool on);
/* Only the arrow moved: present just its rows (much cheaper than
 * gfx_present when the frame is unchanged). */
void     gfx_present_pointer(void);

/* A pointer position from relative mouse counts: what gfx_mouse keeps,
 * here by itself so it can be tested. */
struct pointer {
    int32_t x256, y256;   /* the position in 1/256 pixel, inside the screen */
    int     w, h;         /* the screen it is clamped to */
    bool    accel;        /* quick movement is amplified (mouse.c says how) */
    int     speed;        /* with accel: the gain in percent (100: as it is) */
};
/* In the middle of a w x h screen. */
void pointer_init(struct pointer *p, int w, int h, bool accel);
/* One report's movement. */
void pointer_move(struct pointer *p, int dx, int dy);
static inline int pointer_x(const struct pointer *p) { return p->x256 >> 8; }
static inline int pointer_y(const struct pointer *p) { return p->y256 >> 8; }

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
/* The app is about to wait (for keys, a frame's time): the workers sleep
 * now instead of spinning for the next batch. gfx_key and gfx_key_event
 * call it before they block; an app that waits some other way calls it
 * itself. */
void     pool_rest(void);
/* For the self-tests: the pauses a worker spins between batches before it
 * sleeps (0: the default, about 1 ms), and how many workers sleep now. */
void     pool_set_spin(uint32_t pauses);
uint32_t pool_asleep(void);

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
