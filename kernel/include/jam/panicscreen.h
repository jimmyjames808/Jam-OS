/* The calm panic screen (the owner's design "B1", docs/G1-PLAN.md's
 * polish paragraph): the kernel draws it alone after a panic, with the
 * other CPUs halted and interrupts off. A dark background, the busy ring
 * of docs/design/cursors.svg turning in the middle once a second, one
 * line of Inter under it and a short code under that:
 *   - the stored kernel will start (kexec): "Jam OS hit a problem and is
 *     restarting" for PANIC_HOLD_MS, then the jump as before;
 *   - it can't recover (a crash loop, no stored kernel or a refused one,
 *     a jump that failed): "Jam OS hit a problem it can't recover from" /
 *     "Restarting the PC in 15 s", the details panel by itself after
 *     PANIC_DETAILS_S, the firmware reset at PANIC_RESET_S; if that comes
 *     back, the ring stops and the screen says to hold the power button.
 * No keys: the drivers are user space, gone after a panic. The details
 * go to the log and the serial port as they always did.
 *
 * Three files:
 *   paniccode.c    the code, JAM-<kind>-<4 hex>, from the panic's reason
 *   panicdraw.c    drawing on a framebuffer or a test buffer: the ring
 *                  (worked out per pixel with integers: the kernel has no
 *                  floating point), the baked Inter glyphs
 *                  (<jam/panictext.h>), the 8x16 bitmap font for the
 *                  details panel, and the layout
 *   panicscreen.c  the two cases above, timed by the TSC
 * Nothing here takes a lock or allocates: everything is static, and only
 * the framebuffer is written (text is blended over what the screen has,
 * so it reads it back too; the ring and the panel only write). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/panictext.h>

#define PANIC_HOLD_MS      1500u    /* case 1's screen before the jump */
#define PANIC_HOLD_MAX_MS  5000u    /* panichold= at most: the BSP waits 10 s for a jump */
#define PANIC_DETAILS_S    5u       /* case 2: the details panel appears */
#define PANIC_RESET_S      15u      /* case 2: the firmware reset */

/* ---- the code (paniccode.c) ---------------------------------------------------------
 *
 * JAM-<kind>-<4 hex>: the kind from the reason, the hex digits the low 16
 * bits of an address (upper case):
 *   an exception     its mnemonic (Intel SDM vol. 3, 6.15): DE DB NMI BP
 *                    OF BR UD NM DF TS NP SS GP PF MF AC MC XM VE CP HV VC
 *                    SX, else EX; the hex from RIP, but PF's from the
 *                    faulting address (CR2)
 *   the watchdog     WD (a CPU stopped taking timer interrupts); RIP
 *   a panic(...)     by its message: AS "assertion failed", WD a spinlock
 *                    "stuck for" seconds (a lockup), LK "lockdep:" or a
 *                    "mutex" misuse, OOM "out of memory" anywhere in it,
 *                    else KP (a broken invariant, a test's panic); the hex
 *                    from the address panic() was called from
 * So JAM-PF-0008 is a NULL pointer's field read, JAM-AS-xxxx one assertion
 * of this build. */
#define PANIC_CODE_MAX 16

struct panic_cause {
    char        code[PANIC_CODE_MAX];   /* "JAM-PF-7F3A" */
    const char *kind;                   /* "PF" */
    const char *what;                   /* in words: "page fault" */
    const char *hex_of;                 /* the hex digits' source: "the faulting address" */
    uint64_t    addr;                   /* ... its value */
};

void panic_cause_trap(struct panic_cause *c, uint64_t vector, uint64_t rip, uint64_t cr2);
void panic_cause_watchdog(struct panic_cause *c, uint64_t rip);
void panic_cause_message(struct panic_cause *c, const char *msg, uint64_t caller);

/* What the panic (debug/panic.c) tells the screen for its details panel,
 * written before the screen is drawn. */
#define PANIC_FRAMES  5
#define PANIC_MSG_MAX 160

struct panic_report {
    struct panic_cause cause;
    char        message[PANIC_MSG_MAX];   /* the panic's one line */
    uint64_t    at;                       /* where: RIP, or panic's caller */
    uint64_t    frames[PANIC_FRAMES];     /* the backtrace's first return addresses */
    unsigned    nframes;
    uint32_t    cpu;                      /* the panicking CPU's index */
    const char *note;                     /* panic_note_set's line, "" if none */
};

extern struct panic_report panic_report;

/* ---- drawing (panicdraw.c) ---------------------------------------------------------- */

/* A 32-bit picture: the framebuffer, or a test's buffer. */
struct panic_canvas {
    volatile uint32_t *px;     /* the top-left pixel */
    uint32_t w, h, stride;     /* pixels; stride: pixels from one row to the next */
    uint8_t  rs, gs, bs;       /* each 8-bit channel's shift in a pixel */
};

#define PANIC_RING_PX  40          /* the ring's box: cursors.svg's 24 units at 1x */
#define PANIC_PANEL_W  736         /* the details panel at most, pixels */
#define PANIC_PANEL_LINES 14       /* its text lines at most */

/* Where everything goes on a w x h screen: the ring's centre (between
 * pixels: its box is [rx - 20, rx + 20)), the three lines' baselines and
 * the panel (its lines start at panel_x + 16, panel_y + 12). */
struct panic_layout {
    int ring_x, ring_y;
    int title_y, small_y, code_y;
    int panel_x, panel_y, panel_w, panel_h;
    int panel_cols;            /* 8-pixel characters a panel line holds */
};

void panic_layout(uint32_t w, uint32_t h, struct panic_layout *out);
/* A rectangle of one colour (clipped to the canvas). */
void panic_fill(const struct panic_canvas *c, int x, int y, int w, int h, uint32_t rgb);
/* The busy ring centred at (cx, cy), its arc turned `turn` 65536ths of a
 * turn clockwise from cursors.svg's, over PANIC_BG: every pixel of its box
 * written, nothing read. */
void panic_ring(const struct panic_canvas *c, int cx, int cy, uint32_t turn);
/* A line of baked text: its width in pixels, and drawn with its pen from
 * x on baseline y in rgb, blended over what the canvas has (as libfun's
 * font_draw does). A character the font hasn't baked takes no room. */
int  panic_text_width(const struct panic_font *f, const char *s);
void panic_text(const struct panic_canvas *c, const struct panic_font *f, int x, int y,
                uint32_t rgb, const char *s);
/* At most n characters of s in the 8x16 font, cell by cell: fg on bg. */
void panic_mono(const struct panic_canvas *c, int x, int y, uint32_t fg, uint32_t bg,
                const char *s, int n);
/* The 65536ths-of-a-turn sine, scaled to 1 << 14 (a table, linearly
 * between its points): for the ring, and its test. */
int32_t panic_sin(uint32_t turn);

/* ---- the screen (panicscreen.c) ----------------------------------------------------- */

/* At boot, once bootfs is up: the build's name (bootfs build.txt) and the
 * test word panichold=<ms> (case 1's screen held that long, at most
 * PANIC_HOLD_MAX_MS: for a screenshot). */
void panic_screen_init(void);
/* That build's name ("git 2079f35"), "?" without a build.txt. */
const char *panic_screen_build(void);
/* The panic began: the screen turns PANIC_BG at once (nothing of the
 * kernel's text console is drawn any more). */
void panic_screen_begin(void);
/* Case 1: the screen, held PANIC_HOLD_MS with the ring turning; returns
 * for the jump. */
void panic_screen_restarting(void);
/* Case 2, why: why the stored kernel isn't started. Never returns: the
 * firmware reset at PANIC_RESET_S, or the power button's screen. */
_Noreturn void panic_screen_stuck(const char *why);
