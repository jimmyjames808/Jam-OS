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
 * back puts every window where it was.
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
    int32_t float_w, float_h;      /* floating: its size to ask for; 0: the client's own */
    uint32_t anchor;               /* WM_EDGE_LEFT/TOP: a resize keeps the opposite side still */
    int32_t anchor_x2, anchor_y2;  /* ... that side: the surface's right or bottom edge */
    bool resizing;                 /* a resize grab is on it */
    bool not_responding;           /* asked to close, no pong since: the title bar says so */
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

/* ---- wmtile.c ----------------------------------------------------------------------- */

/* Tile i of n (frame boxes, decorations included) in area: the master and stack layout. */
struct comp_box wm_tile_box(struct comp_box area, unsigned n, unsigned i);
/* The tile ww has in tiling, or will have once mapped: counted among the
 * mapped toplevels, oldest first. */
struct comp_box wm_tile(const struct wm_window *ww);

/* ---- wmgrab.c ----------------------------------------------------------------------- */

/* ww is going or unmapping: a grab on it ends, a double-click forgets it. */
void wm_grab_forget(const struct wm_window *ww);
/* Any grab of ours ends where it is (the layout switched; a fresh start). */
void wm_grab_cancel(void);
/* The time between a double-click's two presses, at most. */
#define WM_DOUBLE_CLICK_NS (400 * NS_PER_MS)
#define WM_MIN_SIDE        32   /* a resize never asks for less than this */

/* ---- deco.c ------------------------------------------------------------------------- */

/* Decorations for a buffer drawn for states, in the layout now (tiling: no
 * title bar, a border all round): sizes around the surface. */
struct deco_sizes {
    int32_t top, left, right, bottom;
};
struct deco_sizes deco_sizes(uint32_t states);
/* The surface's box inside a frame box for states. */
struct comp_box deco_inner(struct comp_box frame, uint32_t states);
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
