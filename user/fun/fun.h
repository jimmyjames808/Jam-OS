/* The fun apps' shared code (bin/life, bin/tetris, bin/fractal).
 *
 * Each app compiles this in through a one-line user/<app>/fun.c
 * (`#include "../fun/fun.c"`), so there is no extra library to build.
 *
 * The screen: TEXT MODE inside the console. term_open opens a key channel
 * (console.open_keys: this program gets the keyboard focus) and switches
 * the console to its alternate screen (ESC [ ? 1049 h; user/console/main.c
 * "the alternate screen"), whose cells the app fills in a RAM grid;
 * term_flush sends only the cells that changed since the last flush, as
 * cursor moves + colours + characters, bracketed as one frame (ESC [ ?
 * 2026 h/l). Cells can hold the block elements (upper/lower half, full
 * block, 25/50/75% shade): with 16 colours, the half blocks give two
 * square "pixels" per 8x16 cell (320 x 180 on the PC's 2560 x 1440) and
 * the shades mix colours. term_close leaves the alternate screen (the
 * console also leaves it by itself if the app dies: its key channel
 * closes). All of the screen hand-off is in term_open / term_close.
 *
 * Also here: the CPU count (CPUID; there is no system call for it), a
 * thread pool that runs `items` of work on every CPU (workers spin briefly
 * between jobs, then sleep on an event), and key decoding for both key
 * sources (a USB keyboard: HID usages; a serial terminal: codepoints). */
#pragma once

#include <os.h>

/* ---- colours: the console's 16-colour palette ---------------------------------- */

enum {
    C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_GREY,
    C_DARK, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_WHITE,
};
/* The RGB the console draws each palette index with (user/console/main.c). */
extern const uint32_t fun_palette[16];

/* Glyphs beyond ASCII (sent as UTF-8 block elements). */
enum { G_UPPER = 1, G_LOWER, G_FULL, G_LIGHT, G_MEDIUM, G_DARK };

/* ---- the terminal ------------------------------------------------------------------ */

struct tcell {
    uint8_t ch, fg, bg;
};

struct term {
    uint32_t     cols, rows;
    struct tcell *cell;        /* rows * cols: what the app wants on screen */
    struct tcell *shown;       /* what the console has */
    handle_t     con, keys;
    bool         open;
    char         out[2048];
    uint32_t     nout;
    uint64_t     bytes_sent;   /* stats */
};

/* Open the console (SR_CONSOLE), take the keys, enter the alternate screen.
 * Needs a console; fails with its status otherwise. */
status_t term_open(struct term *t);
/* Leave the alternate screen (the text screen comes back as it was) and
 * give the keys back. */
void     term_close(struct term *t);
/* Send the changed cells as one frame. */
void     term_flush(struct term *t);
/* Drawing into t->cell (clipped). */
void     term_put(struct term *t, int x, int y, uint8_t ch, uint8_t fg, uint8_t bg);
void     term_fill(struct term *t, int x, int y, int w, int h, uint8_t ch, uint8_t fg, uint8_t bg);
/* Returns the x after the text. */
int      term_text(struct term *t, int x, int y, const char *s, uint8_t fg, uint8_t bg);
int      term_textf(struct term *t, int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...)
             __attribute__((format(printf, 6, 7)));

/* ---- keys ---------------------------------------------------------------------------- */

enum {
    KEY_NONE = 0,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_PGUP, KEY_PGDN, KEY_HOME,
    KEY_QUIT,   /* Esc or Ctrl+C */
};
/* The next key press (DOWN or REPEAT) before deadline: a KEY_* code or a
 * character; KEY_NONE on timeout. KEY_QUIT too if the key channel broke. */
int      term_key(struct term *t, uint64_t deadline);
/* One key event decoded (KEY_NONE for a release or a bare modifier). */
int      key_decode(const struct input_key_event *ev);

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

#define FUN_MAX_THREADS 64

/* Logical CPUs in the package, from CPUID leaf 0x1F/0xB (1..64). */
uint32_t fun_cpu_count(void);
/* Start n - 1 workers (n = 0: one per CPU); the calling thread is worker
 * 0. Returns how many threads the pool has. */
uint32_t pool_start(uint32_t n);
uint32_t pool_threads(void);
/* Run fn(item, worker, arg) for item = 0 .. items - 1 on every pool
 * thread (items handed out one at a time), and wait for all of them. */
void     pool_run(void (*fn)(uint32_t item, uint32_t worker, void *arg), void *arg, uint32_t items);
/* Work items each thread did in the last pool_run. */
extern uint32_t pool_items_by[FUN_MAX_THREADS];

/* ---- odds and ends ---------------------------------------------------------------------- */

static inline uint64_t now_ns(void) { return (uint64_t)jam_clock_get(); }

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
 * and to the kernel log. */
void     say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "1,234,567" */
char    *commas(char *buf, size_t n, uint64_t v);
bool     has_arg(int argc, char **argv, const char *name);
/* The number after "name=" in argv, or def. */
uint64_t arg_num(int argc, char **argv, const char *name, uint64_t def);
