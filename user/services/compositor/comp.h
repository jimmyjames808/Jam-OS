/* comp.h: the compositor's shared model (docs/G1-PLAN.md), what every file
 * of user/services/compositor/ builds on: the clients and their Wayland
 * objects, the surfaces with their double-buffered state, the windows in
 * stacking order, the output and its damage.
 *
 * Who owns what (the plan's tracks; a change to this header is named in
 * the track's report):
 *   main.c      the loop: the port, the paint clock, the order of a turn;
 *   conn.c      connections (svc.connect on /svc/wayland), each client's
 *               budget and caps, dispatch, protocol errors, teardown;
 *   display.c   wl_display, wl_registry and the globals, wl_compositor,
 *               wl_region's requests, wl_output, wl_callback;
 *   surface.c   wl_surface: pending and current state, commit, frame
 *               callbacks, buffer release;
 *   shm.c       wl_shm, wl_shm_pool (VMO_KEEP_PAGES pools mapped
 *               VMAR_KEPT_ONLY), wl_buffer;
 *   region.c    boxes, regions (wl_region, input and opaque) and damage
 *               lists: pure code, no protocol;
 *   scene.c     windows: stacking, position, damage, what is under a point;
 *   output.c    the output: the framebuffer (framebuffer_take), or an image
 *               in memory when there is none (headless: tests, screenshots);
 *   paint.c     composing the damage onto the output: tiles on workers,
 *               opaque windows hiding what is below, the full-screen path;
 *   title.c     drawing title bars, their circles and borders (their sizes
 *               and what a click on them does are the window manager's);
 *   shape.c     rounded corners and shadows; mask.c the shapes they draw;
 *   wallpaper.c the background; look.h every colour and size of the look;
 *   cursor.c    the pointer's picture: the arrow, hidden, or a client's;
 *   clock.c     the paint clock (display.hz) and the frame callbacks' turn;
 *   testscene.c windows of known pixels with no client (a test power);
 *   paint.h     what those share.
 * Later tracks: xdg-shell and window management (xdg.c, wm.c, deco.c), the
 * seat and compctl (seat.c, keyboard.c, pointer.c, focus.c, sources.c,
 * shapes.c), the cursor set (cursors.c, drawn by tools/cursorgen.c), and the
 * desktop around the windows: virtual screens, the top bar, the menus,
 * popovers, notifications, animations and frosting (desk.h has its files).
 *
 * The model:
 *   - a client is one connection (struct comp_client); everything it made
 *     is on its own lists and goes when it goes (client_teardown), so a
 *     client that dies or breaks the protocol leaves nothing behind;
 *   - a surface (struct comp_surface) has pending state that requests
 *     change and current state that commit makes of it, at once, as the
 *     protocol says; it shows nothing until a role (xdg_toplevel, cursor)
 *     gives it a window;
 *   - a window (struct comp_window) is a surface placed on the output: a
 *     position, a stacking place, a frame (the surface plus decorations).
 *     Floating and tiling are policies over the same windows (the window
 *     manager moves and sizes them; the scene only keeps and damages);
 *   - the output (struct comp_scene) is one screen of pixels and its
 *     damage: what must be composed again at the next paint.
 *
 * Threads: the loop's, and a few painting workers (paint.c). The loop
 * serves the clients and paints in turn, and waits while the workers
 * paint, so nothing here takes a lock: nothing changes the scene while a
 * paint reads it, and the workers only read it.
 *
 * Memory: the compositor's own structures for a client are bounded by the
 * caps below and freed with it. A client's pools are its own VMOs (pages
 * charged to its job); our mappings of them, page tables included, are
 * charged to our job and bounded by COMP_POOL_BYTES_MAX. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jwl.h>
#include <os.h>

/* ---- limits (G1-PLAN, "Limits and the security model") ---------------------------- */

#define COMP_CLIENTS_MAX        64u            /* connections at once, for the whole compositor */
#define COMP_SURFACES_MAX       64u            /* per client */
#define COMP_POOLS_MAX          64u            /* per client */
#define COMP_POOL_BYTES_MAX     (256ull << 20) /* per client: pool bytes mapped, in total */
#define COMP_BUFFERS_MAX        256u           /* per client */
#define COMP_REGIONS_MAX        64u            /* per client: wl_region objects */
#define COMP_OUTPUTS_MAX        16u            /* per client: wl_output objects bound */
#define COMP_FRAMES_MAX         64u            /* per surface: frame callbacks pending and waiting */
#define COMP_REGION_RECTS_MAX   256u           /* boxes in one region */
#define COMP_CLIENT_RECTS_MAX   4096u          /* per client: boxes in all its regions */
#define COMP_SURFACE_DAMAGE_MAX 32u            /* damage boxes kept per surface (more: merged) */
#define COMP_OUTPUT_DAMAGE_MAX  64u            /* damage boxes kept for the output (more: merged) */
#define COMP_BUFFER_SIDE_MAX    8192           /* a buffer's width and height: 1 to this */
#define COMP_BUDGET_MSGS        64u            /* messages of one client per turn */
#define COMP_BUDGET_NS          (2 * NS_PER_MS)/* ... or this long, whichever ends first */
#define COMP_HIDDEN_FRAME_NS    NS_PER_S       /* a surface nobody sees: its callbacks this often */

/* The globals, as advertised (the plan's table; a client binds the lower
 * of these and its own). */
#define COMP_COMPOSITOR_VERSION 4u
#define COMP_SHM_VERSION        1u
#define COMP_OUTPUT_VERSION     3u
#define COMP_SEAT_VERSION       5u
#define COMP_XDG_WM_VERSION     1u

/* ---- boxes, regions, damage (region.c) --------------------------------------------- */

/* A box of whole pixels, x1 <= x < x2, y1 <= y < y2; empty when either
 * side is 0 or less. Coordinates stay within +-COMP_COORD_MAX, so sums
 * and differences of two never overflow. */
#define COMP_COORD_MAX (1 << 28)
struct comp_box {
    int32_t x1, y1, x2, y2;
};

/* The box at (x, y) of w by h, clamped to +-COMP_COORD_MAX; empty if w or
 * h is 0 or less. */
struct comp_box box_make(int32_t x, int32_t y, int32_t w, int32_t h);
bool            box_empty(struct comp_box b);
struct comp_box box_intersect(struct comp_box a, struct comp_box b);   /* empty if none */
struct comp_box box_bounds(struct comp_box a, struct comp_box b);      /* smallest holding both */
struct comp_box box_translate(struct comp_box b, int32_t dx, int32_t dy);   /* clamped */
bool            box_contains(struct comp_box b, int32_t x, int32_t y);

/* A set of pixels: disjoint boxes, at most COMP_REGION_RECTS_MAX. charge
 * (may be NULL) is the owning client's count of boxes in all its regions,
 * kept up to date and held to COMP_CLIENT_RECTS_MAX. */
struct comp_region {
    struct comp_box *b;      /* n boxes, room for cap */
    uint32_t n, cap;
    uint32_t *charge;        /* the client's box count, or NULL */
};

void     region_init(struct comp_region *g, uint32_t *charge);
void     region_fini(struct comp_region *g);    /* frees, uncharges; g is empty after */
void     region_clear(struct comp_region *g);
/* g gets box's pixels too (add) or loses them (subtract). On failure g
 * is unchanged: ERR_NO_RESOURCES past COMP_REGION_RECTS_MAX boxes or the
 * client's COMP_CLIENT_RECTS_MAX; ERR_NO_MEMORY. */
status_t region_add(struct comp_region *g, struct comp_box box);
status_t region_subtract(struct comp_region *g, struct comp_box box);
/* dst (keeping its own charge) the same pixels as src. Errors as above. */
status_t region_copy(struct comp_region *dst, const struct comp_region *src);
bool     region_contains(const struct comp_region *g, int32_t x, int32_t y);
struct comp_box region_extents(const struct comp_region *g);   /* empty for an empty region */

/* Damage: boxes that may overlap, at most max of them; one more past that
 * merges them all into their bounds (correct, only more pixels). The
 * boxes live in the owner's array (a surface keeps 32, the output 64). */
struct comp_damage {
    uint32_t n, max;
    struct comp_box *b;      /* max of them, the owner's */
};

void damage_init(struct comp_damage *d, struct comp_box *boxes, uint32_t max);
void damage_clear(struct comp_damage *d);
void damage_add(struct comp_damage *d, struct comp_box box);   /* empty boxes ignored */
bool damage_empty(const struct comp_damage *d);

/* ---- clients and their objects ---------------------------------------------------- */

struct comp_surface;
struct comp_window;

/* A wl_shm_pool: one VMO of the client's (VMO_KEEP_PAGES), mapped by us
 * read-only with every page present (VMAR_KEPT_ONLY), so a read of it
 * never faults whatever the client does. Kept alive by its buffers after
 * wl_shm_pool.destroy, as the protocol says. */
struct comp_pool {
    struct comp_client *client;
    struct comp_pool   *next;      /* the client's pools */
    uint32_t id;                   /* its wl_shm_pool; 0 once destroyed */
    uint32_t refs;                 /* the protocol object and each buffer */
    handle_t vmo;                  /* kept: resize maps it again */
    uint64_t addr;                 /* our mapping of it */
    uint64_t mapped;               /* bytes mapped (the size, rounded up to pages) */
    uint32_t size;                 /* the pool's size, as the client said */
};

/* A wl_buffer: pixels in a pool, checked to lie inside it when made (pools
 * only grow, so they stay inside). */
struct comp_buffer {
    struct comp_client *client;
    struct comp_buffer *next;      /* the client's buffers */
    struct comp_pool   *pool;      /* a reference */
    uint32_t id;                   /* its wl_buffer; 0 once destroyed */
    uint32_t refs;                 /* the protocol object, and each surface state holding it */
    uint32_t busy;                 /* surfaces showing it: release is owed when this drops to 0 */
    uint32_t offset;               /* bytes into the pool */
    int32_t  width, height;        /* pixels, 1 to COMP_BUFFER_SIDE_MAX */
    uint32_t stride;               /* bytes a row, a multiple of 4, >= width * 4 */
    uint32_t format;               /* JWL_WL_SHM_FORMAT_ARGB8888 or _XRGB8888 */
};

/* The first byte of buffer b's pixels, through our mapping of its pool. */
static inline const uint8_t *comp_buffer_data(const struct comp_buffer *b)
{
    return (const uint8_t *)(uintptr_t)(b->pool->addr + b->offset);
}

/* A wl_region. */
struct comp_regobj {
    struct comp_client *client;
    struct comp_regobj *next;      /* the client's regions */
    uint32_t id;
    struct comp_region region;
};

/* A bound wl_output (the events it is owed: geometry, enter/leave). */
struct comp_outres {
    struct comp_client *client;
    struct comp_outres *next;      /* the client's outputs */
    uint32_t id;
};

/* Why a client went, for the log and the stats. */
enum comp_gone {
    COMP_GONE_CLOSED,              /* it closed its end, or its process ended */
    COMP_GONE_PROTOCOL,            /* it broke the protocol: wl_display.error sent */
    COMP_GONE_SLOW,                /* it didn't read: 64 KiB held (no_memory) */
    COMP_GONE_COUNT
};

struct comp_client {
    unsigned slot;                 /* in conn.c's table; the port key is 1 + slot */
    struct jwl_conn *conn;         /* NULL once dead */
    handle_t raw;                  /* our end of its channel (a duplicate): the port watches it */
    bool ready;                    /* its channel may have messages (a port packet came) */
    bool more;                     /* its budget ran out with messages left */
    bool lingering;                /* dead by our hand: waiting for it to close its end */
    uint32_t nsurfaces, npools, nbuffers, nregions, noutputs;
    uint64_t pool_bytes;           /* bytes of its pools mapped, in total */
    uint32_t rects;                /* boxes in all its regions (struct comp_region.charge) */
    struct comp_surface *surfaces;
    struct comp_pool    *pools;
    struct comp_buffer  *buffers;
    struct comp_regobj  *regions;
    struct comp_outres  *outputs;
    void *xdg;                     /* xdg-shell's per-client state (xdg.c), or NULL */
    void *seat;                    /* the seat's per-client state (seat.c), or NULL */
};

/* ---- surfaces --------------------------------------------------------------------- */

/* A role: what a surface is for (wl_surface's "role" in the protocol). One
 * per surface, for good once given. */
enum comp_role {
    COMP_ROLE_NONE,
    COMP_ROLE_XDG_TOPLEVEL,
    COMP_ROLE_XDG_POPUP,
    COMP_ROLE_CURSOR,
    COMP_ROLE_TEST,                /* the `testwin` test power's windows (testwin.c) */
};

struct comp_role_ops {
    const char *name;              /* for errors: "xdg_toplevel" */
    /* After commit applied the state (the new buffer, damage, regions):
     * map, configure, place. OK, or a protocol error already posted. */
    status_t (*commit)(struct comp_surface *s);
    /* The surface is going (wl_surface.destroy, or its client went: then
     * dead is true and nothing may be sent). The role drops its window and
     * whatever it keeps for the surface. */
    void (*gone)(struct comp_surface *s, bool dead);
};

/* What requests change and commit applies (wl_surface's double-buffered
 * state). Damage in buffer coordinates (damage_buffer) is the same as in
 * surface coordinates here: the scale is always 1 and the transform
 * normal, so both go into one list. */
struct comp_state {
    bool attached;                 /* attach came: buffer replaces the current one */
    struct comp_buffer *buffer;    /* a reference, or NULL (a detach) */
    int32_t dx, dy;                /* attach's x and y: the new buffer's offset */
    struct comp_damage damage;     /* surface coordinates; boxes in damage_boxes */
    struct comp_box damage_boxes[COMP_SURFACE_DAMAGE_MAX];
    bool opaque_set, input_set;    /* set_*_region came */
    bool input_all;                /* set_input_region(NULL): the whole surface */
    struct comp_region opaque, input;
};

struct comp_surface {
    struct comp_client  *client;
    struct comp_surface *next;     /* the client's surfaces */
    uint32_t id;                   /* its wl_surface */
    struct comp_state pending;
    /* current: what the last commit made */
    struct comp_buffer *buffer;    /* shown (busy), a reference; NULL: nothing to show */
    int32_t width, height;         /* the surface's size: its buffer's (scale 1); 0 without */
    int32_t dx, dy;                /* the last commit's attach offset, for the role to use */
    struct comp_region opaque;     /* surface coordinates */
    struct comp_region input;      /* surface coordinates, when not input_all */
    bool input_all;                /* the whole surface takes input (the default) */
    /* Frame callback ids, oldest first: nframes committed ones (answered
     * after the next paint that shows the surface), then npending asked
     * for since the last commit. */
    uint32_t frames[COMP_FRAMES_MAX];
    uint32_t nframes, npending;
    uint64_t hidden_done_ns;       /* when callbacks were last answered while hidden */
    uint64_t commits;
    enum comp_role role;
    const struct comp_role_ops *role_ops;   /* NULL without a role */
    void *role_data;               /* the role's own (xdg_surface, cursor) */
    struct comp_window *window;    /* NULL unless the role placed it */
};

/* ---- windows and the output (scene.c) ---------------------------------------------- */

enum comp_layout {
    COMP_FLOATING,                 /* windows overlap, moved by the user */
    COMP_TILING,                   /* the window manager splits the screen */
};

#define COMP_WIN_MAPPED     (1u << 0)   /* shown */
#define COMP_WIN_MAXIMIZED  (1u << 1)
#define COMP_WIN_FULLSCREEN (1u << 2)
#define COMP_WIN_FOCUSED    (1u << 3)   /* has the keyboard focus (focus.c sets it) */
#define COMP_WIN_UNRESPONSIVE (1u << 4) /* didn't answer a ping: its title bar says so */
#define COMP_WIN_ANIMATED   (1u << 5)   /* mapped, but drawn elsewhere: by an animation (anim.c)
                                         * or as a Super+dragged tile's picture (wmgrab.c):
                                         * painting skips it and nothing below it is hidden */
#define COMP_WIN_OVERLAY    (1u << 6)   /* a boot overlay (screens.c: a full-screen window whose
                                         * client takes no keys, the splash's): over every other
                                         * window, whatever is raised or made after it */

/* A floating window's title bar: deco_top, in pixels (title.c draws it,
 * the window manager sets it; look.h has the rest of its look). */
#define COMP_TITLE_H 28

struct comp_window {
    struct comp_surface *surface;
    struct comp_window  *below, *above;   /* stacking order: scene.bottom .. scene.top */
    int32_t x, y;                  /* where the surface's (0, 0) is on the output */
    /* Decorations around the surface (deco.c sets them): the frame is the
     * surface's box grown by these. */
    int32_t deco_top, deco_left, deco_right, deco_bottom;
    uint32_t flags;                /* COMP_WIN_* */
    struct comp_box tile;          /* tiling: the tile it was given (wm.c) */
    /* The size its surface is shown at (0: its buffer's): a tile's that the
     * client was asked to fill (wm.c), so while the client catches up with
     * a new size its last buffer stays, clipped to it, and what it doesn't
     * cover is its buffer's bottom-right pixel's colour (paint.c), never
     * the wallpaper; the frame is the tile's from the first frame. */
    int32_t view_w, view_h;
    void *wm;                      /* the window manager's own (struct wm_window, wm.h) */
    const char *title;             /* its title bar's text (the window manager's), or NULL */
    /* Where it is shown, from (x, y) (anim.c): a screen slide's sideways
     * offset, or a tile's glide to its new place; the window is painted,
     * damaged and hit there (window_shown_x/y). 0, 0 at rest. */
    int32_t slide_x, slide_y;
};

/* Where w's surface's (0, 0) is shown now: (x, y) and its offsets. */
static inline int32_t window_shown_x(const struct comp_window *w)
{
    return w->x + w->slide_x;
}
static inline int32_t window_shown_y(const struct comp_window *w)
{
    return w->y + w->slide_y;
}

struct comp_scene {
    int32_t   width, height;       /* the output, in pixels */
    uint32_t *pixels;              /* headless: the image, 0x00RRGGBB (output.c); else NULL */
    uint32_t  stride;              /* its pixels a row */
    uint32_t  background;          /* 0x00RRGGBB where no window is */
    struct comp_window *bottom, *top;
    uint32_t  nwindows;
    struct comp_damage damage;     /* output coordinates: to compose at the next paint */
    struct comp_box damage_boxes[COMP_OUTPUT_DAMAGE_MAX];
    /* The part of that damage below the desktop's cards (all but the
     * cursor's and the cards' own): what makes a card's blurred backdrop
     * stale (frost.c). */
    struct comp_damage under;
    struct comp_box under_boxes[COMP_OUTPUT_DAMAGE_MAX];
    enum comp_layout layout;
    uint64_t  paints;              /* paints done */
    uint64_t  last_paint_ns;       /* when the last one began (clock.c) */
};

extern struct comp_scene scene;

/* The scene for an output of w by h with the given background, all of it
 * damaged; no windows. */
void scene_init(int32_t w, int32_t h, uint32_t background);
/* b (output coordinates, clipped to the output) to compose again. */
void scene_damage(struct comp_box b);
/* The same for something drawn over everything (the cursor, the desktop's
 * cards): it isn't `under` damage. */
void scene_damage_over(struct comp_box b);
/* The window's frame on the output: the surface's box and its decorations. */
struct comp_box window_frame(const struct comp_window *w);
/* All a window paints: its frame and its shadow (look.h), the larger
 * shadow's whether it is focused or not, so a focus change repaints every
 * pixel either shadow touches. What window_damage damages. */
struct comp_box window_extent(const struct comp_window *w);
/* The surface's box on the output. */
struct comp_box window_surface_box(const struct comp_window *w);
/* A window for s (which must have none), at (x, y), on top, not mapped
 * yet: ERR_NO_MEMORY. */
status_t window_create(struct comp_surface *s, int32_t x, int32_t y, struct comp_window **out);
/* Out of the stacking order, its frame damaged, freed; s->window cleared. */
void window_destroy(struct comp_window *w);
void window_map(struct comp_window *w, bool mapped);   /* damages its frame */
void window_move(struct comp_window *w, int32_t x, int32_t y);   /* damages old and new */
/* w shown view_w x view_h (0, 0: its buffer's size); damages old and new. */
void window_view(struct comp_window *w, int32_t view_w, int32_t view_h);
/* To the top (under the boot overlays, COMP_WIN_OVERLAY, unless w is one);
 * damages its frame. window_create puts a new window there too. */
void window_raise(struct comp_window *w);
void window_damage(struct comp_window *w);             /* all of its extent */
/* Surface damage b (surface coordinates, clipped to the surface) on the output. */
void window_damage_surface(struct comp_window *w, struct comp_box b);
/* The topmost mapped window whose surface takes input at output (x, y)
 * (its input region), or NULL. Decorations are the window manager's. */
struct comp_window *window_at(int32_t x, int32_t y);
/* Can anything of s be seen: a mapped window inside the output. */
bool surface_visible(const struct comp_surface *s);

/* ---- the compositor as a whole --------------------------------------------------- */

struct comp_stats {
    uint64_t clients;                       /* connections taken, in total */
    uint64_t gone[COMP_GONE_COUNT];         /* clients gone, by why */
    uint64_t refused;                       /* connections refused (COMP_CLIENTS_MAX) */
    uint64_t paints, painted_px;            /* paints and pixels composed */
    uint64_t last_paint_ns, worst_paint_ns; /* how long they took */
};

struct comp {
    handle_t port;                 /* everything the loop waits on */
    handle_t vmar;                 /* our address space: pools are mapped into it */
    handle_t svc;                  /* /svc/wayland's server end (init keeps it) */
    bool headless;                 /* no framebuffer: compose into memory */
    bool blanked;                  /* compctl.blank: the background only, no window drawn */
    uint64_t splash_until;         /* the boot's wait for the splash (`splash`, paint.c): the
                                    * background only until a boot overlay maps or until then
                                    * (uptime ns); 0: not waiting */
    bool testwin;                  /* the test power `testwin` (testwin.c): never set by init */
    uint32_t serial;               /* the last event serial handed out */
    uint64_t period_ns;            /* the paint clock's period */
    struct comp_stats stats;
};

extern struct comp comp;

/* The next event serial (never 0). */
uint32_t comp_serial(void);
/* Milliseconds of uptime, as wl_callback.done and input events carry. */
static inline uint32_t comp_ms(uint64_t ns)
{
    return (uint32_t)(ns / NS_PER_MS);
}

/* ---- conn.c ---------------------------------------------------------------------- */

/* Port keys: the svc channel, then 1 + a client's slot. Keys from
 * COMP_KEY_OTHER up are free for other tracks' channels (input sources,
 * compctl). */
#define COMP_KEY_SVC   0u
#define COMP_KEY_OTHER 0x10000u

/* svc.connect: a new connection (svc_serve_request's connect). */
status_t conn_connect(void *ctx, handle_t *out);
/* A port packet with key 1 + slot came: the client may have something. */
void     conn_ready(uint64_t key);
/* One turn for every client with work: its messages (a budget each), then
 * its events flushed; dead ones torn down. */
void     conn_serve_all(void);
/* Flush every live client's events (after a paint answered callbacks). */
void     conn_flush_all(void);
/* Does any client still have messages its budget left (don't sleep), or
 * events a full channel held back (try again soon)? */
bool     conn_more(void);
bool     conn_held(void);
/* Every live client, for code that walks them (frame callbacks). */
struct comp_client *conn_client_at(unsigned slot);

/* A protocol error on cl: wl_display.error naming object, code (the
 * interface's own error enum, or JWL_ERROR_*), the text; the connection
 * is dead from now on. Returns ERR_INVALID_ARGS, for a handler to return. */
status_t comp_error(struct comp_client *cl, uint32_t object, uint32_t code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
/* The compositor itself couldn't (no memory): wl_display.error no_memory. */
status_t comp_no_memory(struct comp_client *cl, uint32_t object, const char *what);
/* The data of cl's live object id if it is of iface, else NULL. */
void    *comp_object(struct comp_client *cl, uint32_t id, const struct jwl_interface *iface);

/* ---- each interface's requests (the dispatch table in conn.c) ------------------------ */

status_t display_request(struct comp_client *cl, struct jwl_msg *m);
status_t registry_request(struct comp_client *cl, struct jwl_msg *m);
status_t compositor_request(struct comp_client *cl, struct jwl_msg *m);
status_t region_request(struct comp_client *cl, struct jwl_msg *m);
status_t output_request(struct comp_client *cl, struct jwl_msg *m);
status_t surface_request(struct comp_client *cl, struct jwl_msg *m);
status_t shm_request(struct comp_client *cl, struct jwl_msg *m);
status_t pool_request(struct comp_client *cl, struct jwl_msg *m);
status_t buffer_request(struct comp_client *cl, struct jwl_msg *m);

/* ---- what each module does when a client goes ------------------------------------ */

/* Everything of cl's, with nothing sent (it is dead): surfaces (their
 * windows and roles), buffers, pools (unmapped), regions, outputs. */
void surfaces_teardown(struct comp_client *cl);
void shm_teardown(struct comp_client *cl);
void display_teardown(struct comp_client *cl);

/* ---- surfaces and buffers (surface.c, shm.c) ----------------------------------------- */

/* wl_compositor.create_surface on compositor: a surface for cl at the new
 * id. OK, or a protocol error posted (the cap, no memory). */
status_t surface_create(struct comp_client *cl, uint32_t compositor, uint32_t id);
/* s gets role, with ops and data. ERR_BAD_STATE: it has a role object
 * now, or had another role (the caller posts the role's error). */
status_t surface_set_role(struct comp_surface *s, enum comp_role role,
                          const struct comp_role_ops *ops, void *data);
/* The role object went (xdg_toplevel.destroy): the surface keeps its role,
 * which may be given again, and loses ops, data and window (unmapped). */
void     surface_drop_role(struct comp_surface *s);

/* wl_registry.bind of wl_shm: the formats sent. */
status_t shm_bind(struct comp_client *cl, uint32_t id, uint32_t version);
/* A buffer's references: its wl_buffer, and each surface state holding it;
 * freed (its pool's reference dropped) at the last. */
void     buffer_ref(struct comp_buffer *b);
void     buffer_unref(struct comp_buffer *b);
/* Shown by one more surface, or one fewer: when none shows it any more and
 * its wl_buffer is still there, wl_buffer.release goes to the client. */
void     buffer_show(struct comp_buffer *b);
void     buffer_unshow(struct comp_buffer *b);

/* After a paint at t (painted), or only on the clock (not painted):
 * frame callbacks of the visible surfaces the paint showed are answered,
 * and those of hidden surfaces at most every COMP_HIDDEN_FRAME_NS. */
void     surfaces_frame_done(uint64_t t, bool painted);
/* When the next hidden surface's callbacks are due (DEADLINE_NEVER: none),
 * and whether a visible one waits for a paint. */
uint64_t surfaces_hidden_deadline(void);
bool     surfaces_waiting_paint(void);

/* ---- painting and the output (output.c, paint.c, title.c, cursor.c, clock.c) ---------- */

/* The output: where composed pixels go. */
struct comp_output {
    uint32_t *px;                  /* pixel (x, y) is px[y * stride + x]: the framebuffer
                                    * (write-combining: written, never read) or the image */
    uint32_t  stride;              /* pixels from one row to the next */
    int32_t   width, height;
    uint8_t   rs, gs, bs;          /* the red, green and blue channels' bit positions */
    bool      native;              /* the format is 0x00RRGGBB: copied as it is */
    bool      screen;              /* the framebuffer (false: headless) */
    handle_t  owner;               /* framebuffer_take's owner token, held for good */
};
extern struct comp_output output;

/* The framebuffer (SR_RESOURCE with RIGHT_ROOT_SCREEN), or with headless,
 * no root or no framebuffer (ERR_NOT_FOUND) an image of w by h in memory
 * (SR_USER + 1 if the starter gave one); scene_init for its size comes
 * after. ERR_BAD_STATE: someone else owns the screen; others: no memory, a
 * framebuffer that isn't 32 bits a pixel. */
status_t output_open(bool headless, int32_t w, int32_t h);

/* The workers (threads in all, the loop's thread included; 0: the
 * default, a few). Call once, after output_open. */
status_t paint_init(uint32_t threads);
/* Compose the scene's damage onto the output, then clear it: what the
 * clock calls. The output pixels written. */
uint64_t paint_frame(void);
/* The boot's wait for the splash (init's `splash` argument): from now on
 * the screen is only the splash's background (LOOK_BLANK: no wallpaper,
 * strip, window or cursor) until a boot overlay (COMP_WIN_OVERLAY) is
 * mapped with a buffer, or for 5 s at most if none is. */
void     paint_splash_wait(void);
/* The wait over if a boot overlay is up or t is past its end: then all of
 * the output is damaged, to be painted as it is. paint_frame checks too;
 * the loop calls it for the end, which nothing else wakes it for. */
void     paint_splash_check(uint64_t t);
/* When the wait ends at the latest (DEADLINE_NEVER: no wait). */
uint64_t paint_splash_deadline(void);
/* Is w's surface hidden behind one opaque window above it (or not shown
 * at all)? Its frame callbacks then come as a hidden surface's. */
bool     window_covered(const struct comp_window *w);

/* A title bar's box on the output, and its circles' (empty: none), as
 * title.c draws them: for the window manager's clicks. A press is on a
 * circle anywhere in its title_button_hit box, a little larger than the
 * circle (look.h); title_button_at says which (TITLE_NONE: none). */
enum title_button { TITLE_CLOSE, TITLE_MINIMISE, TITLE_FULLSCREEN, TITLE_BUTTONS,
                    TITLE_NONE = TITLE_BUTTONS };
struct comp_box title_bar_box(const struct comp_window *w);
struct comp_box title_button_box(const struct comp_window *w, enum title_button b);
struct comp_box title_button_hit(const struct comp_window *w, enum title_button b);
enum title_button title_button_at(const struct comp_window *w, int32_t x, int32_t y);
/* The close circle's box (title_button_box's). */
struct comp_box title_close_box(const struct comp_window *w);

/* Paints at most hz times a second (1 to 240; display.hz, 60 by default). */
void     clock_set_hz(uint32_t hz);
/* Paint if there is damage (or a visible surface waits) and the clock
 * allows; answer the frame callbacks that are due. One loop turn's. */
void     clock_turn(void);
/* When the loop must wake for the clock (DEADLINE_NEVER: nothing to do). */
uint64_t clock_deadline(void);

/* ---- the seat: input, focus and compctl (seat.c, keyboard.c, pointer.c, focus.c,
 * sources.c, ctl.c, testwin.c) ----------------------------------------------------------
 *
 * Input sources (HID drivers, serialin) connect through compctl and speak
 * the `input` protocol; the loop serves them first in every turn, so
 * typing never waits behind a client. Keys go to the window with the
 * keyboard focus only; the pointer to the window under it, or, while a
 * button is held, to the one the press started in (the implicit grab).
 * The keys a client never sees (Ctrl+Alt+Del, Alt+Tab, the window keys)
 * are taken before any focus is looked at. */

/* Port keys COMP_KEY_SEAT .. COMP_KEY_SEAT + 0xffff are the seat's: compctl
 * channels, input sources, init's answer to a reboot. */
#define COMP_KEY_SEAT     0x20000u
#define COMP_KEY_SEAT_END 0x30000u

/* At start, after the port exists: the keymap, compctl's channel (SR_USER
 * + 2, optional), the pointer in the middle of the output. */
status_t seat_init(void);
/* A port packet with a key of the seat's came. */
void     seat_packet(uint64_t key);
/* Each turn, first: every input source and compctl channel with work, a
 * budget each. */
void     seat_serve(void);
/* Each turn, after the clients: the pointer's focus checked against the
 * scene (a window that appeared, moved or went under it), motion a busy
 * client was owed. */
void     seat_turn(void);
/* When the loop must turn again for the seat: 0 if a source's budget left
 * work, DEADLINE_NEVER if nothing waits. */
uint64_t seat_deadline(void);

/* The globals' wl_seat, and its objects' requests (the dispatch table). */
status_t seat_bind(struct comp_client *cl, uint32_t id, uint32_t version);
status_t seat_request(struct comp_client *cl, struct jwl_msg *m);
status_t keyboard_request(struct comp_client *cl, struct jwl_msg *m);
status_t pointer_request(struct comp_client *cl, struct jwl_msg *m);
/* wp-cursor-shape-v1 (shapes.c): its global, and its two objects' requests. */
#define COMP_CURSOR_SHAPE_VERSION 2u
status_t shapes_bind(struct comp_client *cl, uint32_t id, uint32_t version);
status_t shapes_request(struct comp_client *cl, struct jwl_msg *m);
status_t shape_device_request(struct comp_client *cl, struct jwl_msg *m);
/* cl went: its seat objects, and every focus, grab and serial it had. */
void     seat_teardown(struct comp_client *cl);

/* scene.c calls these: w was mapped (a client's first window takes the
 * keyboard focus), or is unmapped or going (whatever focus or grab it had
 * moves on: the keyboard to the next window down). */
void     seat_window_mapped(struct comp_window *w);
void     seat_window_gone(struct comp_window *w);

/* For the window manager (C3: wm.c, xdg.c). */
/* The keyboard focus to w (NULL: none): leave and enter sent. */
void     seat_focus(struct comp_window *w);
/* The window with the keyboard focus, or NULL. */
struct comp_window *seat_focused(void);
/* Does cl take keys: it has a wl_keyboard (libjwl's no_keyboard binds none:
 * the boot splash's). A client without one never gets the keyboard focus. */
bool     seat_takes_keys(struct comp_client *cl);
/* Is serial that of a button press sent to cl whose button is still held?
 * (xdg_toplevel.move and resize are honoured only then.) */
bool     seat_button_serial_ok(const struct comp_client *cl, uint32_t serial);
/* A grab of the compositor's own (a move or resize): until the last
 * button is released, motion goes to ops->motion with the pointer's
 * position on the output and no client sees the pointer (the one that had
 * it gets leave); then ops->end. ERR_BAD_STATE if no button is held or a
 * grab is on already. */
struct comp_grab_ops {
    void (*motion)(void *data, int32_t x, int32_t y);
    void (*end)(void *data);
};
status_t seat_grab_begin(const struct comp_grab_ops *ops, void *data);
/* The grab ends now (its window went): ops->end is not called. */
void     seat_grab_cancel(void);

/* The compositor's cursor set (cursors.c; docs/design/cursors.svg). */
enum cursor_shape {
    CURSOR_ARROW,
    CURSOR_RESIZE_EW,              /* left and right */
    CURSOR_RESIZE_NS,              /* up and down */
    CURSOR_RESIZE_NWSE,            /* top left and bottom right */
    CURSOR_RESIZE_NESW,            /* top right and bottom left */
    CURSOR_MOVE,
    CURSOR_TEXT,
    CURSOR_HAND,
    CURSOR_BUSY,                   /* a ring turning: an app is starting */
    CURSOR_SHAPES
};

/* The pointer, for painting the cursor (C2: cursor.c): where it is, and
 * what to draw there. surface is the cursor surface of the client under
 * the pointer (wl_pointer.set_cursor), or NULL for one of the set, shape;
 * hidden: that client asked for no cursor at all. */
struct comp_cursor {
    int32_t x, y;                  /* the pointer's tip, output pixels */
    struct comp_surface *surface;  /* COMP_ROLE_CURSOR, or NULL: shape */
    int32_t hot_x, hot_y;          /* the surface's point drawn at (x, y) */
    bool hidden;
    enum cursor_shape shape;       /* without a surface: the client's (wp_cursor_shape) or
                                    * the compositor's choice (pointer.c) */
    bool moved;                    /* a mouse has moved it: until then it hovers nothing (it
                                    * starts in the output's middle, often on a gap) */
};

/* The compositor's own cursor at (x, y) where no client's surface is: a
 * frame's resize arrows on a resizable floating window's edges and
 * corners (deco.c's grab zones), the hand on its circles, the arrow
 * elsewhere (wm_cursor_at); the hand on the top bar's islands, menu rows
 * and buttons (desk_cursor_at: CURSOR_SHAPES where the desktop has
 * nothing there). */
enum cursor_shape wm_cursor_at(int32_t x, int32_t y);
enum cursor_shape desk_cursor_at(int32_t x, int32_t y);
/* The shape the pointer shows over nothing of a client's: the desktop's,
 * else the window manager's, busy for the arrow while an app starts. */
enum cursor_shape cursor_shape_at(int32_t x, int32_t y);
/* What the cursor shows may have changed without the pointer moving (busy
 * began or ended): the seat chooses again. */
void seat_cursor_changed(void);
extern struct comp_cursor cursor;

/* Hooks the seat calls; the painting and window-manager tracks define
 * them (until they do, focus.c's weak defaults stand in). */
/* The pointer moved from (old_x, old_y), or the cursor's picture changed:
 * damage what the cursor covered and covers. Default: nothing. */
void cursor_moved(int32_t old_x, int32_t old_y);
/* Alt+Tab (Alt+Shift+Tab: backward): the window to focus after from (NULL:
 * none focused), or NULL for none. Default: the next mapped window down
 * the stacking order, wrapping. */
struct comp_window *wm_cycle(struct comp_window *from, bool backward);
/* A click (a button press) focused w: raise it, as the arrangement wants.
 * Default: window_raise. */
void wm_clicked(struct comp_window *w);
/* A button press at output (x, y), with the modifiers held (INPUT_MOD_*:
 * every source's), that the window manager may take for itself (a title
 * bar, its circles, a frame's edge, a gap between tiles, anything with
 * Super held): true if it did; the press and its release then reach no
 * client. Default: false. */
bool wm_press(int32_t x, int32_t y, uint32_t button, uint8_t mods);
/* Is (x, y) on a gap between tiles a drag resizes (its hit area reaches a
 * little into the tiles' borders): no client is under the pointer there.
 * Default: false. */
bool wm_gap_covers(int32_t x, int32_t y);
/* The window keys (wmkeys.c, the owner's table): a key press with the
 * source's modifier byte, true if taken (acted on, and neither it nor its
 * release reaches a client). Default: false. */
bool wm_key(uint16_t usage, uint8_t mods);
/* Super+F (and the full-screen circle, a double-click on the title bar):
 * w full screen, or back. Default: nothing. */
void wm_toggle_fullscreen(struct comp_window *w);
/* The layout key (Super+T): the screen floating, or tiling, or back.
 * Default: nothing. */
void wm_toggle_layout(void);
/* Super+Q: ask w's client to close it (xdg_toplevel.close), as its close
 * circle does; tiling's windows have none. Default: nothing. */
void wm_close(struct comp_window *w);
/* The minimise circle on w: it shrinks into its chip on the top bar and
 * is hidden until it is brought back (its chip, Alt+Tab). Default: nothing. */
void wm_minimise(struct comp_window *w);
/* The keyboard focus moved to w (NULL: none), COMP_WIN_FOCUSED already set
 * (seat_focus calls it last, whatever moved it): the window manager raises
 * w and tells both toplevels whether they are activated. Default: nothing. */
void wm_focus_changed(struct comp_window *w);

/* The test power `testwin` (testwin.c): a surface without a role that
 * commits a buffer becomes a window at the attach's x and y, mapped while
 * it has a buffer. For tests that need windows without xdg-shell; init
 * never passes it. surface.c calls this at commit. */
status_t testwin_commit(struct comp_surface *s);

/* ---- window management (wm.c, wmtile.c, wmgrab.c, deco.c) -------------------------------
 *
 * Every toplevel is a window the window manager places: floating (new
 * windows centred, the next one cascaded; moved by the title bar or
 * Super+drag, resized by the edges or Super+right-drag when the client
 * allows) or tiling (dwindle, wmtile.c: each screen's room split in a
 * binary tree, a new window halving the focused tile along its longer
 * side; the gaps between tiles dragged to resize them; a window that
 * can't resize sits centred in its tile at its own size; tiles have a gap
 * of background between them and at the edges). scene.layout says which
 * for the current screen; Super+T switches it. The keys are wmkeys.c's.
 * The seat's hooks above (wm_cycle, wm_clicked, wm_press, wm_key,
 * wm_toggle_*, wm_close, wm_focus_changed) are defined in wm.c, wmgrab.c
 * and wmkeys.c. A window without a toplevel (testwin's: w->wm is NULL) is
 * the seat's alone: no decorations, never placed, cycled after the
 * toplevels. */

/* Decorations (deco.c sets struct comp_window's deco_*; title.c draws
 * them, look.h says how they look). Floating: the title bar, COMP_TITLE_H
 * high, above the surface (its three circles at the left end) and a
 * DECO_OUTLINE outline on the other sides; maximised, the title bar only.
 * Tiling: no title bar, a DECO_BORDER border on all four sides, its colour
 * the focus. Full screen: none. The window's title is w->title (the window manager's
 * copy, at most 255 bytes; it outlives the window), and
 * COMP_WIN_UNRESPONSIVE says the client didn't answer the ping a close
 * sent. A resizable floating window's edges also take presses DECO_GRAB
 * pixels outside its frame: a border thin enough to look right is too
 * thin to hit. */
#define DECO_OUTLINE 1
#define DECO_BORDER  2
#define DECO_GRAB    6

/* The topmost mapped window whose frame (and grab margin) holds output (x,
 * y), or NULL (the background). *on_surface: the point is in its surface's
 * input region (the client's); else it is on the decorations (the window
 * manager's). A point in a surface but outside its input region looks at
 * the windows below. (window_at sees surfaces only: a title bar over
 * another window's surface hides it here, not there.) */
struct comp_window *wm_window_at(int32_t x, int32_t y, bool *on_surface);

/* The compositor's grabs for a client's xdg_toplevel.move and resize, once
 * seat_button_serial_ok said yes, from (x, y), the pointer now (edges:
 * xdg_toplevel.resize_edge's bits). Started with seat_grab_begin.
 * ERR_BAD_STATE: w can't be moved or resized now (tiling, maximised,
 * no button held, a grab on already); ERR_INVALID_ARGS: edges. */
status_t wm_begin_move(struct comp_window *w, int32_t x, int32_t y);
status_t wm_begin_resize(struct comp_window *w, uint32_t edges, int32_t x, int32_t y);
/* Every window placed again for layout (the start: init's saved choice). */
void     wm_set_layout(enum comp_layout layout);

/* The layout's setting in /data/etc/settings (<settings.h>): `display.layout
 * = floating` or `tiling`. The compositor reads no files: init reads it and
 * hands it over (the `layout=` argument), and saves it when told the user
 * switched (ctl_layout_changed). */
#define WM_LAYOUT_SETTING "display.layout"
const char *wm_layout_name(enum comp_layout layout);
bool        wm_layout_parse(const char *s, enum comp_layout *out);
/* The user switched the layout: compctl tells init, which saves it
 * (ctl.c: the answer to init's layout_wait). */
void        ctl_layout_changed(enum comp_layout layout);

/* ---- the desktop (desk.h has its inside) --------------------------------------------------
 *
 * The top bar, virtual screens, minimising, Alt+Tab's list, the search
 * box, the popovers, notifications and the animations (docs/G1-PLAN.md
 * "The look"). The seat offers it every key press and button press first,
 * the loop its clock, and painting draws it over the windows. */

/* At start: on (the strip and the cards) or not (the window manager
 * alone: the `nodesk` argument, the test scene), animations or not. */
void     desk_init(bool on, bool animate);
bool     desk_on(void);
/* A key press (HID usage, the source's modifier byte, the xkb modifiers
 * with the locks): true if the desktop took it (no client sees it, nor its
 * release). A key's release, seen before any client sees it. */
bool     desk_key(uint16_t usage, uint8_t mods, uint32_t xkb_mods);
void     desk_key_up(uint16_t usage);
/* A button press at (x, y), before the window manager's: true if taken. */
bool     desk_press(int32_t x, int32_t y, uint32_t button);
/* Is the strip or a card at (x, y), so no window is under the pointer there? */
bool     desk_covers(int32_t x, int32_t y);
/* An app the desktop launched hasn't shown a window yet (at most
 * DESK_BUSY_NS): the arrow is the busy ring. */
bool     desk_busy(void);
/* The loop's clock: animations, the clock on the strip, notifications
 * fading, Alt+Tab's list showing. When it next needs a turn. */
void     desk_tick(uint64_t t);
uint64_t desk_deadline(void);

/* Hooks for the plumbing (track D2b defines them; desk.c's weak defaults
 * do what a desktop with nothing behind it can). */
/* Run app (a desk_apps name in lower case: "jamjar"), or cmd in a new terminal. */
void     ctl_launch(const char *app);
void     ctl_run_in_terminal(const char *cmd);
/* Button `button` (0 the first) of notification id was pressed (the card
 * goes). */
void     ctl_notify_answered(uint32_t id, uint32_t button);
/* The mixer's volume (0..100): read (false: unknown), and set. */
bool     ctl_volume(uint32_t *percent);
void     ctl_set_volume(uint32_t percent);
/* The output's name, and what is playing (false: nothing). */
bool     ctl_audio_output(char *buf, size_t n);
bool     ctl_now_playing(char *buf, size_t n);
/* The network's state (false: unknown). */
struct desk_net;
bool     ctl_network(struct desk_net *out);

/* ---- xdg-shell (xdg.c, xdgtop.c) -------------------------------------------------- */

/* wl_registry.bind of xdg_wm_base. */
status_t xdg_bind(struct comp_client *cl, uint32_t id, uint32_t version);
status_t xdg_wm_base_request(struct comp_client *cl, struct jwl_msg *m);
status_t xdg_positioner_request(struct comp_client *cl, struct jwl_msg *m);
status_t xdg_surface_request(struct comp_client *cl, struct jwl_msg *m);
status_t xdg_toplevel_request(struct comp_client *cl, struct jwl_msg *m);
status_t xdg_popup_request(struct comp_client *cl, struct jwl_msg *m);
/* Everything of cl's xdg-shell objects, nothing sent (before surfaces_teardown). */
void     xdg_teardown(struct comp_client *cl);
/* The ping clock: pings unanswered for XDG_PING_NS mark their client's
 * windows not responding. xdg_deadline: when xdg_tick is next due
 * (DEADLINE_NEVER: nothing waits). */
#define XDG_PING_NS (5 * NS_PER_S)
void     xdg_tick(uint64_t t);
uint64_t xdg_deadline(void);
