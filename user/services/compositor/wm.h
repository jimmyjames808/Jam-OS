/* wm.h: the window manager's inside (wm.c, wmtile.c, wmgrab.c, deco.c),
 * and what xdg-shell (xdg.c, xdgtop.c) and the tests tell it. What other
 * tracks call (the seat, painting) is in comp.h.
 *
 * The model. Each toplevel has a struct wm_window from get_toplevel to its
 * destroy; it gets a struct comp_window (the scene's) while it is mapped.
 * The window manager never speaks the protocol: it decides a configure (a
 * size and states, struct wm_config) and hands it to the role's ops, which
 * send it when the client may hear it; the client acks it and commits a
 * buffer, and the commit tells the window manager which states the buffer
 * was drawn for (wm_commit's states). Placement follows those states, not
 * the wanted ones, so a client that is slow to answer keeps its old place
 * and size until it does (the plan's "the frame just isn't resized").
 *
 * Floating and tiling are two ways of placing the same windows: the
 * floating place and size (float_*) are kept while tiling, so switching
 * back puts every window where it was. Tiling is dwindle (wmtile.c): each
 * tiling screen has a binary tree of splits whose leaves are its windows.
 *
 * Threads: one, the compositor's loop (comp.h). Memory: one struct per
 * toplevel, bounded by the client's surfaces (COMP_SURFACES_MAX). */
#pragma once

#include "comp.h"

#define WM_TEXT_MAX 256   /* a title's or app id's bytes kept, the NUL included (the plan's cap) */

/* The states of a configure (xdg_toplevel.state, as bits). */
#define WM_ST_MAXIMIZED  (1u << 0)
#define WM_ST_FULLSCREEN (1u << 1)
#define WM_ST_RESIZING   (1u << 2)
#define WM_ST_ACTIVATED  (1u << 3)

/* Resize edges: xdg_toplevel.resize_edge's bits (xdgtop.c checks they match). */
#define WM_EDGE_TOP    1u
#define WM_EDGE_BOTTOM 2u
#define WM_EDGE_LEFT   4u
#define WM_EDGE_RIGHT  8u

/* A configure: the surface size suggested (0: the client picks) and the states. */
struct wm_config {
    int32_t width, height;
    uint32_t states;               /* WM_ST_* */
};

/* What the role does for the window manager. */
struct wm_ops {
    /* The configure the window manager wants now: send it when the client
     * may hear it (after its initial commit), or keep it until then. Called
     * whenever anything that goes into it may have changed: the role drops
     * repeats. */
    void (*configure)(void *ctx, const struct wm_config *cfg);
    /* The user asked the window to close (its close circle, Super+Q). */
    void (*close)(void *ctx);
};

/* What the window asks to be (the client's or the user's request). */
enum wm_mode {
    WM_NORMAL,                     /* floating at its place, or in its tile */
    WM_MAXIMIZED,                  /* the whole output below its title bar */
    WM_FULLSCREEN,                 /* the whole output, no decorations */
};

struct wm_window {
    struct wm_window *prev, *next; /* every toplevel, oldest first: tiling and Alt+Tab order */
    struct comp_surface *surface;
    struct comp_window *win;       /* while mapped; win->wm is this */
    const struct wm_ops *ops;
    void *ctx;                     /* the ops' own */
    char title[WM_TEXT_MAX], app_id[WM_TEXT_MAX];
    int32_t min_w, min_h, max_w, max_h;   /* the client's limits; 0: none */
    enum wm_mode want;             /* asked for */
    enum wm_mode before_fs;        /* what full screen returns to */
    uint32_t shown;                /* WM_ST_* the shown buffer was drawn for */
    bool float_placed;             /* float_x, float_y are set */
    int32_t float_x, float_y;      /* floating: where the surface's (0, 0) goes */
    int32_t float_w, float_h;      /* floating: its size to ask for; 0: the client's own
                                    * (a mapped window with none: wm.c's float_default) */
    uint32_t anchor;               /* WM_EDGE_LEFT/TOP: a resize keeps the opposite side still */
    int32_t anchor_x2, anchor_y2;  /* ... that side: the surface's right or bottom edge */
    bool resizing;                 /* a resize grab is on it */
    bool not_responding;           /* asked to close, no pong since: the title bar says so */
    /* The desktop's (screens.c): the virtual screen it is on while mapped,
     * the one a full-screen window came from, minimised or not, a boot
     * overlay (on no screen: over all of them), and when it last had the
     * focus (Alt+Tab's order, most recent first). */
    struct desk_screen *screen;
    struct desk_screen *home;
    bool minimised;
    bool overlay;
    uint64_t focused_at;           /* a count, not a time: 0 never */
    /* Tiling's (wmtile.c): its leaf in a screen's tree, and that screen. */
    struct tile_node *leaf;        /* NULL: not tiled */
    struct desk_screen *tiled_on;
    /* A glide to its new tile (anim.c): where its surface was shown when
     * the glide began, and whether it is gliding. */
    int32_t glide_x, glide_y;
    bool glide_noted, gliding;
    /* The memory (wmsave.c): its key (jam_window_memory_v1; 0: none
     * asked), and whether it took a remembered place it hasn't been
     * mapped into yet. */
    uint64_t key;
    bool restoring;
};

/* ---- wm.c --------------------------------------------------------------------------- */

/* No windows, no focus, no grab, the given layout: the start (and each test). */
void wm_init(enum comp_layout layout);
/* A toplevel on s, told things through ops (with ctx). NULL: no memory. */
struct wm_window *wm_create(struct comp_surface *s, const struct wm_ops *ops, void *ctx);
/* The toplevel goes: unmapped, forgotten, freed. Nothing is sent to it. */
void wm_destroy(struct wm_window *ww);
/* The configure ww should have now. */
void wm_wanted(const struct wm_window *ww, struct wm_config *out);
/* wm_wanted handed to ww's ops. */
void wm_reconfigure(struct wm_window *ww);
/* A commit with a buffer, drawn for states (the configure it acked): ww is
 * mapped (the first time: placed, and the seat told by the scene) and
 * placed for those states. ERR_NO_MEMORY. */
status_t wm_commit(struct wm_window *ww, uint32_t states);
/* A commit without a buffer: ww is unmapped and back where get_toplevel
 * left it (its requested states and floating place forgotten). */
void wm_unmap(struct wm_window *ww);
void wm_set_title(struct wm_window *ww, const char *title);   /* cut to WM_TEXT_MAX */
void wm_set_app_id(struct wm_window *ww, const char *app_id);
/* The client's limits (checked by the caller: 0 or more, max >= min where both are set). */
void wm_set_limits(struct wm_window *ww, int32_t min_w, int32_t min_h, int32_t max_w,
                   int32_t max_h);
/* Can the user change ww's size: a limit that pins neither side. */
bool wm_resizable(const struct wm_window *ww);
/* The client's or user's request: maximised on or off, full screen on or
 * off (off returns to what it was before). */
void wm_request_maximized(struct wm_window *ww, bool on);
void wm_request_fullscreen(struct wm_window *ww, bool on);
void wm_set_not_responding(struct wm_window *ww, bool on);
/* Every toplevel's configure renewed and every mapped one placed again
 * (a layout change, a window joining or leaving the tiles). */
void wm_relayout(void);
/* ww placed for its shown states and the layout. */
void wm_place(struct wm_window *ww);
/* The oldest toplevel (then ->next), for walking them. */
struct wm_window *wm_first(void);

/* The arrangement of ww's screen (the default's before it has one). */
enum comp_layout wm_layout_of(const struct wm_window *ww);
/* The toplevel with the keyboard focus, or NULL. */
struct wm_window *wm_focused(void);
/* Focus changes count on from at least n (a restored focus order, wmsave.c). */
void wm_focus_count_at_least(uint64_t n);

/* Every toplevel's configure renewed and placed again, as wm_relayout,
 * with the tiles that move gliding to their new places (anim.c): a window
 * opening or going, a swap, a keyboard resize. */
void wm_reflow(void);
/* The top a window's frame may reach on ww's screen: the floor under the
 * strip (the output's top without the desktop). */
int32_t wm_floor(const struct wm_window *ww);
/* Can ww be moved and resized by the user: floating, in its normal state. */
bool wm_floating_normal(const struct wm_window *ww);

/* ---- wmtile.c: dwindle -------------------------------------------------------------- */

#define WM_GAP       6       /* background between tiles, and around them */
#define TILE_ONE     65536   /* a split's ratio: all of the room */
#define TILE_MIN     9830    /* 15%: a split's ratio at least ... */
#define TILE_MAX     55706   /* ... and 85% at most */
#define WM_PUSH      48      /* Super+Alt+direction: pixels an edge moves */
#define WM_GAP_HIT   12      /* a gap takes presses this wide, centred on it */
#define WM_TILE_MIN_W 200    /* a tile is halved across only if the halves keep this, */
#define WM_TILE_MIN_H 120    /* ... down this; else the window goes to a new screen (wmtile.c) */

/* A node of a screen's tree: a leaf (a window's tile) or a split of its
 * room in two, a beside b (across) or a above b, with WM_GAP between. */
struct tile_node {
    struct tile_node *up;          /* the split it is half of; NULL: the root */
    struct tile_node *a, *b;       /* a split's halves (a left or above); NULL: a leaf */
    struct wm_window *ww;          /* a leaf's window */
    bool across;                   /* a split: side by side; else one over the other */
    int32_t ratio;                 /* a split: a's share of the room less the gap, of TILE_ONE */
    struct comp_box box;           /* the last layout: a leaf's tile, a split's room */
    /* The last layout: the least room the node's windows need (a leaf:
     * the tiler's minimum, or its window's own minimum and border if that
     * is more; a split along its axis: both halves' and the gap, across
     * it: the larger half's). A split's halves never get less than their
     * need while its room has it (its ratio a wish, held to it), and a
     * gap dragged or pushed stops there (tiles_gap_move). */
    int32_t need_w, need_h;
    /* A placeholder (wmsave.c's restore: ww NULL): the key of the window
     * whose place it keeps until it comes back, and that window's
     * minimum, for its need. 0: none. */
    uint64_t hold;
    int32_t hold_min_w, hold_min_h;
};

/* A gap between two tiles: the split it belongs to, where it is, the box
 * that takes its presses, the bar drawn when it is lit (look.h). */
struct tile_gap {
    struct tile_node *split;
    struct comp_box gap, hit, bar;
};

/* Every tiling screen's tree made to match its tiled windows (mapped,
 * shown on it, not minimised: a window gone leaves its sibling the whole
 * of their parent; a new one splits the focused tile, or the last, in
 * half along its longer side; a tree made from nothing takes the windows
 * in their order, each splitting the last; a window whose split would
 * leave halves under WM_TILE_MIN_W wide or WM_TILE_MIN_H high, or either
 * window's own minimum outside its half, goes to a new screen at the end
 * instead, screens_spill, the screen it would have split unchanged) and
 * laid out in its room; the trees of floating screens dropped. */
void tiles_update(void);
/* s's tree dropped (its windows untiled): a screen going, or switched. */
void tiles_drop(struct desk_screen *s);
/* The tiler's minimum for a half, WM_TILE_MIN_W by WM_TILE_MIN_H unless
 * the compositor's `tilemin=<w>x<h>` (a test's small output) says. */
void tiles_set_min(int32_t w, int32_t h);
/* ww's tile gone from its tree, its sibling taking their parent's room
 * (it is unmapped or going). */
void tiles_forget(struct wm_window *ww);
/* The tile ww has in tiling, or will have once mapped (the half of the
 * tile a new window splits). */
struct comp_box wm_tile(const struct wm_window *ww);
/* A tree's changes so far: a node pointer kept across them is stale. */
uint64_t tiles_generation(void);
/* a and b, tiled on one screen, trade tiles. False: they aren't. */
bool tiles_swap(struct wm_window *a, struct wm_window *b);
/* The gap of s's tree whose hit box holds (x, y), the outermost first.
 * False: none. */
bool tiles_gap_at(const struct desk_screen *s, int32_t x, int32_t y, struct tile_gap *out);
/* A split's gap. */
struct tile_gap tiles_gap(struct tile_node *split);
/* The split's gap moved so its middle is at pos (x across, else y), its
 * ratio kept within TILE_MIN and TILE_MAX, and stopped where either half
 * would get less than its windows need (its need_w or need_h: a gap
 * already past that moves only back towards it). The caller lays out
 * again. */
void tiles_gap_move(struct tile_node *split, int32_t pos);
/* Super+Alt+direction: ww's tile's edge on that side pushed WM_PUSH
 * that way (with no edge there, its other edge). False: nothing moved. */
bool tiles_push(struct wm_window *ww, int dx, int dy);

/* A node for tiles_build, in pre-order: a split (across or down, its
 * ratio) or a leaf keeping the place of the window with key `hold` (0: a
 * window not remembered, whose place goes at once, as a closed one's). */
struct tile_spec {
    bool split, across;
    int32_t ratio;
    uint64_t hold;
    int32_t min_w, min_h;          /* a leaf's window's minimum */
};
/* s's tree (s has none) made from spec[0..n), a whole tree in pre-order
 * (checked: false, and no tree, if it isn't), its leaves placeholders,
 * those of no window gone at once, laid out. */
bool tiles_build(struct desk_screen *s, const struct tile_spec *spec, unsigned n);
/* The box of the placeholder keeping key's place (on any screen). False: none. */
bool tiles_held_box(uint64_t key, struct comp_box *out);
/* ww (mapped now, ww->key its key) into the placeholder keeping its place,
 * if it is shown tiled on that placeholder's screen; else the placeholder
 * goes, as a closed window's place. */
void tiles_take_hold(struct wm_window *ww);
/* Every placeholder gone, as closed windows' places: how many. */
unsigned tiles_drop_holds(void);

/* ---- wmsave.c: the arrangement a restarted compositor comes back to -------------------- */

#define WM_SAVE_MAGIC   0x4a4d4c57u   /* "WLMJ" */
#define WM_SAVE_VERSION 1u
#define WM_SAVE_SCREENS 16u           /* desk.h's DESK_SCREENS_MAX */
#define WM_SAVE_WINDOWS 64u           /* windows remembered at most (more: not remembered) */
#define WM_SAVE_NODES   256u          /* tree nodes, all screens (a tree past it: not kept) */
#define WM_SAVE_SPLIT   0xffffu       /* a node's win: a split */
#define WM_SAVE_GONE    0xfffeu       /* ... a leaf of a window not remembered */
#define WM_SAVE_HOLD_NS (5 * NS_PER_S)   /* how long a remembered place waits for its window */
#define WM_SAVE_MINIMISED    1u       /* a window's flags */
#define WM_SAVE_FLOAT_PLACED 2u
#define WM_SAVE_FLOAT_SIZE   4u

struct wm_save_screen {
    uint16_t first, n;             /* its tree's nodes, pre-order (n 0: no tree) */
    uint8_t layout;                /* enum comp_layout */
    uint8_t spill;
    uint8_t pad[2];
};
struct wm_save_window {
    uint64_t key;
    uint64_t rank;                 /* its focused_at (0: never focused) */
    int32_t fx, fy, fw, fh;        /* its floating place and size */
    int32_t min_w, min_h;          /* its minimum (its placeholder's need) */
    uint8_t screen;                /* a normal screen's index (a full-screen window's home) */
    uint8_t want, before_fs;       /* enum wm_mode */
    uint8_t flags;                 /* WM_SAVE_MINIMISED, ... */
    uint8_t pad[4];
};
struct wm_save_node {
    int32_t ratio;                 /* a split's */
    uint16_t win;                  /* a leaf's window (an index), WM_SAVE_SPLIT or WM_SAVE_GONE */
    uint8_t across;
    uint8_t pad;
};
/* The description: the normal screens in order (full-screen ones are
 * made again by their windows), the windows with keys, each tiling
 * screen's tree. Fixed size; checked whole before anything of it is used. */
struct wm_save {
    uint32_t magic, version, bytes;  /* WM_SAVE_MAGIC, WM_SAVE_VERSION, sizeof(struct wm_save) */
    uint32_t nscreens, nwins, nnodes;
    uint32_t cur;                    /* the normal screen shown (a full screen's: its home) */
    uint32_t deflt;                  /* new screens' layout */
    uint64_t focused;                /* the focused window's key (0: none) */
    uint64_t cur_full;               /* the key of the window whose full screen was shown (0) */
    uint64_t sum;                    /* FNV-1a 64 of the whole with sum 0 */
    struct wm_save_screen screens[WM_SAVE_SCREENS];
    struct wm_save_window wins[WM_SAVE_WINDOWS];
    struct wm_save_node nodes[WM_SAVE_NODES];
};

/* The arrangement now (mapped windows with keys; no boot overlay). */
void     wm_save_describe(struct wm_save *out);
/* A description's checksum (its sum field left out). */
uint64_t wm_save_sum(const struct wm_save *d);
/* A fresh compositor (no windows yet) takes d: its screens, their
 * layouts and trees with a placeholder in each remembered window's tile,
 * the screen shown; each remembered window waits WM_SAVE_HOLD_NS from t
 * for its client to present its key. ERR_INVALID_ARGS: d fails a check
 * (said in the log, nothing used); ERR_BAD_STATE: windows already. */
status_t wm_save_load(const struct wm_save *d, uint64_t t);
/* Remembered windows still awaited; when the wait ends. */
bool     wm_save_pending(void);
uint64_t wm_save_deadline(void);
/* At t: the wait over, the places of windows not back go as closed ones'. */
void     wm_save_tick(uint64_t t);
/* jam_window_memory_v1.identify: ww's key; `presented` (0: none) takes a
 * remembered place if one waits under it and ww isn't mapped yet. */
uint64_t wm_save_key(struct wm_window *ww, uint64_t presented);
/* wm.c, at ww's first buffer: ww onto its remembered screen and place
 * (instead of screens_window_new). False: not a remembered window. */
bool     wm_save_place(struct wm_window *ww);
/* ... and once it is mapped: the focus to the remembered window if it is
 * back; the last one back ends the wait. */
void     wm_save_mapped(struct wm_window *ww);
/* Was ww's full screen the one shown? */
bool     wm_save_shown_full(const struct wm_window *ww);
/* Nothing remembered or awaited (wm_init). */
void     wm_save_forget(void);
/* main.c: the state VMO (SR_STATE; HANDLE_INVALID: no memory), mapped,
 * and a dead compositor's description in it taken (wm_save_load); then
 * at each loop turn the wait's end and the arrangement written there if
 * it changed (two copies, the commit word last). */
status_t wm_save_open(handle_t vmo, uint64_t t);
void     wm_save_turn(uint64_t t);

/* ---- wmgrab.c ----------------------------------------------------------------------- */

/* ww is going or unmapping: a grab on it ends, a double-click forgets it. */
void wm_grab_forget(const struct wm_window *ww);
/* Any grab of ours ends where it is (the layout switched; a fresh start). */
void wm_grab_cancel(void);
/* The resize cursor for a gap a press at (x, y) would drag (gap_under),
 * else CURSOR_SHAPES. */
enum cursor_shape wm_gap_cursor(int32_t x, int32_t y);
/* Before a paint: the gap under the pointer (or being dragged) lit, its
 * bar damaged when that changes (desk.h's wm_marks). */
void wm_marks_update(void);
/* The time between a double-click's two presses, at most. */
#define WM_DOUBLE_CLICK_NS (400 * NS_PER_MS)
#define WM_MIN_SIDE        32   /* a resize never asks for less than this */

/* ---- deco.c ------------------------------------------------------------------------- */

/* Decorations for a buffer drawn for states, in layout (tiling: no title
 * bar, a border all round): sizes around the surface. */
struct deco_sizes {
    int32_t top, left, right, bottom;
};
struct deco_sizes deco_sizes(uint32_t states, enum comp_layout layout);
/* The surface's box inside a frame box for states in layout. */
struct comp_box deco_inner(struct comp_box frame, uint32_t states, enum comp_layout layout);
/* w's decorations set for states (its frame damaged if they change). */
void deco_set(struct comp_window *w, uint32_t states);

enum deco_part {
    DECO_NONE,                     /* not in w's frame */
    DECO_SURFACE,                  /* the client's */
    DECO_TITLE,
    DECO_CLOSE,                    /* the title bar's circles */
    DECO_MINIMISE,
    DECO_FULLSCREEN,
    DECO_EDGE,                     /* a border or corner: *edges says which */
};
enum deco_part deco_hit(const struct comp_window *w, int32_t x, int32_t y, uint32_t *edges);
