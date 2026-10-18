/* console: what its parts share (main.c says what the console does).
 *
 * The console is a front end and a back end. The front end is the
 * `console` protocol's side: the clients and their levels, the focus of
 * key channels, the kernel log's lines, the event loop. The back end is
 * where the terminal is: it takes the program output and the log lines,
 * and it is where the keys come from. There are two (struct backend):
 * the window (a text model drawn into a Wayland window, and COM1) and
 * the connection (bytes on a channel to a remote terminal).
 *
 * Front end:
 *   keys.c     the focus stack of key channels and tty clients, and the keys sent to it
 *   tty.c      POSIX clients: console.open_tty, the `tty` protocol, the line discipline
 *   paste.c    pasted text typed into the focus
 *   clients.c  the console protocol's clients and their levels
 *   main.c     the kernel log, startup and the event loop
 *
 * Window back end (winback.c is its struct backend):
 *   winback.c  the back end's calls, and the window's scrollback keys
 *   text.c     the text model (scrollback, the current line) and the
 *              alternate screen's grid
 *   term.c     program output as a small terminal: escapes, UTF-8
 *   screen.c   what the screen shows: the cells walked for winpaint.c
 *   window.c   the window: the terminal as a Wayland client
 *   winpaint.c ... drawing the cells into its window's buffers
 *   wlinput.c  ... its size, and Wayland's keys and pointer as input events
 *   cellpaint.c a cell drawn in the 8x16 bitmap or the smooth font (cells.h)
 *   view.c     which lines of the text the screen shows
 *   select.c   the mouse's selection: its geometry and its text
 *   clip.c     ... in the window: the mouse and the keys that copy and paste */
#pragma once

#include <stdarg.h>
#include <idl/console.h>
#include <idl/input.h>
#include <jwl_client.h>
#include <openkeys.h>
#include <os.h>
#include <utf8.h>
#include "cells.h"

#define MAX_COLS   480
#define MAX_ROWS   180
#define SCROLLBACK 4000         /* committed lines kept */
#define MAX_CLIENTS 32          /* SR_USER + 0..7 at start (ADMIN), then new_client's */
#define START_CLIENTS 8
#define MAX_FOCUS   8
#define MAX_TTYS    8           /* console.open_tty's clients at once */
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

/* Client levels (console.idl new_client): see main.c. */
enum { L_ADMIN, L_SHELL, L_PROGRAM };
#define LOG_ONLY_MAX 32
struct client {
    uint8_t level;                   /* L_* */
    bool    show_log;                /* console.show_log: it asks for the log on the screen */
    bool    bracketed;               /* it wrote ESC [ ? 2004 h: its pastes are bracketed */
    char    log_only[LOG_ONLY_MAX];  /* ... only this process's lines ("": all of them) */
};

/* Port keys: the kind in the high half, an index in the low. K_KLOG and
 * K_CLIENT are the front end's; the others go to the back end's event. */
enum { K_KLOG = 1, K_CLIENT, K_ALT, K_WL, K_MASTER, K_CTL, K_TTY, K_TTY_OUT };
#define KEY(kind, i) ((uint64_t)(kind) << 32 | (i))

/* ---- the back end ---------------------------------------------------------------- */

/* Where the terminal is. The front end calls these, all on the console's
 * one thread, from the event loop; a hook may be NULL where it says so. */
struct backend {
    /* Draw the kernel log written before the console started (its last
     * part): a terminal with a screen shows it; a connection's is new. */
    bool replay_log;
    /* Set up (the console's port exists, the clients are bound): false
     * after saying why, and the console ends. */
    bool (*init)(void);
    /* The startup line (the clients bound, whether the kernel log is off
     * the screen). */
    void (*banner)(unsigned clients, bool log_off);
    /* The first frame, once the boot log is in (NULL: none). */
    void (*ready)(void);
    /* Program output (console.write) of client c, as it was written. */
    void (*write)(struct client *c, const uint8_t *text, uint32_t length);
    /* A tty client's output or echo, after the line discipline's output
     * processing: sent as it is (no CR added before an LF). */
    void (*write_raw)(struct client *c, const uint8_t *text, uint32_t length);
    /* A kernel log line some client asked for (without its newline). */
    void (*log_line)(const char *s, size_t n);
    /* The terminal's size in cells. */
    void (*size)(uint16_t *c, uint16_t *r);
    /* console.clear. */
    void (*clear)(void);
    /* terminal.font for the window (true: the bitmap; NULL: nothing to change). */
    void (*set_font)(bool bitmap);
    /* Focus channel h was dropped from the focus stack (NULL: nothing to do). */
    void (*focus_dropped)(handle_t h);
    /* A port packet with one of the back end's keys (K_ALT, K_WL, ...). */
    void (*event)(uint32_t kind, uint32_t i);
    /* When the back end next needs the loop (DEADLINE_NEVER: it waits for
     * a port packet). */
    uint64_t (*deadline)(void);
    /* The loop's wait timed out: the back end's own work (a key repeat, a
     * reconnect's try), before the clients are served (NULL: none). */
    void (*tick)(void);
    /* After the clients: draw what changed, send what is queued. */
    void (*flush)(void);
    /* The back end can take no more output now (a full queue): the front
     * end serves no client's request until it can (NULL: never). */
    bool (*congested)(void);
};
extern const struct backend win_backend, conn_backend;
extern const struct backend *back;   /* the one this console runs */
/* conn.c: a "cols=<n>", "rows=<n>" or "endidle" argument (anything else is
 * ignored). */
void conn_arg(const char *arg);

/* main.c */
extern handle_t root;              /* SR_RESOURCE */
extern handle_t port;              /* everything we wait for */
extern bool closing;               /* the terminal is gone (window closed, hung up): main ends with 0 */
/* Kernel log lines logged so far go into the scrollback (those that are
 * shown: console.show_log). */
void klog_event(void);

/* ---- view.c: which lines the screen shows ------------------------------------
 *
 * Lines are numbered as the scrollback numbers them: 0 to committed - 1
 * the committed ones, `committed` the current line (the cursor's). Without
 * a window (no compositor: nothing shows it) the current line is the
 * bottom row. In a window (from_top) the text starts at the top as in
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

/* ---- screen.c: what the screen shows ----------------------------------------- */

/* Draw the cells that changed since the last render (into the window). */
void render(void);
/* What the screen shows now, cell by cell: show(x, y, cell, marks) for
 * each of the rows x cols cells (the scrollback's view and the current
 * line, or the alternate screen); marks: CELL_CURSOR, CELL_SELECTED. */
void grid_walk(void (*show)(uint32_t x, uint32_t y, struct cell c, uint8_t inv));

/* ---- keys.c: keys and the focus stack ----------------------------------------- */

/* The focus stack: open_keys channels and tty clients, the newest last. */
extern handle_t focus[MAX_FOCUS];                /* a key channel, or a tty's channel (our end) */
extern struct client *focus_client[MAX_FOCUS];   /* the client that opened each (a tty's own) */
extern int8_t focus_tty[MAX_FOCUS];              /* a tty's slot (tty.c), or -1: a key channel */
extern unsigned nfocus;
/* Drop focus i (its client is gone, or it was pushed out); a tty's goes
 * with it (ttys_release). */
void focus_drop(unsigned i);
status_t op_open_keys(void *ctx, handle_t *out);
/* Push h, a tty client's channel (tty: its slot), as the newest focus of
 * client c; the keys typed before anyone listened go to it.
 * ERR_NO_RESOURCES: the stack is full and c is a PROGRAM client. */
status_t focus_push(handle_t h, struct client *c, int tty);
/* The newest focus's tty slot, or -1 if it is a key channel (or none). */
int  focus_top_tty(void);
/* A key typed into the terminal, whatever the back end it came from: to
 * the newest focus a PROGRAM or SHELL or ADMIN client opened (and, for
 * Ctrl+C from a program's, the shell below it too); to a tty client as
 * bytes (tty.c). Ctrl+Alt+C (a window's) never reaches a program: it is
 * Ctrl+C to the shell's keys. Typed before anyone listened, it waits for
 * the first. */
void focus_key(const struct input_key_event *ev);
/* focus_key, but false (the key NOT taken, to be tried again) when the
 * focus's key channel is full: the client isn't reading yet. */
bool focus_key_try(const struct input_key_event *ev);
/* ev to the newest key channel below focus `below` that a SHELL or ADMIN
 * client opened: false if there is none (or it can't take it). */
bool focus_to_shell(const struct input_key_event *ev, unsigned below);

/* ---- tty.c: POSIX clients (console.open_tty, the `tty` protocol) ---------------- */

void ttys_init(void);
/* tty clients connected now. */
unsigned ttys_count(void);
status_t op_open_tty(void *ctx, handle_t *out_tty, handle_t *out_data);
/* A tty's packet: K_TTY (its requests, or its channel closed) or
 * K_TTY_OUT (its output). */
void tty_event(uint32_t kind, uint32_t i);
/* Bytes typed while tty i has the focus (a key's, or a connection's as
 * they came), through the line discipline. */
void ttys_type(unsigned i, const uint8_t *p, size_t n);
/* A key typed while tty i has the focus: its bytes (<keybytes.h>). */
void ttys_key(unsigned i, const struct input_key_event *ev);
/* tty i asked for bracketed paste. */
bool ttys_bracketed(unsigned i);
/* Throw away the input not read yet: the line discipline's, and what tty
 * i's data channel holds for it. */
void ttys_flush_in(unsigned i);
/* tty i's focus entry was dropped: what it left unread goes back to the
 * terminal, and its slot is freed. */
void ttys_release(unsigned i);
/* Drop tty i's focus entry (it is gone, or broke the protocol). */
void ttys_hang_up(unsigned i);
/* What the terminal holds, to its reader: the newest tty, or the keys
 * focus when no tty is left. */
void ttys_deliver(void);
/* The focus changed: ttys_deliver at the loop's next turn (not now: the
 * caller may be walking the focus stack). */
void ttys_refocus(void);
/* The loop's share: when it must run, its turn, and the congested back end
 * free again. */
uint64_t ttys_deadline(void);
void ttys_pump(void);
void ttys_resume(void);
/* The tty output queued so far, shown before a client's request is served
 * (the shell's prompt after a program's last lines, not among them): false
 * if the back end is congested first (the request waits, as for
 * back->congested). */
bool ttys_show_queued(void);
/* The terminal's size changed: every tty client is told. */
void ttys_resized(void);

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
extern bool font_bitmap;          /* terminal.font = bitmap (argument, set_font) */
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
 * regrid); false: no memory, and neither changed (window_render tries
 * again, and draws nothing until both are of the grid it walks). */
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
/* A wl_pointer event as a mouse report: whole pixels moved, the wheel in
 * notches (+ away from the user), the buttons held (INPUT_BTN_*). */
struct con_mouse {
    int16_t dx, dy;
    int8_t  wheel;
    uint8_t buttons;
};
/* winback.c: a key from the window: the scrollback's keys (Shift+PageUp
 * and PageDown), or to the focus. */
void win_key(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp);
/* A mouse report from the window: the wheel scrolls back. */
void mouse_event(const struct con_mouse *ev);
/* What mouse_of_wayland keeps between pointer events. */
struct pointer_track {
    int32_t x, y;        /* the last position, surface coordinates, fixed 24.8 */
    uint8_t buttons;     /* INPUT_BTN_* held */
};
/* A wl_pointer event as a mouse report (relative pixels, the buttons
 * held, the wheel in notches, + away from the user); false: it makes
 * none (enter, leave, no movement, another axis or button). */
bool mouse_of_wayland(struct pointer_track *p, const struct jwl_event *ev,
                      struct con_mouse *out);
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

/* ---- conn.c, connin.c: the connection mode ----------------------------------------- */

/* The master channel m, kept for conn_in_take; false: no memory. */
bool conn_in_init(handle_t m);
/* Take the bytes the remote terminal typed off the master (a turn's
 * share: at most a few messages and a few ms) and type them into the
 * focus. OK, or the master's error: ERR_PEER_CLOSED is the owner hanging
 * up. */
status_t conn_in_take(void);
/* When conn_in_idle must run: 0 if input is left, else the time a lone
 * Escape is due (DEADLINE_NEVER: none). */
uint64_t conn_in_deadline(void);
/* Each turn: the rest of an interrupted take, and a lone ESC that has
 * waited long enough typed as the Escape key. */
status_t conn_in_idle(void);

/* ---- clients.c ------------------------------------------------------------------ */

/* Startup's client channels (SR_USER + 0..7): ADMIN, on the port. */
void clients_init(void);
unsigned client_count(void);
/* A client of a lower level than ADMIN is connected (a shell, a program). */
bool clients_users(void);
/* Some client asks for the kernel log on the screen (console.show_log)
 * and a line from process `name` (n bytes; n 0: the kernel's own line) is
 * one it wants. */
bool clients_show_line(const char *name, size_t n);
/* Some client asks for every line of the log. */
bool clients_show_all(void);
/* Client i's channel is readable (or gone): serve one round of its
 * requests, at most CLIENT_BUDGET of them or CLIENT_BUDGET_NS. */
void client_event(unsigned i);
/* When the loop must come back for the clients: 0 when some client still
 * had requests queued when its round ended (clients_serve_pending gives it
 * another), else when the ones held for their unread answers are looked
 * at again (DEADLINE_NEVER: none). */
uint64_t clients_deadline(void);
void clients_serve_pending(void);
/* The back end was congested (back->congested) and no longer is: the
 * clients whose requests waited get a round. */
void clients_resume(void);
