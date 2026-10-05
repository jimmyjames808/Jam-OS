/* utest's fake compositor for libjwl's client side (<jwl_client.h>):
 * jwlc_fake.c serves, jwlc_drive.c makes it send and holds the shared
 * test helpers. A compositor's end of the protocol, just enough of it, on
 * libjwl's own transport and the generated tables, in this process: the
 * registry with the globals at versions a test picks, sync, surfaces with
 * xdg_toplevel (a configure at the first commit, acks counted), pools
 * mapped as the real compositor maps them (VMAR_KEPT_ONLY, read-only),
 * buffers whose first pixel is read at each commit, release of the
 * buffer a commit replaces, frame callbacks answered at commit, the seat
 * with a keyboard (the real US keymap VMO) and a pointer, and calls that
 * make it send input, configures, close, ping and an error.
 *
 * Its connect function (fake_connect) is a client's config.connect: each
 * call is a new connection (a channel pair; the client gets one end),
 * which the fake takes up at its next fake_serve, dropping the one before.
 * Single-threaded tests alternate the two sides with fake_pump; a test
 * with blocking calls runs fake_thread on a thread of its own. */
#pragma once

#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <jwl_client.h>

#define FAKE_SURFACES 8u
#define FAKE_POOLS    8u
#define FAKE_BUFFERS  16u
#define FAKE_FRAMES   8u

enum { FAKE_COMPOSITOR, FAKE_SHM, FAKE_WM_BASE, FAKE_SEAT, FAKE_OUTPUT, FAKE_GLOBALS };

struct fake_pool {
    uint32_t id;             /* 0: a free slot (or destroyed, while addr is set) */
    handle_t vmo;            /* the client's, kept to map again after a resize */
    uint64_t addr, size;     /* our read-only kept mapping */
};

struct fake_buffer {
    uint32_t id;             /* 0: a free slot */
    struct fake_pool *pool;
    int32_t  offset, w, h, stride;
    uint32_t format;
};

struct fake_surface {
    uint32_t id, xdg, toplevel;   /* 0: none */
    uint32_t pending;             /* the buffer attached since the last commit */
    bool     attached;            /* an attach since the last commit */
    uint32_t current;             /* the buffer the last commit showed */
    unsigned commits, attaches, damages, acks;
    struct jwl_rect last_damage;
    uint32_t frames[FAKE_FRAMES]; /* callbacks waiting for the next commit */
    unsigned nframes;
    uint32_t sent_serial;         /* the last configure's serial; 0: none yet */
    uint32_t acked;               /* the last serial acked */
    char     title[64];
    int32_t  min_w, min_h, max_w, max_h;
    bool     fullscreen, maximized;
    uint32_t pixel;               /* the shown buffer's first pixel, read at its commit */
    uint32_t move_serial;
};

struct fake {
    struct jwl_conn *conn;        /* NULL: none */
    handle_t pending;             /* the server end of the last connect, not taken up yet */
    unsigned connects;            /* connections handed out */
    unsigned refuse;              /* connects to refuse before the next works */
    uint32_t offer[FAKE_GLOBALS]; /* the version offered; 0: not offered */
    uint32_t formats;             /* wl_shm formats offered, by bit */
    int32_t  cfg_w, cfg_h;        /* the first configure's size */
    uint32_t cfg_states;          /* ... and states (bit n: state n) */
    bool     hold_frames;         /* don't answer frame callbacks */
    int32_t  repeat_rate, repeat_delay;
    const char *keymap_text;      /* NULL: keymap_us's */
    bool     ping_at_bind;        /* ping as soon as xdg_wm_base is bound */
    bool     stop;                /* fake_thread's loop ends (atomic) */
    uint32_t serial;
    /* this connection's (the counts kept after it closes, until the next) */
    uint32_t bound[FAKE_GLOBALS]; /* the version each was bound at; 0: not */
    uint32_t keyboard, pointer, seat, wm_base;
    unsigned syncs, pongs, binds, pools_made, buffers_made;
    struct fake_surface s[FAKE_SURFACES];
    struct fake_pool p[FAKE_POOLS];
    struct fake_buffer b[FAKE_BUFFERS];
    char     unexpected[80];      /* the first request the fake doesn't know; "" none */
};

void     fake_init(struct fake *f);
void     fake_free(struct fake *f);
status_t fake_connect(void *ctx, handle_t *out);
/* Take up a pending connection, serve every request waiting, flush. */
void     fake_serve(struct fake *f);
/* The compositor dies: its end of the connection closes. */
void     fake_drop(struct fake *f);
/* Serve and dispatch by turns until neither side has anything left. */
bool     fake_pump(struct fake *f, struct jwl_client *c);
/* A config for a client of f. */
struct jwl_client_config fake_config(struct fake *f);

/* Surface i (in the order made on this connection); NULL past the last. */
struct fake_surface *fake_surface(struct fake *f, unsigned i);
status_t fake_configure(struct fake *f, struct fake_surface *s, int32_t w, int32_t h,
                        uint32_t states);
status_t fake_close(struct fake *f, struct fake_surface *s);
status_t fake_ping(struct fake *f);
status_t fake_kb_enter(struct fake *f, struct fake_surface *s);
status_t fake_key(struct fake *f, uint32_t code, bool pressed);
status_t fake_mods(struct fake *f, uint32_t depressed, uint32_t locked);
status_t fake_ptr_enter(struct fake *f, struct fake_surface *s, int32_t x, int32_t y);
status_t fake_motion(struct fake *f, int32_t x, int32_t y);
status_t fake_button(struct fake *f, uint32_t button, bool pressed);
status_t fake_wheel(struct fake *f, int32_t notches);
/* Data read through the pool mapping of buffer id: its first pixel. */
uint32_t fake_pixel(struct fake *f, uint32_t buffer);

/* Test helpers: a client of f made and pumped until ready (NULL if it
 * isn't); the next queued event of type, skipping others (false: none);
 * the job's handles and message bytes now. */
struct jwl_client *fake_ready(struct fake *f);
bool     fake_next_of(struct jwl_client *c, uint32_t type, struct jwl_event *ev);
void     fake_held(uint64_t *handles, uint64_t *bytes);
/* Check-style helpers (they print a FAILED line and return false):
 * the client and the fake destroyed, the fake saw nothing unexpected,
 * and the job holds h0 handles and b0 message bytes again; a window of
 * wc made, pumped and configured, its fake surface the slot-th one,
 * in *s; px drawn into the window's next buffer and presented with
 * damage r (NULL: all) and a frame callback, its slot in *slot. */
bool     fake_all_gone(struct fake *f, struct jwl_client *c, uint64_t h0, uint64_t b0);
bool     fake_window(struct fake *f, struct jwl_client *c, const struct jwl_window_config *wc,
                     unsigned slot, struct jwl_window **w, struct fake_surface **s);
bool     fake_draw(struct jwl_window *w, uint32_t px, const struct jwl_rect *r, unsigned *slot);
void     fake_fill(const struct jwl_frame *fr, uint32_t px);

/* A thread serving f until *stop (fake_thread_start / _stop). */
status_t fake_thread_start(struct fake *f, handle_t *thread);
void     fake_thread_stop(struct fake *f, handle_t thread);
