/* desk.h: the owner's desktop around the windows (docs/G1-PLAN.md "The
 * look"), what its files share. comp.h has what the rest of the compositor
 * calls; look.h every colour and size.
 *
 *   screens.c   virtual screens: made as needed, each with its own
 *               arrangement; full screen on a screen of its own; minimising
 *               and bringing back; which windows are shown;
 *   anim.c      the animations' clock, places and ends (opening, closing,
 *               minimising, restoring, screens sliding); animdraw.c their
 *               pictures (a snapshot of the window, drawn scaled);
 *   strip.c     the top bar: its three islands' layout and what a click on
 *               them does; stripdraw.c draws it;
 *   menus.c     the search box (the app menu) and Alt+Tab's list;
 *   popover.c   the volume, network and clock popovers; notify.c the
 *               notifications; popdraw.c and menudraw.c draw them;
 *   frost.c     the frosting: the strip's blurred wallpaper (made once) and
 *               the blur behind each card (made while it is open);
 *   ui.c        the drawing they share: rounded boxes, glass cards, letter
 *               tiles, icons, the fonts;
 *   desk.c      the glue: the seat's keys and presses, the loop's clock, the
 *               hooks a later track fills in (launching apps, the mixer's
 *               volume, the network's state, notifications' answers).
 *
 * Threads: the loop's only, but for the drawing files, which run on the
 * painting workers and only read what the loop set before the paint (as
 * the rest of painting does: paint.c).
 *
 * The desktop is on (desk_init) for the real compositor; off it is the
 * window manager alone: no strip, no cards, no animations, the whole
 * output for windows, Alt+Tab focusing the next window at once (the
 * `nodesk` argument, tests that check windows' places, and the test
 * scene until its `desktop` command). Virtual screens and minimising work
 * either way. */
#pragma once

#include <wallclock.h>
#include "paint.h"
#include "wm.h"

/* ---- screens.c --------------------------------------------------------------------------- */

#define DESK_SCREENS_MAX 16u   /* virtual screens at once (Super+Right past the last refused) */

enum screen_kind {
    SCREEN_NORMAL,                 /* windows, the strip, an arrangement of its own */
    SCREEN_FULL,                   /* one full-screen window, nothing else */
};

struct desk_screen {
    bool used;                     /* a slot in use */
    enum screen_kind kind;
    enum comp_layout layout;       /* a normal screen's arrangement */
};

/* One normal screen, current, in layout; no windows. */
void     screens_init(enum comp_layout layout);
/* The arrangement new screens get (and the current one, if normal). */
void     screens_set_default(enum comp_layout layout);
unsigned screens_count(void);
unsigned screens_cur_index(void);
struct desk_screen *screens_cur(void);
struct desk_screen *screens_nth(unsigned i);          /* NULL past the last */
int      screens_index(const struct desk_screen *s);  /* -1: not a screen now */
/* Toplevels on s (minimised ones too). */
unsigned screens_windows(const struct desk_screen *s);
/* Where windows go on s: the output, less the strip and its gap on a
 * normal screen while the desktop is on (comp.h: windows never go under
 * the strip). */
struct comp_box screens_room(const struct desk_screen *s);

/* What the user does. Each slides when the desktop animates. */
void     screens_go(unsigned i);                      /* to screen i (Super+1..9, a dot) */
void     screens_step(int dir);                       /* Super+Left/Right (past the last: new) */
void     screens_add(void);                           /* the strip's "+": a new one at the end */
void     screens_move(struct wm_window *ww, int dir); /* Super+Shift+Left/Right */
void     screens_minimise(struct wm_window *ww);
void     screens_restore(struct wm_window *ww);
/* To ww's screen, restored if minimised, focused (Alt+Tab, a chip). */
void     screens_activate(struct wm_window *ww);
/* The window that would have the focus on s: the one focused last that
 * isn't minimised, or NULL. */
struct wm_window *screens_mru(const struct desk_screen *s);

/* What the window manager tells (wm.c). */
/* ww's first buffer: onto the current screen (or the normal one left of a
 * full-screen screen, which then becomes current). */
void     screens_window_new(struct wm_window *ww);
/* ww's window goes (unmapped or destroyed): its screen may go too. */
void     screens_window_gone(struct wm_window *ww);
/* ww is asked to be full screen now, or no more. */
void     screens_fullscreen(struct wm_window *ww, bool on);
/* Is ww's window to be shown now (its screen current or sliding, not
 * minimised)? And every window mapped or unmapped to match. */
bool     screens_shown(const struct wm_window *ww);
void     screens_sync(void);
/* anim.c: a slide between two screens ended (or there was none). */
void     screens_slide_done(void);

/* ---- anim.c, animdraw.c -------------------------------------------------------------------- */

enum anim_kind {
    ANIM_NONE,
    ANIM_OPEN,                     /* grows from 92% and fades in */
    ANIM_CLOSE,                    /* the reverse, of a window already gone */
    ANIM_MINIMISE,                 /* shrinks into its chip */
    ANIM_RESTORE,                  /* grows out of its chip */
    ANIM_SLIDE,                    /* two screens' windows slide sideways */
};

/* A window's picture for an animation: its frame (decorations and client
 * pixels, rounded corners transparent), premultiplied 0xAARRGGBB. */
struct anim_snap {
    uint32_t *px;                  /* w * h; NULL: none */
    int32_t   w, h;
    uint64_t  bytes;               /* big_alloc's */
};

/* Animations on or off (off: every one is at its end at once). */
void     anim_init(bool on);
bool     anim_enabled(void);
/* Each begins by ending the one running (an interrupted animation jumps to
 * its end). w is mapped (open, restore: drawn by the animation until its
 * end), about to be unmapped (minimise) or about to go (close). */
void     anim_open(struct comp_window *w);
void     anim_close(struct comp_window *w);
void     anim_minimise(struct comp_window *w, struct comp_box chip);
void     anim_restore(struct comp_window *w, struct comp_box chip);
/* Screen from's windows slide out towards -dir, to's in from dir. */
void     anim_slide(const struct desk_screen *from, const struct desk_screen *to, int dir);
void     anim_finish(void);
/* w is going: an animation that draws it ends. */
void     anim_forget(const struct comp_window *w);
enum anim_kind anim_running(void);
/* The clock: the animation at t (its boxes damaged); when it next needs a
 * tick (DEADLINE_NEVER: nothing runs). */
void     anim_tick(uint64_t t);
uint64_t anim_deadline(void);
/* Is s one of a running slide's two screens? */
bool     anim_slides(const struct desk_screen *s);

/* What the drawing reads (the loop sets it before a paint). */
struct anim_draw {
    enum anim_kind kind;
    const struct anim_snap *snap;  /* its picture */
    struct comp_box at;            /* where the picture is drawn now (scaled) */
    uint32_t alpha;                /* 0..255 */
    bool above_strip;              /* minimise and restore: over the strip */
};
bool     anim_now(struct anim_draw *out);   /* false: no picture to draw */

/* animdraw.c: w's picture (ERR_NO_MEMORY: none, the animation is skipped),
 * and giving one back; the picture where it meets t. */
status_t anim_snapshot(const struct comp_window *w, struct anim_snap *out);
void     anim_snapshot_free(struct anim_snap *s);
void     anim_draw(const struct tile_buf *t, bool above_strip);

/* ---- the fonts and the clock (desk.c) ------------------------------------------------------ */

/* The desktop's text, baked at desk_init: Inter at the look's sizes. NULL
 * where there was no memory (the text is then measured as 7 pixels a
 * character and not drawn). */
struct desk_fonts {
    struct font *r11, *m11;        /* labels, the calendar */
    struct font *r12, *m12;        /* the strip, chips, tiles' letters */
    struct font *r13, *m13;        /* rows, notifications' titles */
    struct font *r16;              /* the search box's line */
    struct font *m17;              /* the clock popover's time */
};
extern struct desk_fonts desk_font;
/* str's width in f (7 a character without the font). */
int      desk_text_w(const struct font *f, const char *str);

/* The local time now (or the one a test fixed), and whether the clock is set. */
bool     desk_time(struct civil *out);
/* Tests and the test scene: the time fixed (NULL: the clock again). */
void     desk_time_fix(const struct civil *c);

/* ---- strip.c ------------------------------------------------------------------------------- */

enum strip_part {
    STRIP_NONE,
    STRIP_JAM,                     /* "Jam OS": the search box */
    STRIP_DOT,                     /* screen `index`'s dot */
    STRIP_PLUS,                    /* a new screen */
    STRIP_CHIP,                    /* window `ww`'s chip */
    STRIP_NOCHIP,                  /* "No windows" */
    STRIP_MODE,                    /* floating or tiling */
    STRIP_NET,
    STRIP_VOL,
    STRIP_CLOCK,
};

#define STRIP_ITEMS_MAX  64u
#define STRIP_CHIP_TEXT  64u

struct strip_item {
    enum strip_part part;
    struct comp_box box;           /* on the output: what is drawn, what a click hits */
    uint32_t index;                /* STRIP_DOT: the screen's */
    struct wm_window *ww;          /* STRIP_CHIP: its window (valid until the next layout) */
    bool on;                       /* lit: the current dot, the focused chip, an open popover */
    bool minimised;                /* STRIP_CHIP */
    bool full;                     /* STRIP_DOT: a full-screen screen's */
    char text[STRIP_CHIP_TEXT];    /* a chip's title, the clock's text */
};

struct strip_layout {
    bool shown;                    /* the desktop is on and the current screen is normal */
    struct comp_box islands[3];    /* left, centre, right (empty: none) */
    struct strip_item items[STRIP_ITEMS_MAX];
    unsigned n;
};
extern struct strip_layout strip;

/* Something the strip shows changed: laid out again (and damaged) before
 * the next paint. */
void     strip_dirty(void);
/* Lay out now if anything changed; the strip's box damaged if so. */
void     strip_update(void);
struct comp_box strip_box(void);   /* empty while not shown */
/* The item at (x, y), or NULL. */
const struct strip_item *strip_at(int32_t x, int32_t y);
/* ww's chip's box (laid out first), or empty. */
struct comp_box strip_chip_box(const struct wm_window *ww);
/* The first item of a part (STRIP_VOL ...), or NULL. */
const struct strip_item *strip_find(enum strip_part part);
/* A press on the strip: what it does. */
void     strip_click(const struct strip_item *it);

/* ---- menus.c: the search box and Alt+Tab ---------------------------------------------------- */

/* The apps the search box lists (a fixed table until there is a list of
 * installed apps): the name shown, what it is, its tile's letter and
 * colour; the command is the name in lower case. */
#define DESK_APPS 9u
struct desk_app {
    const char *name, *desc;
    char letter;
    uint32_t colour;
};
extern const struct desk_app desk_apps[DESK_APPS];

#define SEARCH_TEXT_MAX 64u        /* bytes typed, at most */
#define SEARCH_ROWS_MAX (DESK_APPS + 1)

struct search_state {
    bool open;
    char text[SEARCH_TEXT_MAX];    /* what is typed (UTF-8) */
    unsigned row[SEARCH_ROWS_MAX]; /* the rows: an app's index, or DESK_APPS: "Run ..." */
    unsigned nrows, sel;
    uint64_t used[DESK_APPS];      /* when each app was last run from here (a count) */
};
extern struct search_state search;

void     search_toggle(void);
void     search_close(void);
/* A key while it is open (all of them are its): usage, xkb modifiers. */
void     search_key(uint16_t usage, uint32_t xkb_mods);
/* Its box, its rows' boxes. */
struct comp_box search_box(void);
struct comp_box search_row_box(unsigned i);
bool     search_press(int32_t x, int32_t y);   /* inside it: taken */

#define ALTTAB_MAX 64u
struct alttab_row {
    struct wm_window *ww;
    uint32_t group;                /* 0: this screen; s + 1: screen s's;
                                    * DESK_SCREENS_MAX + 1: the minimised */
    bool first;                    /* the first of its group: a divider (and label) above */
};
struct alttab_state {
    bool active;                   /* Alt held since a Tab */
    bool shown;                    /* held past LOOK_ALTTAB_SHOW_MS: the list shows */
    uint64_t since;
    struct alttab_row rows[ALTTAB_MAX];
    unsigned n, sel, top;          /* top: the first row shown */
};
extern struct alttab_state alttab;

/* Alt+Tab (back: Shift held): the list made, or the next row. */
void     alttab_step(bool back);
/* Alt let go (go: to the selected window) or Esc (not). */
void     alttab_end(bool go);
struct comp_box alttab_box(void);
struct comp_box alttab_row_box(unsigned i);   /* empty: scrolled out */
bool     alttab_press(int32_t x, int32_t y);

/* ---- popover.c ----------------------------------------------------------------------------- */

enum pop_kind { POP_NONE, POP_VOLUME, POP_NETWORK, POP_CLOCK };

/* What the network popover shows (ctl_network). */
struct desk_net {
    bool up;                       /* the link is up */
    char address[48];              /* "192.168.1.57", "" none yet */
    char nic[32];                  /* "RTL8125B" */
    uint32_t mbps;                 /* the link's speed; 0: unknown */
    uint64_t rx_bps, tx_bps;       /* bytes a second, now */
};

struct pop_state {
    enum pop_kind kind;
    struct comp_box box;
    uint32_t volume;               /* 0..100, as last read or set */
    bool dragging;                 /* the slider's grab is on */
    struct desk_net net;
    char output[48], playing[64];
    struct civil now;              /* the clock popover's time */
    uint64_t refreshed;            /* when the data was read */
};
extern struct pop_state pop;

/* Open kind under the strip item at opener (its right edge), or close it if
 * it is the one open. */
void     pop_toggle(enum pop_kind kind, struct comp_box opener);
void     pop_close(void);
/* Its parts: the slider's track. */
struct comp_box pop_slider_box(void);
bool     pop_press(int32_t x, int32_t y);
/* The data read again (the clock's minute, the network's rates). */
void     pop_refresh(uint64_t t);
/* The calendar for c's month: cells[i] the day in cell i (0: blank),
 * Monday first, six weeks of seven; the number of cells used. */
unsigned pop_calendar(const struct civil *c, uint8_t cells[42]);

/* ---- notify.c ------------------------------------------------------------------------------ */

#define NOTIFY_MAX          5u     /* cards shown at once (the oldest without buttons goes first) */
#define NOTIFY_BUTTONS_MAX  3u
#define NOTIFY_TEXT_MAX     96u

/* A notification to post (comp.h: notify_post). */
struct notify_spec {
    const char *title, *body;      /* body may be NULL */
    char letter;                   /* the tile's letter (0: an "i") */
    uint32_t colour;               /* the tile's colour (0: blackcurrant) */
    const char *buttons[NOTIFY_BUTTONS_MAX];   /* the first is the main one */
    unsigned nbuttons;
};

struct notify_card {
    uint32_t id;                   /* 0: a free slot */
    char title[NOTIFY_TEXT_MAX], body[NOTIFY_TEXT_MAX];
    char letter;
    uint32_t colour;
    char buttons[NOTIFY_BUTTONS_MAX][24];
    unsigned nbuttons;
    uint64_t posted, leaving;      /* when it came, when it began to fade (0: not) */
    struct comp_box box;           /* where it is now */
    uint32_t alpha;                /* 0..255, now */
    int32_t dx;                    /* sliding in or out: pixels to the right */
};

struct notify_state {
    struct notify_card cards[NOTIFY_MAX];   /* the newest first */
    unsigned n;
    uint32_t next_id;
};
extern struct notify_state notes;

uint32_t notify_post(const struct notify_spec *n);
void     notify_withdraw(uint32_t id);
/* The cards' places and fades at t; when the next change is due. */
void     notify_tick(uint64_t t);
uint64_t notify_deadline(void);
/* The box round every card (empty: none); a card's buttons' boxes. */
struct comp_box notify_box(void);
struct comp_box notify_button_box(const struct notify_card *c, unsigned b);
bool     notify_press(int32_t x, int32_t y);

/* ---- desk.c -------------------------------------------------------------------------------- */

/* The desktop's damage: drawn over the windows, so it never makes what is
 * behind a card stale (frost.c's backdrops); b with the shadow a card
 * casts around it. */
void     desk_damage(struct comp_box b);
/* Run an app (ctl_launch), the cursor busy until its first window maps
 * (at most DESK_BUSY_NS); or a command in a terminal. */
#define DESK_BUSY_NS (10 * NS_PER_S)
void     desk_launch(const char *app);
void     desk_run(const char *cmd);
/* ww's first buffer was mapped (wm.c): the busy cursor ends if it is the
 * app being launched. */
void     desk_window_mapped(const struct wm_window *ww);

/* ---- the drawing (stripdraw.c, menudraw.c, popdraw.c, frost.c, ui.c) ------------------------- */

/* frost.c */
/* The strip's frosted picture for the output (once; ERR_NO_MEMORY: a flat
 * tint). The cards' backdrops are made as they open. */
status_t frost_init(int32_t w, int32_t h);
/* Row y of the strip's picture (y < LOOK_STRIP_H), or NULL. */
const uint32_t *frost_strip_row(int32_t y);
/* A card's backdrop: what is behind box b, blurred; slot: which card. */
enum frost_slot { FROST_MENU, FROST_POP, FROST_NOTES, FROST_SLOTS };
/* Before a paint: each card whose backdrop is stale (just opened, moved,
 * or something behind it was damaged: the scene's `under` damage) has its
 * box damaged; after planning, frost_build makes those again (on the
 * painting workers). */
void     frost_prepare(void);
void     frost_build(void);
/* Pixel row y of slot's backdrop from x (inside its box), or NULL. */
const uint32_t *frost_row(enum frost_slot slot, int32_t x, int32_t y);
/* A card's box for slot (empty: closed), as the drawing has it now. */
struct comp_box frost_card_box(enum frost_slot slot);

/* ui.c: drawing into a tile (all clipped to it). */
/* A rounded box b of colour rgb at alpha a (0..255) over the tile. */
void     ui_round(const struct tile_buf *t, struct comp_box b, int32_t r, uint32_t rgb, uint32_t a);
/* A glass card (look.h): its shadow, the blurred backdrop of slot tinted,
 * its outline; overall alpha a (a card fading). */
void     ui_glass(const struct tile_buf *t, struct comp_box b, enum frost_slot slot, uint32_t a);
/* A 1-pixel divider from x1 to x2 at row y. */
void     ui_divider(const struct tile_buf *t, int32_t x1, int32_t x2, int32_t y);
/* A letter tile: a rounded square of colour col with letter (or an "i"). */
void     ui_tile(const struct tile_buf *t, struct comp_box b, int32_t r, uint32_t col, char letter,
                 const struct font *f);
/* Text in box b (one line, cut with "…", capitals centred down it). */
void     ui_text(const struct tile_buf *t, struct comp_box b, const struct font *f, uint32_t rgb,
                 bool centre, const char *str);
/* The icons, drawn in box b (square, LOOK_ICON_* sizes). */
enum ui_icon { UI_FLOATING, UI_TILING, UI_NETWORK, UI_VOLUME, UI_SEARCH, UI_TERMINAL, UI_INFO,
               UI_PLUS };
void     ui_icon(const struct tile_buf *t, struct comp_box b, enum ui_icon icon, uint32_t rgb);
/* Corner coverage for radius r (1..UI_RADIUS_MAX): the top left r by r. */
#define UI_RADIUS_MAX 16
const uint8_t *ui_corner(int32_t r);
void     ui_init(void);

/* deskpaint.c: what paint.c calls. */
/* Before a paint is planned: the strip laid out, stale backdrops damaged. */
void     desk_paint_prepare(void);
/* After it is planned: the backdrops made. */
void     desk_paint_backdrops(void);
/* Does the desktop hide all of b (inside the strip): nothing under it drawn? */
bool     desk_hides(struct comp_box b);
/* Does it draw anything over the windows now (no full-screen copy then)? */
bool     desk_over_windows(void);
/* Its layers where they meet t: over the windows the opening and closing
 * pictures, the strip and the minimising one (low); the cards (high). */
void     desk_draw_low(const struct tile_buf *t);
void     desk_draw_high(const struct tile_buf *t);

/* stripdraw.c, menudraw.c, popdraw.c */
void     strip_draw(const struct tile_buf *t);
void     search_draw(const struct tile_buf *t);
void     alttab_draw(const struct tile_buf *t);
void     pop_draw(const struct tile_buf *t);
void     notify_draw(const struct tile_buf *t);
