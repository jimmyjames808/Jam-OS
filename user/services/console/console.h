/* console: what its parts share (main.c says what the console does).
 *
 *   text.c     the text model (scrollback, the current line) and the
 *              alternate screen's grid
 *   term.c     program output as a small terminal: escapes, UTF-8
 *   screen.c   drawing the cells on the framebuffer, and lending it out
 *   window.c   the window mode: the terminal as a Wayland client
 *   winpaint.c ... drawing the cells into its window's buffers
 *   wlinput.c  ... its size, and Wayland's keys and pointer as input events
 *   cellpaint.c a cell drawn in the 8x16 bitmap or the smooth font (cells.h)
 *   view.c     which lines of the text the screen shows
 *   select.c   the mouse's selection: its geometry and its text
 *   clip.c     ... in the window: the mouse and the keys that copy and paste
 *   keys.c     the focus stack of key channels, and the input sources
 *   paste.c    pasted text typed into the focus
 *   clients.c  the console protocol's clients and their levels
 *   notices.c  the few things worth a line while the kernel log is off
 *              the screen
 *   main.c     the kernel log, startup and the event loop */
#pragma once

#include <stdarg.h>
#include <idl/console.h>
#include <idl/input.h>
#include <jwl_client.h>
#include <os.h>
#include <utf8.h>
#include "cells.h"

#define MAX_COLS   480
#define MAX_ROWS   180
#define SCROLLBACK 4000         /* committed lines kept */
#define MAX_CLIENTS 32          /* SR_USER + 0..7 at start (ADMIN), then new_client's */
#define START_CLIENTS 8
#define MAX_SOURCES 16
#define MAX_FOCUS   8
#define PENDING_KEYS 256
#define RENDER_NS  16000000ull
/* The window mode's terminal (wlinput.c, term_grid): at most this many
 * cells, at least the classic 80x24 (unless the output is smaller). */
#define WIN_COLS     160
#define WIN_ROWS     50
#define WIN_MIN_COLS 80
#define WIN_MIN_ROWS 24
/* SR_USER + this: /svc/wayland's shared channel (window mode). */
#define WAYLAND_ROLE 10
/* One round of one client's requests: a count and a time, so a client
 * writing flat out can't hold up the keys (Ctrl+C), the other clients or
 * the render. */
#define CLIENT_BUDGET    64
#define CLIENT_BUDGET_NS (20 * NS_PER_MS)
#define KLOG_BUF   16384

/* The cells' colours (cells.h has the palette). */
#define A_KERNEL  ATTR(C_GREY, C_BLACK)
#define A_STAMP   ATTR(C_DARK, C_BLACK)
#define A_PROC    ATTR(C_BGREEN, C_BLACK)
#define A_OUT     ATTR(C_WHITE, C_BLACK)
#define A_NOTICE  ATTR(C_BYELLOW, C_BLACK)

/* Client levels (console.idl new_client): see main.c. */
enum { L_ADMIN, L_SHELL, L_PROGRAM };
#define LOG_ONLY_MAX 32
struct client {
    uint8_t level;                   /* L_* */
    bool    show_log;                /* console.show_log: it asks for the log on the screen */
    bool    bracketed;               /* it wrote ESC [ ? 2004 h: its pastes are bracketed */
    char    log_only[LOG_ONLY_MAX];  /* ... only this process's lines ("": all of them) */
};

/* Port keys: the kind in the high half, an index in the low. */
enum { K_KLOG = 1, K_CLIENT, K_SOURCE, K_ALT, K_LEASE, K_INIT, K_WL };
#define KEY(kind, i) ((uint64_t)(kind) << 32 | (i))

/* main.c */
extern handle_t root;              /* SR_RESOURCE */
extern handle_t port;              /* everything we wait for */
/* Kernel log lines logged so far go into the scrollback (or, while the
 * log is off the screen, to notices.c). */
void klog_event(void);

/* ---- view.c: which lines the screen shows ------------------------------------
 *
 * Lines are numbered as the scrollback numbers them: 0 to committed - 1
 * the committed ones, `committed` the current line (the cursor's). On the
 * full-screen console (nocomp) the current line is always the bottom row,
 * as it ever was. In a window (from_top) the text starts at the top as in
 * any terminal: the screen's first row is line `top` (0 at the start, the
 * current line after a clear) until the current line reaches the bottom
 * row; from then on the screen scrolls, the current line on the bottom
 * row. Either way the current line is on the screen unless scrolled back,
 * whatever the rows (a resize keeps it in view). No state: utest checks
 * them (conwin.c). */
struct view {
    uint64_t committed;   /* lines committed */
    uint64_t top;         /* from_top: the line the screen starts at (<= committed) */
    uint32_t rows;        /* the screen's rows */
    bool     from_top;    /* window mode */
};
/* The line on the screen's first row, not scrolled back (below 0: rows
 * before the first line, blank). */
int64_t  view_base(const struct view *v);
/* How far back the view may go: to the oldest line the scrollback keeps. */
uint32_t view_back_max(const struct view *v);

/* ---- text.c: the text model -------------------------------------------------- */

extern uint32_t cols, rows;
extern uint64_t committed;         /* lines ever committed; line i at i % SCROLLBACK */
extern struct cell cur[MAX_COLS];  /* the current line */
extern uint32_t cur_x;             /* the cursor */
extern uint8_t out_attr;           /* program output colour (ESC [ m) */
extern uint8_t out_style;          /* ... and style (S_BOLD: ESC [ 1 m) */
extern uint32_t view_back;         /* lines scrolled back (0: at the bottom) */
extern uint64_t top_line;          /* window mode: the line the screen starts at (view.c) */
extern bool dirty;                 /* the screen needs a render */

/* The scrollback's line i (committed ones only). */
struct cell *line(uint64_t i);
void blank(struct cell *c, uint32_t n, uint8_t attr);
/* The view as it is now (view.c). */
struct view view_now(void);
/* The scrollback, allocated (and touched) for cols x rows. */
bool text_init(void);
/* A kernel log line (without its newline), wrapped at cols, above the
 * current line. */
void kernel_line(const char *s, size_t n);
/* A notice (notices.c), the same way, in its own colour. */
void notice_out(const char *s, size_t n);
/* The glyph for code point cp (not a control character): ASCII, a Latin
 * letter, the box drawing and block elements as they are, anything else
 * '?'. */
uint16_t cell_glyph(uint32_t cp);
/* Commit the current line and start a new one. */
void new_line(void);
/* ESC [ 2 J: the text so far scrolled off the screen (a screenful of
 * blank lines; in window mode, the screen starts again at the current
 * line, on the top row). */
void clear_screen(void);
/* The grid becomes c x r cells (the window was resized): the scrollback,
 * the current line and the alternate screen keep their cells, cut or
 * padded with blanks at the new width (nothing is wrapped again); the
 * view and the cursors stay inside. false: no memory (or a size out of
 * range), and nothing changed. */
bool text_regrid(uint32_t c, uint32_t r);

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

/* The client whose console.write is being read (clients.c sets it around
 * the bytes; NULL otherwise): the DEC modes a program asks for are its own
 * (ESC [ ? 2004 h, bracketed paste). */
extern struct client *out_writer;

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
/* What the screen shows now, cell by cell: show(x, y, cell, marks) for
 * each of the rows x cols cells (the scrollback's view and the current
 * line, or the alternate screen); marks: CELL_CURSOR, CELL_SELECTED. */
void grid_walk(void (*show)(uint32_t x, uint32_t y, struct cell c, uint8_t inv));
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
extern struct client *focus_client[MAX_FOCUS];   /* the client that opened each */
extern unsigned nfocus;
/* Drop focus i (its client is gone, or it was pushed out). */
void focus_drop(unsigned i);
status_t op_open_keys(void *ctx, handle_t *out);
status_t op_connect_input(void *ctx, handle_t *out);
/* Input source i is readable (or gone). */
void source_event(unsigned i);
/* A key, from an input source or the window (terminal: from a terminal's
 * text, where PageUp scrolls back without Shift): Ctrl+Alt+Del, the
 * scrollback's keys, or to the focus. */
void key_event(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool terminal);
/* A mouse report, from an input source or the window: to the focus if
 * it asked for the mouse, else the wheel scrolls back. */
void mouse_event(const struct input_mouse_event *ev);
/* Does the focus (the newest key channel) want the mouse? Then the
 * window's pointer is its, not the selection's. */
bool focus_wants_mouse(void);
/* Super+Enter: ask init for another terminal (initctl.terminal), without
 * waiting; a refusal is said on this terminal. */
void terminal_ask(void);
/* Something came on init's control channel: the answer to a request of
 * ours (a failed reboot, a terminal or not), or init's end closed while
 * a reboot is asked for (reset the machine). */
void init_event(void);
/* Watch init's control channel for answers (at start). */
void init_watch(void);
/* When the console resets the machine itself if init hasn't
 * (DEADLINE_NEVER: no reboot asked for); reboot_due does it then. */
uint64_t reboot_deadline(void);
void reboot_due(void);

/* ---- paste.c: pasted text typed into the focus ------------------------------------ */

/* Text (UTF-8, copied) typed into the newest focus as key presses with
 * usage 0, as a serial terminal's (paste.c says which characters go and
 * how), between ESC [ 200 ~ and ESC [ 201 ~ if that focus's client asked
 * for bracketed paste. As fast as the focus reads: what doesn't fit its
 * channel goes at paste_deadline (paste_pump). false: a paste is still
 * going (this one is dropped), no focus, or no memory. */
bool paste_start(const char *text, size_t n);
void paste_pump(void);
/* When paste_pump must run (0: now; DEADLINE_NEVER: no paste). */
uint64_t paste_deadline(void);
/* The rest of a paste dropped (a key typed): its bracket still closed. */
void paste_stop(void);

/* ---- window.c, winpaint.c, wlinput.c: the window mode --------------------------- */

extern bool window_mode;           /* a compositor draws the screen: we draw into a window */
extern unsigned term_no;           /* this terminal's number (1: the first, the system's) */
extern bool closing;               /* our window was closed: main ends with 0 */
extern bool font_bitmap;           /* terminal.font = bitmap (argument, set_font) */
/* Window mode on /svc/wayland's shared channel svc, as terminal `term`:
 * connect (without waiting: the window opens once the compositor
 * answers). false: it can't (said); the text then goes to COM1 only. */
bool window_init(handle_t svc, unsigned term);
/* The client's packet (K_WL): its messages, the window, the events. */
void window_event(void);
/* When the client has work of its own (key repeat, a reconnect's try). */
uint64_t window_deadline(void);
/* The window, if it is up and configured now (NULL: nothing to draw on). */
struct jwl_window *window_now(void);
/* Draw what changed into the window's free buffer and present it. */
void window_render(void);
/* The window's shadow grids for cols x rows (at start and after a
 * regrid); false: no memory. */
bool paint_regrid(void);
/* Every cell of both buffers drawn again at the next render. */
void paint_forget(void);
/* The window's cells: their size and font (window.c sets it: the smooth
 * font unless the argument or console.set_font says bitmap). */
extern struct cell_look look;
/* The smooth font (bitmap false) or the 8x16 bitmap for the window: the
 * grid again for the window's size, everything drawn again. */
void window_set_font(bool bitmap);

/* The terminal's grid in look l on an output of ow x oh pixels (0: not
 * known yet): a window of at most three quarters of it each way (its
 * WIN_PAD padding included), at most WIN_COLS x WIN_ROWS cells, at least
 * WIN_MIN_COLS x WIN_MIN_ROWS if the output has room. */
void term_grid(const struct cell_look *l, int32_t ow, int32_t oh, uint32_t *out_cols,
               uint32_t *out_rows);
/* The size in pixels of a window holding c x r cells in look l. */
void window_size(const struct cell_look *l, uint32_t c, uint32_t r, int32_t *w, int32_t *h);
/* The grid that fits a window of w x h pixels inside its padding (1 to
 * MAX_COLS x MAX_ROWS). */
void grid_of_size(const struct cell_look *l, int32_t w, int32_t h, uint32_t *out_cols,
                  uint32_t *out_rows);
/* terminal.font's value (init's argument "font=<value>", console.set_font):
 * 0 smooth, 1 bitmap, -1 neither. */
int  term_font_parse(const char *value);
/* A wl_keyboard key (evdev code, JWL_KEY_* state, KEYMAP_MOD_* modifiers,
 * the character it types) as the console's key event: the HID usage, the
 * left modifier keys. false: a key with no HID usage (dropped). */
bool key_of_wayland(uint32_t code, uint32_t state, uint32_t mods, uint32_t cp,
                    struct input_key_event *out);
/* The key asks for another terminal: Super+Enter pressed. */
bool asks_terminal(const struct input_key_event *ev);
/* What mouse_of_wayland keeps between pointer events. */
struct pointer_track {
    int32_t x, y;        /* the last position, surface coordinates, fixed 24.8 */
    uint8_t buttons;     /* INPUT_BTN_* held */
};
/* A wl_pointer event as a mouse report (relative pixels, the buttons
 * held, the wheel in notches, + away from the user); false: it makes
 * none (enter, leave, no movement, another axis or button). */
bool mouse_of_wayland(struct pointer_track *p, const struct jwl_event *ev,
                      struct input_mouse_event *out);
/* The pointer's shape at surface pixel (x, y) of a window with a cols x
 * rows grid of l's cells inside its padding (wp-cursor-shape-v1's): the
 * text bar over the text, the arrow over the padding. */
uint32_t pointer_shape_at(const struct cell_look *l, uint32_t cols, uint32_t rows, int32_t x,
                          int32_t y);
/* The clipboard's keys: Super+C or Ctrl+Shift+C copies, Super+V or
 * Ctrl+Shift+V pastes (Ctrl+C alone stays the interrupt). */
enum clip_key { CLIP_NONE, CLIP_COPY, CLIP_PASTE };
enum clip_key clip_key_of(const struct input_key_event *ev);
/* Does a key typed clear the selection: a press that isn't a modifier
 * alone or the scrollback's page keys. */
bool key_clears_selection(const struct input_key_event *ev);
/* The next key pasted text types, from text[*at .. n): true with *out (a
 * press, usage 0 and the character) and *at moved past what it took; false
 * at the end. Printable characters as themselves; '\n', '\r' and "\r\n" a
 * newline '\n'; '\t' a tab; every other control character (ESC among
 * them, so pasted text can't end a bracketed paste early), DEL, the C1
 * controls and bad UTF-8 are dropped. */
bool paste_key(const char *text, size_t n, size_t *at, struct input_key_event *out);

/* ---- select.c, clip.c: the mouse's selection (window mode) ---------------------- */

/* A point of the text: a line, as the scrollback numbers them (view.c),
 * and a column. */
struct sel_pt {
    int64_t  line;
    uint32_t col;
};
enum sel_unit { SEL_CHAR, SEL_WORD, SEL_LINE };
struct selection {
    bool on;                       /* something is selected: drawn, and copied by Super+C */
    enum sel_unit unit;            /* a drag's, a double click's, a triple click's */
    struct sel_pt anchor, head;    /* where the press was, where the pointer is */
};
/* The text a selection reads: line i's cols cells (NULL: not kept). */
struct sel_text {
    const struct cell *(*line)(void *ctx, int64_t i);
    void *ctx;
    uint32_t cols;
};
/* The cell under surface pixel (x, y) of a window with a cols x rows grid
 * of l's cells inside its padding, clamped to the grid. */
void   sel_cell_at(const struct cell_look *l, uint32_t cols, uint32_t rows, int32_t x, int32_t y,
                   uint32_t *col, uint32_t *row);
/* The selection's first and last cell, its ends grown to their word or
 * line: false if nothing is selected. */
bool   sel_range(const struct selection *s, const struct sel_text *t, struct sel_pt *from,
                 struct sel_pt *to);
/* Is (line, col) in [from, to]? */
bool   sel_has(const struct sel_pt *from, const struct sel_pt *to, int64_t line, uint32_t col);
/* The selected text as UTF-8 into out (cap bytes, cut there; no NUL):
 * its length. */
size_t sel_copy(const struct selection *s, const struct sel_text *t, char *out, size_t cap);
/* Counting clicks: a press within 500 ms of the last on the same cell is
 * the next of a double or triple click (and a fourth starts again). */
struct click_track {
    unsigned count;                /* the last press's: 1, 2 or 3 (0: none yet) */
    uint32_t time_ms;              /* its time (wl_pointer's) */
    struct sel_pt at;              /* its cell */
};
unsigned sel_click(struct click_track *k, uint32_t time_ms, struct sel_pt at);
/* A press at `at`, the clicks'th: one starts a drag (nothing selected
 * until it moves to another cell), two select a word, three a line. */
void   sel_press(struct selection *s, struct sel_pt at, unsigned clicks);
/* The drag reached `at`: true if the selection changed. */
bool   sel_drag(struct selection *s, struct sel_pt at);

/* clip.c: the selection in this terminal. */
extern struct selection sel;
/* A pointer event of the window's: true if the selection took it (the
 * left button and its drags, while the focus doesn't want the mouse and
 * the text screen is up); the wheel is never taken. */
bool   clip_pointer(const struct jwl_event *ev);
/* A key from the window: true if it was a copy or paste key (or the
 * release of one), which goes no further. Any other key typed clears the
 * selection. */
bool   clip_key(const struct input_key_event *ev);
/* Is (line, col) of the text screen selected now? clip_begin computes the
 * range once for a screen's walk (grid_walk). */
void   clip_begin(void);
bool   clip_marked(int64_t line, uint32_t col);
/* Nothing selected (a full-screen program took the screen). */
void   clip_clear(void);

/* window.c: the clipboard through the window's client. */
/* Offer text as the selection; false if it can't (no clipboard). */
bool   window_copy(const char *text, size_t n);
/* Ask for the selection's text: it comes to paste_start (JWL_EV_PASTE). */
void   window_paste(void);

/* ---- notices.c: while the kernel log is off the screen ---------------------------- */

/* Who wrote a log line, from the kernel's mark on it (klog_lines) and
 * init's table of the system's writers (<logwriters.h>): only the kernel's
 * own lines and those of init's, devmgr's and logd's real processes can
 * make notices. W_OTHER: anyone else, or not known (no mark, no table). */
enum log_writer { W_KERNEL, W_INIT, W_DEVMGR, W_LOGD, W_OTHER };

/* A kernel log line (without its newline) that `w` wrote: follow what it
 * says, and turn it into a notice if announce (the log is not on the
 * screen) and it is one. */
void     notice_take(const char *s, size_t n, bool announce, enum log_writer w);
/* When notice_tick has something to do next (uptime ns), or DEADLINE_NEVER. */
uint64_t notice_deadline(void);
/* Announce what has settled; nothing while the log is shown. */
void     notice_tick(bool shown);
/* What was read so far is known (the log before the console started):
 * only what changes from now on is news. whole_log: that log began at the
 * boot's first line (it hasn't wrapped). */
void     notice_settle(bool whole_log);
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
