/* console: what its parts share (main.c says what the console does).
 *
 *   text.c     the text model (scrollback, the current line) and the
 *              alternate screen's grid
 *   term.c     program output as a small terminal: escapes, UTF-8
 *   screen.c   drawing the cells on the framebuffer, and lending it out
 *   keys.c     the focus stack of key channels, and the input sources
 *   clients.c  the console protocol's clients and their levels
 *   main.c     the kernel log, startup and the event loop */
#pragma once

#include <font.h>
#include <idl/console.h>
#include <idl/input.h>
#include <os.h>

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
#define KLOG_BUF   16384

/* Palette indices (screen.c has the colours). */
enum { C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_GREY,
       C_DARK, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_WHITE };
#define ATTR(fg, bg) ((uint8_t)((fg) | (bg) << 4))
#define A_KERNEL  ATTR(C_GREY, C_BLACK)
#define A_STAMP   ATTR(C_DARK, C_BLACK)
#define A_PROC    ATTR(C_BGREEN, C_BLACK)
#define A_OUT     ATTR(C_WHITE, C_BLACK)

struct cell {
    uint8_t ch, attr;   /* the character (or a G_* block); ATTR(fg, bg) */
};
/* The block elements full-screen programs draw with, as cell characters. */
enum { G_UPPER = 1, G_LOWER, G_FULL, G_LIGHT, G_MEDIUM, G_DARK };

/* Client levels (console.idl new_client): see main.c. */
enum { L_ADMIN, L_SHELL, L_PROGRAM };
struct client {
    uint8_t level;      /* L_* */
};

/* Port keys: the kind in the high half, an index in the low. */
enum { K_KLOG = 1, K_CLIENT, K_SOURCE, K_ALT, K_LEASE };
#define KEY(kind, i) ((uint64_t)(kind) << 32 | (i))

/* main.c */
extern handle_t root;              /* SR_RESOURCE */
extern handle_t port;              /* everything we wait for */
/* Kernel log lines logged so far go into the scrollback. */
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
status_t op_lend_screen(void *ctx, uint32_t *w, uint32_t *h, uint32_t *pitch, uint8_t *rs,
                        uint8_t *gs, uint8_t *bs, uint64_t *size, handle_t *screen,
                        handle_t *out_lease);
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

/* ---- clients.c ------------------------------------------------------------------ */

/* Startup's client channels (SR_USER + 0..7): ADMIN, on the port. */
void clients_init(void);
unsigned client_count(void);
/* Client i's channel is readable (or gone). */
void client_event(unsigned i);
