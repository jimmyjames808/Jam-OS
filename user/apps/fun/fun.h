/* libfun: the fun apps' shared code (user/apps/fun).
 *
 * libfun.a, linked into the programs that use it (bin/life, bin/tetris,
 * bin/fractal, bin/demo, ...), one object per job: gfx.c (the screen and
 * drawing), wl.c and wlpaint.c (a window on the compositor instead),
 * text.c (the 8x16 text), utf8.c, font.c, fontdraw.c, fontdata.c and
 * ttf.c (the smooth text), keys.c (the key channel), mouse.c (the pointer
 * and its arrow), pool.c (CPUs and the thread pool), util.c (maths,
 * memory, output, arguments, self-tests).
 *
 * The screen: the apps draw real pixels into a RAM back buffer (scr.s:
 * 0xRRGGBB pixels), and gfx_present shows what changed since the last
 * present, compared against a RAM copy of what is shown, in 64-pixel row
 * pieces, on every CPU. Where it shows them depends on what the program
 * was given:
 *   - a window (wl.c, wlpaint.c), when its namespace has the compositor
 *     (/svc/wayland, the list's `svc wayland`) and the compositor offers
 *     windows: gfx_open opens one (an xdg_toplevel through libjwl,
 *     <jwl_client.h>), present copies the changed pieces into the free one
 *     of the window's two shared buffers, damages them and commits, one
 *     commit per frame callback; keys and the mouse come from the seat,
 *     the close box is KEY_QUIT, and the compositor draws the pointer;
 *   - otherwise the whole screen, borrowed from the console as before
 *     there was a compositor (the `nocomp` boot, or no compositor offering
 *     windows): console.lend_screen through SR_CONSOLE gives a
 *     write-combining VMO of the framebuffer, its geometry and a lease
 *     channel, and console.open_keys the keyboard focus. The framebuffer
 *     itself is never read (reads of write-combining memory are very
 *     slow). gfx_close closes the lease and the key channel: the console
 *     redraws its text screen and the keys go back to the shell. If the
 *     app dies instead (a crash, a kill, Ctrl+C: the shell kills the job),
 *     the kernel closes those handles and the console does the same.
 * The app's code is the same either way: all of the hand-off is in
 * gfx_open / gfx_close, and scr.w and scr.h are the size it draws at (a
 * window's size, or the screen's).
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
    int w, h;           /* pixels drawn at: the window's or the screen's */
    int ui;             /* a UI scale for text: 1 up to 1100 lines (h), 2 above */
    /* private */
    uint32_t *shown;    /* what the screen or the window shows (RAM copy) */
    uint32_t *fb;       /* the framebuffer (write-combining: written, never read) */
    uint64_t fb_len;    /* bytes mapped at fb (the borrowed screen's VMO) */
    uint32_t pitch;     /* framebuffer bytes per line */
    uint8_t  rs, gs, bs;   /* the framebuffer's red, green, blue bit positions */
    bool     native;    /* the framebuffer is 0xRRGGBB too: copied as it is */
    handle_t con, keys, lease;   /* the console, our key channel, the screen's lease */
    bool     open;      /* gfx_open succeeded, gfx_close not called yet */
    bool     windowed;  /* a window on the compositor (wl.c), not the borrowed screen */
    uint32_t bg;        /* the colour opened on: round a window's picture when they differ */
    uint64_t presents, bytes;   /* stats: presents, bytes written to the screen or window */
};
extern struct screen scr;

/* Open a window, or else take the keys and borrow the screen (above);
 * fills scr. The pool (pool_start) should be started first: gfx_present
 * runs on it. The window is the size the app asked for (gfx_window_size),
 * else $FUN_WINDOW ("<w>x<h>"), else 1600x1000 on a screen bigger than
 * 1920x1200 and the whole screen (maximised) on smaller ones. Errors: the
 * screen's (ERR_NOT_FOUND: no console either), ERR_NO_MEMORY. */
status_t gfx_open(void);
/* The same with the back buffer, and so the window or the screen, all
 * colour bg at first (gfx_open: black): no black frame on its way to bg. */
status_t gfx_open_on(uint32_t bg);
/* The same as a full-screen window (the screen when borrowed): scr is
 * the screen's size. */
status_t gfx_open_fullscreen(uint32_t bg);
/* Full screen without the keys, which stay with whoever has them (the
 * boot splash: what is typed meanwhile waits for the shell). gfx_key then
 * returns KEY_QUIT at once: don't wait with it. */
status_t gfx_open_screen(uint32_t bg);
/* Give the window up (its back buffer goes with it: scr.s is drawn on no
 * more), or the screen and the keys back (the console redraws its text). */
void     gfx_close(void);
/* Before gfx_open: the window's title (and app id; NULL or unset: "Jam OS"),
 * and the size the app would like its window (w, h of at least 320x200;
 * 0: the default above). Kept by pointer: a string that stays. */
void     gfx_title(const char *title);
void     gfx_window_size(int w, int h);
/* Before gfx_open: the app takes any size its window is given (the
 * compositor's resize, maximise, full screen). gfx_key then says
 * KEY_RESIZE, after which scr.w, scr.h and scr.s are new and the app draws
 * the whole picture again. Without it a window keeps the app's size, and
 * a bigger window (full screen by the compositor's key) shows the picture
 * centred on scr.bg. */
void     gfx_resizable(void);
/* Tests: how gfx_open reaches the compositor instead of /svc/wayland (a
 * libjwl connect function, <jwl_client.h>); NULL: /svc/wayland again. */
void     gfx_connect_with(status_t (*connect)(void *ctx, handle_t *out), void *ctx);
/* Tests: borrow the screen from this console channel (a fake's) instead
 * of SR_CONSOLE, and ask for no window; HANDLE_INVALID: as usual again.
 * The channel stays the caller's. */
void     gfx_console_with(handle_t con);
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
/* n premultiplied pixels of src over the n opaque pixels of dst, in place,
 * exactly as px_over (over.c): AVX2 where it pays, else SSE2. The two
 * versions by name, for fbbench to compare. Rows need no alignment. */
void px_over_row(uint32_t *dst, const uint32_t *src, int n);
void px_over_row_sse2(uint32_t *dst, const uint32_t *src, int n);
void px_over_row_avx2(uint32_t *dst, const uint32_t *src, int n);   /* AVX2 CPUs only */
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

/* ---- smooth text -------------------------------------------------------------------- */

/* Anti-aliased proportional text in Inter (third_party/inter: Regular and
 * Medium, cut to printable Latin-1 and a little punctuation, linked in:
 * fontdata.c), for the compositor's title bars and top bar (font.c bakes,
 * fontdraw.c measures and draws). The terminal and the apps keep the 8x16
 * text above.
 *
 * A struct font is one weight at one size with every glyph baked: at
 * font_open, stb_truetype (third_party/stb_truetype) renders each glyph's
 * coverage at four horizontal positions a quarter pixel apart, into one
 * block of memory that is then made read-only. Measuring and drawing only
 * read it: they allocate nothing, write nothing but the surface's pixels,
 * and may run on any number of threads at once, once the font has reached
 * them (open it before the threads start, or hand it over with a release
 * store, or through pool_run, which orders memory). Text is UTF-8; a code
 * point with no glyph, a control character and each malformed byte draws
 * as a box (.notdef). There is no shaping: kerning between pairs, nothing
 * else.
 *
 * Positions: x, y are a baseline's left end; the pen moves in 1/256
 * pixels and each glyph is drawn at the nearest quarter pixel, so widths
 * are as the font means them, not one rounding per glyph. Each pixel is
 * drawn as px_over(pixel, argb_pm(rgb, coverage)): full coverage gives rgb
 * exactly, none leaves the pixel as it was. */
struct font;

enum font_weight {
    FONT_REGULAR,   /* Inter Regular (400) */
    FONT_MEDIUM,    /* Inter Medium (500): a focused window's title */
};
#define FONT_PX_MIN 6     /* the sizes font_open bakes: pixels to the em */
#define FONT_PX_MAX 128

/* Weight w at px pixels to the em (CSS's font-size: Inter's capitals are
 * 0.73 of it), baked now. Errors: ERR_OUT_OF_RANGE (px or w),
 * ERR_NO_MEMORY. It takes some milliseconds and a few hundred KiB at
 * title sizes (ARCHITECTURE.md "Smooth text" has the numbers): open
 * the sizes a program needs once, at its start. */
status_t font_open(enum font_weight w, int px, struct font **out);
/* Give a font back (NULL: nothing). No thread may still be drawing with it. */
void     font_close(struct font *f);

struct font_metrics {
    int px;        /* pixels to the em: the size it was opened at */
    int ascent;    /* the face's height above the baseline, pixels (rounded up) */
    int descent;   /* its depth below the baseline (positive, rounded up) */
    int line_h;    /* baseline to baseline for lines of text */
    int cap_h;     /* a capital's height (rounded): what font_draw_in centres */
};
const struct font_metrics *font_metrics(const struct font *f);

/* How wide str is drawn: the pen's advance with kerning, rounded to
 * whole pixels (ink may reach a pixel or so past either end). */
int    font_width(const struct font *f, const char *str);
/* str in colour rgb (0xRRGGBB) with its baseline's left end at x, y of s,
 * only inside clip (NULL: all of s) and s. Returns the x after it (x plus
 * font_width). */
int    font_draw(const struct surf *s, const struct rect *clip, const struct font *f, int x, int y,
                 uint32_t rgb, const char *str);

enum font_align {
    FONT_LEFT,     /* from r's left edge */
    FONT_CENTRE,   /* centred across r */
};
/* str on one line in r: cut short with an ellipsis ("…") if it is wider
 * than r->w (as font_ellipsize cuts), placed by align across r, its
 * capitals centred down r (the baseline at r->y + (r->h + cap_h) / 2),
 * drawn only inside r and s. r may reach outside s (a tile of a bigger
 * picture: every tile computes the same layout and draws its part).
 * Returns the width drawn. */
int    font_draw_in(const struct surf *s, const struct rect *r, const struct font *f, uint32_t rgb,
                    enum font_align align, const char *str);
/* str as it fits in max_w pixels, into buf (n bytes, always terminated
 * when n > 0): str itself if it is no wider and fits in buf; else the
 * longest start of it (whole code points, not ending in a space) that fits
 * with "…" after it, and the "…"; "" if not even "…" fits. Returns the
 * length written. */
size_t font_ellipsize(const struct font *f, const char *str, int max_w, char *buf, size_t n);

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
    KEY_QUIT,   /* Esc or Ctrl+C, or the window's close box */
    KEY_MOUSE,  /* not a key: the mouse did something (gfx_mouse says what) */
    KEY_BACKSPACE, KEY_TAB, KEY_END, KEY_DELETE,
    KEY_RESIZE, /* not a key: the window's size changed (only after gfx_resizable) */
};
/* The next key press (DOWN or REPEAT) before deadline: a KEY_* code or a
 * character; KEY_NONE on timeout (deadline 0: don't wait). KEY_QUIT too if
 * the key channel broke (a window: the compositor is gone for good) and
 * when the window's close box is clicked. In a window it is also where
 * the compositor's news is read (KEY_RESIZE, frames, the buffers it gives
 * back): an app that never calls it still works, as gfx_present reads
 * them too. After gfx_mouse_open it is also how the mouse is
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
 * a count whatever the speed, which the tests need). In a window the
 * pointer is the compositor's (it moves it, accelerates it and draws the
 * arrow): x and y are where it is over the window, kept as they were
 * last while it is elsewhere, and accel is not looked at. */
status_t gfx_mouse_open(bool accel);
/* The mouse now, and what it did since the last call. */
void     gfx_mouse(struct mouse *out);
/* Hide or show the arrow (shown by default once the mouse has moved). */
void     gfx_pointer_show(bool on);
/* Only the arrow moved: present just its rows (much cheaper than
 * gfx_present when the frame is unchanged). Nothing in a window. */
void     gfx_present_pointer(void);

/* The arrow (mouse.c), at scale 1: '#' its outline (black), 'o' its fill
 * (white), ' ' nothing; the hot spot is the top-left corner. The
 * compositor draws the same one. */
#define POINTER_ARROW_W 12
#define POINTER_ARROW_H 18
extern const char pointer_arrow[POINTER_ARROW_H][POINTER_ARROW_W + 1];

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
/* Give back a block of big_alloc's (p NULL: nothing), bytes as asked. */
void     big_free(void *p, uint64_t bytes);
/* Make a block of big_alloc's read-only (bytes as asked): a write to it
 * faults from then on. Errors: vmar_protect's. */
status_t big_seal(void *p, uint64_t bytes);
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
