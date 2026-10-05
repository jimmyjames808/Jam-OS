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
 *   headless.c  composing into memory (no framebuffer: tests, screenshots).
 * Later tracks: painting on the framebuffer (output.c, paint.c, cursor.c,
 * clock.c), xdg-shell and window management (xdg.c, wm.c, deco.c), the
 * seat and compctl (seat.c, keyboard.c, pointer.c, focus.c, sources.c).
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
 * Threads: one. The loop serves the clients and paints in turn (a paint
 * may later run on workers while the loop waits for it), so nothing here
 * takes a lock: nothing changes the scene while a paint reads it.
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

struct comp_window {
    struct comp_surface *surface;
    struct comp_window  *below, *above;   /* stacking order: scene.bottom .. scene.top */
    int32_t x, y;                  /* where the surface's (0, 0) is on the output */
    /* Decorations around the surface (deco.c sets them): the frame is the
     * surface's box grown by these. */
    int32_t deco_top, deco_left, deco_right, deco_bottom;
    uint32_t flags;                /* COMP_WIN_* */
    struct comp_box tile;          /* tiling: the tile it was given (wm.c) */
    void *wm;                      /* the window manager's own */
};

struct comp_scene {
    int32_t   width, height;       /* the output, in pixels */
    uint32_t *pixels;              /* headless: the image, 0x00RRGGBB; NULL until set up */
    uint32_t  stride;              /* pixels a row */
    uint32_t  background;          /* 0x00RRGGBB where no window is */
    struct comp_window *bottom, *top;
    uint32_t  nwindows;
    struct comp_damage damage;     /* output coordinates: to compose at the next paint */
    struct comp_box damage_boxes[COMP_OUTPUT_DAMAGE_MAX];
    enum comp_layout layout;
    uint64_t  paints;              /* paints done */
    uint64_t  last_paint_ns;       /* when the last one ended */
};

extern struct comp_scene scene;

/* The scene for an output of w by h with the given background, all of it
 * damaged; no windows. */
void scene_init(int32_t w, int32_t h, uint32_t background);
/* b (output coordinates, clipped to the output) to compose again. */
void scene_damage(struct comp_box b);
/* The window's frame on the output: the surface's box and its decorations. */
struct comp_box window_frame(const struct comp_window *w);
/* The surface's box on the output. */
struct comp_box window_surface_box(const struct comp_window *w);
/* A window for s (which must have none), at (x, y), on top, not mapped
 * yet: ERR_NO_MEMORY. */
status_t window_create(struct comp_surface *s, int32_t x, int32_t y, struct comp_window **out);
/* Out of the stacking order, its frame damaged, freed; s->window cleared. */
void window_destroy(struct comp_window *w);
void window_map(struct comp_window *w, bool mapped);   /* damages its frame */
void window_move(struct comp_window *w, int32_t x, int32_t y);   /* damages old and new */
void window_raise(struct comp_window *w);              /* to the top; damages its frame */
void window_damage(struct comp_window *w);             /* all of its frame */
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

/* ---- surfaces, buffers and painting (surface.c, shm.c, headless.c) ----------------- */

/* wl_compositor.create_surface: a surface for cl at the new id. OK, or a
 * protocol error posted (the cap, no memory). */
status_t surface_create(struct comp_client *cl, uint32_t id);
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
/* Compose the scene's damage into scene.pixels (headless), and clear it. */
void     headless_compose(void);
