/* console: what its parts share (main.c says what the console does).
 *
 *   text.c     the text model (scrollback, the current line) and the
 *              alternate screen's grid
 *   term.c     program output as a small terminal: escapes, UTF-8
 *   screen.c   drawing the cells on the framebuffer, and lending it out
 *   keys.c     the focus stack of key channels, and the input sources
 *   clients.c  the console protocol's clients and their levels
 *   notices.c  the few things worth a line while the kernel log is off
 *              the screen
 *   main.c     the kernel log, startup and the event loop */
#pragma once

#include <stdarg.h>
#include <font.h>
#include <idl/console.h>
#include <idl/input.h>
#include <os.h>
#include <utf8.h>

#define GW 8
#define GH 16
#define MAX_COLS   480
#define MAX_ROWS   180
#define SCROLLBACK 4000         /* committed lines kept */
#define MAX_CLIENTS 32          /* SR_USER + 0..7 at start (ADMIN), then new_client's */
#define START_CLIENTS 8
#define MAX_SOURCES 16
#define MAX_FOCUS   8
#define PENDING_KEYS 128
#define RENDER_NS  16000000ull
/* One round of one client's requests: a count and a time, so a client
 * writing flat out can't hold up the keys (Ctrl+C), the other clients or
 * the render. */
#define CLIENT_BUDGET    64
#define CLIENT_BUDGET_NS (20 * NS_PER_MS)
#define KLOG_BUF   16384

/* Palette indices (screen.c has the colours). */
enum { C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_GREY,
       C_DARK, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_WHITE };
#define ATTR(fg, bg) ((uint8_t)((fg) | (bg) << 4))
#define A_KERNEL  ATTR(C_GREY, C_BLACK)
#define A_STAMP   ATTR(C_DARK, C_BLACK)
#define A_PROC    ATTR(C_BGREEN, C_BLACK)
#define A_OUT     ATTR(C_WHITE, C_BLACK)
#define A_NOTICE  ATTR(C_BYELLOW, C_BLACK)

struct cell {
    uint16_t ch;    /* the glyph: ASCII, a G_* block, or G_LATIN + n */
    uint8_t  attr;  /* ATTR(fg, bg) */
};
/* The block elements full-screen programs draw with, as cell characters. */
enum { G_UPPER = 1, G_LOWER, G_FULL, G_LIGHT, G_MEDIUM, G_DARK };
/* U+00A0 + n (n < FONT_LATIN_N, <font.h>) is glyph G_LATIN + n. */
#define G_LATIN 128u

/* Client levels (console.idl new_client): see main.c. */
enum { L_ADMIN, L_SHELL, L_PROGRAM };
#define LOG_ONLY_MAX 32
struct client {
    uint8_t level;                   /* L_* */
    bool    show_log;                /* console.show_log: it asks for the log on the screen */
    char    log_only[LOG_ONLY_MAX];  /* ... only this process's lines ("": all of them) */
};

/* Port keys: the kind in the high half, an index in the low. */
enum { K_KLOG = 1, K_CLIENT, K_SOURCE, K_ALT, K_LEASE };
#define KEY(kind, i) ((uint64_t)(kind) << 32 | (i))

/* main.c */
extern handle_t root;              /* SR_RESOURCE */
extern handle_t port;              /* everything we wait for */
/* Kernel log lines logged so far go into the scrollback (or, while the
 * log is off the screen, to notices.c). */
void klog_event(void);

/* ---- text.c: the text model -------------------------------------------------- */

extern uint32_t cols, rows;
extern uint64_t committed;         /* lines ever committed; line i at i % SCROLLBACK */
extern struct cell cur[MAX_COLS];  /* the current line */
extern uint32_t cur_x;             /* the cursor */
extern uint8_t out_attr;           /* program output colour (ESC [ m) */
extern uint32_t view_back;         /* lines scrolled back (0: at the bottom) */
extern bool dirty;                 /* the screen needs a render */

/* The scrollback's line i (committed ones only). */
struct cell *line(uint64_t i);
void blank(struct cell *c, uint32_t n, uint8_t attr);
/* The scrollback, allocated (and touched) for cols x rows. */
bool text_init(void);
/* A kernel log line (without its newline), wrapped at cols, above the
 * current line. */
void kernel_line(const char *s, size_t n);
/* A notice (notices.c), the same way, in its own colour. */
void notice_out(const char *s, size_t n);
/* The glyph for code point cp (not a control character): ASCII, a Latin
 * letter or a block element as they are, anything else '?'. */
uint16_t cell_glyph(uint32_t cp);
/* Commit the current line and start a new one. */
void new_line(void);
/* ESC [ 2 J: a screenful of blank lines. */
void clear_screen(void);

/* The alternate screen (text.c says what it is). */
extern struct cell *alt;           /* rows * cols */
extern bool alt_on, alt_cursor, alt_sync;
extern uint64_t alt_sync_since;
extern uint32_t alt_x, alt_y;
extern handle_t alt_owner;         /* a focus channel, or HANDLE_INVALID */
void alt_newline(void);
void alt_enter(void);
void alt_leave(void);
/* The alternate screen's owner (gen: which time it was entered) closed. */
void alt_owner_event(uint32_t gen);

/* ---- term.c: program output -------------------------------------------------- */

/* One byte of program output. */
void out_char(uint8_t ch);
/* ESC [ p m. */
void sgr(uint32_t p);

/* ---- screen.c: the framebuffer ----------------------------------------------- */

/* Take the framebuffer from the kernel and size cols x rows to it; false:
 * serial only. */
bool screen_init(void);
/* The shadow grid for cols x rows. */
bool screen_alloc(void);
/* Draw the cells that changed since the last render. */
void render(void);
/* Quiet (the boot splash is coming, init's argument "quiet"): draw nothing
 * until a lent screen comes back, or until `until` (uptime ns) if nobody
 * borrows it, so the kernel's dark splash background stays up with no
 * text on it. */
void screen_quiet(uint64_t until);
status_t op_lend_screen(void *ctx, uint32_t *w, uint32_t *h, uint32_t *pitch, uint8_t *rs,
                        uint8_t *gs, uint8_t *bs, uint64_t *size, handle_t *screen,
                        handle_t *out_lease);
/* A program has the screen (lend_screen) and hasn't given it back. */
bool screen_lent(void);
/* console.blank: on, the whole screen the splash background and nothing
 * drawn; off, all of it drawn again. */
void screen_blank(bool on);
/* The lease's other end closed: the screen is ours again. */
void lease_ended(void);

/* ---- keys.c: keys and input sources ----------------------------------------- */

extern handle_t focus[MAX_FOCUS];
extern unsigned nfocus;
/* Drop focus i (its client is gone, or it was pushed out). */
void focus_drop(unsigned i);
status_t op_open_keys(void *ctx, handle_t *out);
status_t op_connect_input(void *ctx, handle_t *out);
/* Input source i is readable (or gone). */
void source_event(unsigned i);

/* ---- notices.c: while the kernel log is off the screen ---------------------------- */

/* A kernel log line (without its newline): follow what it says, and turn
 * it into a notice if announce (the log is not on the screen) and it is
 * one. */
void     notice_take(const char *s, size_t n, bool announce);
/* When notice_tick has something to do next (uptime ns), or DEADLINE_NEVER. */
uint64_t notice_deadline(void);
/* Announce what has settled; nothing while the log is shown. */
void     notice_tick(bool shown);
/* What was read so far is known (the log before the console started):
 * only what changes from now on is news. */
void     notice_settle(void);
/* For the selftest (selftest.c): the last notice put on the screen ("" if
 * none), and everything forgotten (no log lines from then on). */
const char *notice_last(void);
void        notice_reset(void);

/* ---- selftest.c ------------------------------------------------------------------ */

/* `run console selftest`: the notices made of log lines, checked without
 * a screen; the exit code is the number of failures. */
int console_selftest(void);

/* ---- clients.c ------------------------------------------------------------------ */

/* Startup's client channels (SR_USER + 0..7): ADMIN, on the port. */
void clients_init(void);
unsigned client_count(void);
/* Some client asks for the kernel log on the screen (console.show_log)
 * and a line from process `name` (n bytes; n 0: the kernel's own line) is
 * one it wants. */
bool clients_show_line(const char *name, size_t n);
/* Some client asks for every line of the log. */
bool clients_show_all(void);
/* Client i's channel is readable (or gone): serve one round of its
 * requests, at most CLIENT_BUDGET of them or CLIENT_BUDGET_NS. */
void client_event(unsigned i);
/* Some client still had requests queued when its round ended: the main
 * loop must not sleep, and calls clients_serve_pending for another round. */
bool clients_pending(void);
void clients_serve_pending(void);
