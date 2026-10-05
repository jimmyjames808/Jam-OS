/* <jwl_client.h>: libjwl's client side (user/lib/jwl_client.c, jwl_shm.c,
 * jwl_window.c, jwl_seat.c), on top of <jwl.h>'s codec and transport.
 * What a Jam OS program uses to open windows on the compositor
 * (docs/G1-PLAN.md, "libjwl: the codec and the client library").
 *
 * The model:
 *   - a struct jwl_client is one program's link to the compositor: the
 *     connection (a channel from /svc/wayland), the globals it bound
 *     (wl_compositor, wl_shm, xdg_wm_base, wl_seat, wl_output, at the
 *     versions below or lower if the compositor offers less), the seat's
 *     keyboard and pointer, and a queue of events for the program
 *     (struct jwl_event);
 *   - pools (struct jwl_pool) are VMOs whose pages stay (VMO_KEEP_PAGES),
 *     mapped into the program and shared with the compositor as
 *     wl_shm_pool; buffers (struct jwl_buffer) are rectangles of a pool;
 *   - a window (struct jwl_window) is a wl_surface with the xdg_toplevel
 *     role, its configure/ack loop, two buffers of its own and frame
 *     callbacks.
 *
 * Every object of the program's (pools, buffers, windows) outlives the
 * connection. When the compositor dies the library notices the channel
 * close (JWL_EV_DISCONNECTED), connects again (first at once, then with a
 * growing pause up to a second), binds the globals again and makes every
 * pool, buffer and window again on the new connection, showing each
 * window's last buffer (its pixels are still in the pool), and then says
 * JWL_EV_RECONNECTED. A frame callback the dead compositor never answered
 * comes as a JWL_EV_FRAME once the window is back, so an animation goes on.
 * A connection the compositor ended with wl_display.error (this program
 * broke the protocol) is not made again: the client is dead.
 *
 * The loop. Nothing here blocks but jwl_client_connect, jwl_client_wait_event,
 * jwl_client_roundtrip and the connect step (below). A program with a loop
 * of its own binds the client's channel to its port (jwl_client_bind_port:
 * kept across reconnects), waits on the port with jwl_client_deadline as
 * the deadline (key repeat, a reconnect's pause), and on each wake calls
 * jwl_client_dispatch and then takes events with jwl_client_next_event
 * until ERR_SHOULD_WAIT. A simple program calls jwl_client_wait_event.
 *
 * Connecting is the config's connect function: by default svc_open of
 * JWL_SERVICE, a call that can wait up to svc_open's 5 s while a
 * restarted compositor comes up; a loop that serves others gives its own
 * that doesn't (one that answers ERR_SHOULD_WAIT until it has a channel is
 * asked again at the next pause).
 *
 * Threads: a client and everything made from it belong to one thread. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <keymap.h>
#include <os.h>

#define JWL_SERVICE "wayland"   /* the compositor's name under /svc */

/* The versions this library binds at most (the plan's table): a global
 * offered at less is bound at what is offered. */
#define JWL_CLIENT_COMPOSITOR_VERSION 4u   /* damage_buffer */
#define JWL_CLIENT_SHM_VERSION        1u
#define JWL_CLIENT_SEAT_VERSION       5u   /* wl_keyboard and wl_pointer at 5 too */
#define JWL_CLIENT_OUTPUT_VERSION     3u
#define JWL_CLIENT_WM_BASE_VERSION    1u

#define JWL_EVENT_QUEUE      256u    /* events waiting for the program; more are dropped */
#define JWL_SIZE_MAX         8192    /* a window's or buffer's width and height, at most */
#define JWL_TEXT_MAX         256u    /* a title's or app id's bytes kept, with the NUL */
#define JWL_RECONNECT_MAX_MS 1000u   /* the longest pause between two tries */

struct jwl_client;
struct jwl_pool;
struct jwl_buffer;
struct jwl_window;

/* ---- the client ----------------------------------------------------------------- */

struct jwl_client_config {
    /* A new connection's channel end into *out (the client owns it). NULL:
     * svc_open(JWL_SERVICE). ERR_SHOULD_WAIT: not yet, ask again at the
     * next pause; any other error: the same. */
    status_t (*connect)(void *ctx, handle_t *out);
    void     *connect_ctx;
    bool      no_reconnect;   /* a lost connection stays lost: the client is dead */
    bool      quiet;          /* no log lines (tests that break things on purpose) */
    bool      no_keyboard;    /* never bind wl_keyboard: no keys wanted, so the compositor
                               * never gives this client's windows the keyboard focus (the
                               * boot splash: what is typed meanwhile goes to the terminal) */
    const char *name;         /* the program's name, for log lines; NULL: "jwl" */
};

/* What the client knows, for the program and for tests. */
struct jwl_client_info {
    uint32_t generation;          /* connections made: 1 the first, +1 each reconnect */
    uint32_t compositor_version;  /* each global's bound version; 0: not offered */
    uint32_t shm_version;
    uint32_t seat_version;
    uint32_t output_version;
    uint32_t wm_base_version;
    uint32_t shm_formats;         /* bit n: wl_shm format n offered (n < 32) */
    int32_t  output_width;        /* the output's current mode; 0 until told */
    int32_t  output_height;
    int32_t  output_refresh;      /* mHz */
    int32_t  output_scale;        /* 1 until told */
    uint32_t seat_caps;           /* JWL_WL_SEAT_CAPABILITY_* */
    const struct keymap *keymap;  /* the keyboard's layout (keymap_of_xkb), NULL: no keyboard */
    int32_t  repeat_rate;         /* keys per second, 0: no repeat */
    int32_t  repeat_delay;        /* ms before the first repeat */
    uint32_t error_object;        /* wl_display.error that ended it: object, code, text */
    uint32_t error_code;
    char     error_text[120];
    uint64_t events_dropped;      /* the queue was full */
    uint64_t pings;               /* xdg_wm_base pings answered */
};

/* Start a client (cfg is copied; its name kept by pointer): connects at
 * once by cfg->connect and sends the registry's requests; the globals are
 * bound as the replies come (jwl_client_dispatch). ERR_NO_MEMORY; the
 * connect step's error (with no_reconnect; otherwise the client starts
 * and tries again later). */
status_t jwl_client_create(const struct jwl_client_config *cfg, struct jwl_client **out);
/* jwl_client_create, then dispatch until the client is ready (every
 * global bound and their first events read) or deadline: OK; ERR_TIMED_OUT
 * (the client is destroyed); ERR_NOT_SUPPORTED: no wl_compositor or
 * wl_shm, or neither xrgb8888 nor argb8888 (the other globals are
 * optional: no xdg_wm_base, no windows); a dead connection's status. */
status_t jwl_client_connect(const struct jwl_client_config *cfg, uint64_t deadline,
                            struct jwl_client **out);
/* Destroy every window, buffer and pool still alive, then the connection. */
void     jwl_client_destroy(struct jwl_client *c);

/* OK while ready; ERR_SHOULD_WAIT while (re)connecting; once dead, why. */
status_t jwl_client_status(const struct jwl_client *c);
const struct jwl_client_info *jwl_client_info(const struct jwl_client *c);
/* The connection now (NULL while there is none), for requests of a
 * program's own and for tests. Don't keep it: a reconnect replaces it. */
struct jwl_conn *jwl_client_conn(struct jwl_client *c);
/* The connection's channel now, HANDLE_INVALID while there is none. */
handle_t jwl_client_channel(const struct jwl_client *c);

/* Bind the channel to port under key (SIG_READABLE | SIG_PEER_CLOSED,
 * persistent), now and on every connection after: the port's packet with
 * key means "call jwl_client_dispatch". Errors: port_bind's. */
status_t jwl_client_bind_port(struct jwl_client *c, handle_t port, uint64_t key);
/* When dispatch has work of its own to do (a key repeat, a reconnect):
 * an absolute deadline, DEADLINE_NEVER for none. */
uint64_t jwl_client_deadline(const struct jwl_client *c);

/* Read every message waiting, run what is due (key repeat, a reconnect
 * try), and flush. Never blocks but in the connect step. OK; a dead
 * client's status. */
status_t jwl_client_dispatch(struct jwl_client *c);
/* The id of a global bound on the connection now (iface: one of the five
 * this library binds, e.g. &jwl_wl_compositor_interface), 0 if none: for
 * requests of the program's own. */
uint32_t jwl_client_global(const struct jwl_client *c, const struct jwl_interface *iface);

/* A wl_callback answered: for a frame callback on a surface the program
 * made itself with requests of its own (jwl_client_frame). The library's
 * windows use the same. */
struct jwl_callback {
    uint32_t id;               /* the callback's id while it waits; 0 once done */
    bool     done;
    uint32_t time;             /* done's argument: the paint's time, ms */
    struct jwl_window *win;    /* the library's: a window's frame; NULL for the program's */
};
/* wl_surface.frame on the program's own surface `surface` (made from the
 * bound wl_compositor, so of its version): cb->done becomes true when it
 * is answered, at a dispatch. Keep cb until then or until the connection
 * goes (JWL_EV_DISCONNECTED: the callback goes with it, never done). The
 * program's own objects have map data NULL and the library ignores their
 * events; it doesn't make them again after a reconnect. */
status_t jwl_client_frame(struct jwl_client *c, uint32_t surface, struct jwl_callback *cb);

/* Write what is waiting to go. OK, or the connection's status. */
status_t jwl_client_flush(struct jwl_client *c);
/* wl_display.sync, dispatching until its answer: every event the
 * compositor sent before it has been read. OK; ERR_TIMED_OUT;
 * ERR_PEER_CLOSED: the connection went meanwhile; a dead client's status. */
status_t jwl_client_roundtrip(struct jwl_client *c, uint64_t deadline);

/* ---- events -------------------------------------------------------------------- */

enum jwl_event_type {
    JWL_EV_NONE,
    JWL_EV_CONFIGURE,       /* a window's size or states: draw at the size, then present */
    JWL_EV_CLOSE,           /* the user asked a window to close */
    JWL_EV_FRAME,           /* a frame callback: a good time to draw the next frame */
    JWL_EV_KEYBOARD_ENTER,  /* a window got the keyboard focus */
    JWL_EV_KEYBOARD_LEAVE,  /* ... lost it (keys held are released, as far as we know) */
    JWL_EV_KEY,             /* a key pressed, released or repeated */
    JWL_EV_MODIFIERS,       /* the modifiers changed (key.mods) */
    JWL_EV_POINTER_ENTER,   /* the pointer came over a window (pointer.x, .y) */
    JWL_EV_POINTER_LEAVE,
    JWL_EV_POINTER_MOTION,  /* (pointer.x, .y); queued motion is merged */
    JWL_EV_POINTER_BUTTON,  /* button.button (evdev: 0x110 left), .pressed */
    JWL_EV_POINTER_AXIS,    /* axis.axis (0 vertical), .value (fixed), .discrete (notches) */
    JWL_EV_DISCONNECTED,    /* the connection went (conn.why); windows come back by themselves */
    JWL_EV_RECONNECTED,     /* every object made again on connection conn.generation */
    JWL_EV_DEAD,            /* the client is dead for good (conn.why) */
};

#define JWL_KEY_RELEASED 0u
#define JWL_KEY_PRESSED  1u
#define JWL_KEY_REPEATED 2u   /* made by the library from repeat_info */

/* Bits of jwl_event.configure.states and jwl_window_states: 1 << each
 * xdg_toplevel state (JWL_XDG_TOPLEVEL_STATE_*). */
#define JWL_STATE(s) (1u << (s))

struct jwl_event {
    uint32_t type;              /* enum jwl_event_type */
    struct jwl_window *win;     /* the window it is about; NULL for none */
    union {
        struct {
            int32_t  width, height;   /* the size to draw at (the window's, after the rules) */
            int32_t  asked_w, asked_h;/* what the compositor said (0: the client picks) */
            uint32_t states;          /* JWL_STATE bits */
            bool     rebuilt;         /* the first after a reconnect; the last buffer shows */
        } configure;
        struct {
            uint32_t time;            /* the compositor's time of the paint, ms */
            bool     made_up;         /* after a reconnect: the callback was lost */
        } frame;
        struct {
            uint32_t code;            /* the evdev key code */
            uint32_t state;           /* JWL_KEY_* */
            uint32_t sym;             /* the keysym it types (0: none) */
            uint32_t cp;              /* the character it types (0: none) */
            uint32_t mods;            /* KEYMAP_MOD_* at the time */
            uint32_t time;            /* ms, the compositor's */
        } key;
        struct {
            int32_t x, y;             /* surface coordinates, fixed 24.8 */
        } pointer;
        struct {
            uint32_t button;          /* evdev: 0x110 left, 0x111 right, 0x112 middle */
            bool     pressed;
            uint32_t time;
        } button;
        struct {
            uint32_t axis;            /* JWL_WL_POINTER_AXIS_* */
            int32_t  value;           /* fixed 24.8 */
            int32_t  discrete;        /* notches (axis_discrete), 0 if none came */
        } axis;
        struct {
            status_t why;             /* DISCONNECTED, DEAD: what ended the connection */
            uint32_t generation;      /* RECONNECTED: info.generation */
        } conn;
    };
};

/* The next event: OK with *out; ERR_SHOULD_WAIT when there is none (call
 * jwl_client_dispatch after the next wake). Never blocks. */
status_t jwl_client_next_event(struct jwl_client *c, struct jwl_event *out);
/* The next event, dispatching and waiting for one until deadline: OK;
 * ERR_TIMED_OUT; a dead client's status once its JWL_EV_DEAD was taken. */
status_t jwl_client_wait_event(struct jwl_client *c, uint64_t deadline, struct jwl_event *out);

/* ---- pools and buffers (jwl_shm.c) ----------------------------------------------- */

/* A pool of size bytes (whole pages; at most INT32_MAX rounded down to a
 * page): a VMO_KEEP_PAGES VMO, every page committed now and charged to our
 * job, mapped read-write. The compositor gets a duplicate that may only
 * be read and mapped. ERR_INVALID_ARGS: 0 or too big; ERR_NO_MEMORY. */
status_t jwl_pool_create(struct jwl_client *c, uint64_t size, struct jwl_pool **out);
/* Grow the pool to size bytes (never smaller: ERR_INVALID_ARGS), mapped
 * again (jwl_pool_data moves) and told to the compositor. */
status_t jwl_pool_grow(struct jwl_pool *p, uint64_t size);
/* Destroy the pool once its buffers are (the compositor keeps what it
 * mapped until its buffers are gone). Its buffers must be destroyed first. */
void     jwl_pool_destroy(struct jwl_pool *p);
uint8_t *jwl_pool_data(const struct jwl_pool *p);
uint64_t jwl_pool_size(const struct jwl_pool *p);

/* A buffer of the pool: width x height pixels of format
 * (JWL_WL_SHM_FORMAT_XRGB8888 or ARGB8888, premultiplied), rows stride
 * bytes apart, from offset. ERR_INVALID_ARGS: a size outside 1 to
 * JWL_SIZE_MAX, stride < width * 4 or not a multiple of 4, outside the
 * pool, or a format the compositor didn't offer. */
status_t jwl_buffer_create(struct jwl_pool *p, uint64_t offset, int32_t width, int32_t height,
                           int32_t stride, uint32_t format, struct jwl_buffer **out);
void     jwl_buffer_destroy(struct jwl_buffer *b);
/* Its first pixel (moves when its pool grows). */
uint32_t *jwl_buffer_pixels(const struct jwl_buffer *b);
/* Attached to a surface and not released by the compositor yet: don't
 * draw into it. */
bool     jwl_buffer_busy(const struct jwl_buffer *b);
/* For a program attaching a buffer to a surface of its own: its wl_buffer
 * id on the connection now (0 while there is none), and the mark that it
 * was attached (busy until wl_buffer.release). */
uint32_t jwl_buffer_id(const struct jwl_buffer *b);
void     jwl_buffer_attached(struct jwl_buffer *b);

/* ---- windows (jwl_window.c) -------------------------------------------------------- */

struct jwl_window_config {
    int32_t     width, height;   /* the size it wants (1 to JWL_SIZE_MAX) */
    const char *title;           /* NULL: none; cut to JWL_TEXT_MAX - 1 bytes */
    const char *app_id;
    bool        resizable;       /* takes the sizes asked; else min = max = its own size */
    bool        alpha;           /* argb8888 (premultiplied) instead of xrgb8888 */
    bool        fullscreen;      /* ask for full screen from the start */
    bool        maximized;
};

/* A rectangle of a window's buffer, in pixels. */
struct jwl_rect {
    int32_t x, y, w, h;
};

/* What jwl_window_begin hands out: a buffer to draw the next frame into. */
struct jwl_frame {
    uint32_t *px;          /* pixel (x, y) is px[y * stride / 4 + x] */
    int32_t   width, height;
    int32_t   stride;      /* bytes */
    unsigned  slot;        /* which of the window's two buffers (0, 1) */
};

/* A window: its surface, xdg_surface and xdg_toplevel are made now (or
 * when the client is next ready), with the title, app id, sizes and
 * states asked. It can't show anything until its first JWL_EV_CONFIGURE.
 * ERR_INVALID_ARGS: a size out of range; ERR_NOT_SUPPORTED: the
 * compositor offers no xdg_wm_base; ERR_NO_MEMORY; a dead client's
 * status. */
status_t jwl_window_create(struct jwl_client *c, const struct jwl_window_config *cfg,
                           struct jwl_window **out);
/* Destroy it (its events still queued are dropped). */
void     jwl_window_destroy(struct jwl_window *w);
/* Configured on this connection: begin and present may be used. */
bool     jwl_window_configured(const struct jwl_window *w);
/* The size to draw at, and the states (JWL_STATE bits). */
void     jwl_window_size(const struct jwl_window *w, int32_t *width, int32_t *height);
uint32_t jwl_window_states(const struct jwl_window *w);
/* A buffer of the window's at its size that the compositor isn't using,
 * into *out (the one not shown, if both are free). ERR_BAD_STATE: not
 * configured yet; ERR_SHOULD_WAIT: both are busy (wait for an event and
 * try again); ERR_NO_MEMORY. */
status_t jwl_window_begin(struct jwl_window *w, struct jwl_frame *out);
/* The pixels of slot (0 or 1) at the window's size now, for a program
 * that keeps both buffers up to date: ERR_BAD_STATE if that buffer
 * isn't made (yet, or for this size). Don't write into a busy one. */
status_t jwl_window_slot(struct jwl_window *w, unsigned slot, struct jwl_frame *out);
/* Show what was drawn into the buffer of the last jwl_window_begin: acks
 * a pending configure, attaches it, damages rects (n of them; NULL: the
 * whole buffer), asks a frame callback if frame, commits and flushes.
 * ERR_BAD_STATE: no begin since the last present; a dead connection's
 * status (the frame shows after a reconnect). */
status_t jwl_window_present(struct jwl_window *w, const struct jwl_rect *rects, unsigned n,
                            bool frame);
status_t jwl_window_set_title(struct jwl_window *w, const char *title);
status_t jwl_window_set_fullscreen(struct jwl_window *w, bool on);
status_t jwl_window_set_maximized(struct jwl_window *w, bool on);
/* A move by the pointer, from the button press last seen on this window
 * (the compositor ignores it otherwise). ERR_BAD_STATE: no press seen. */
status_t jwl_window_move(struct jwl_window *w);
/* The program's own pointer for the window. */
void     jwl_window_set_user(struct jwl_window *w, void *user);
void    *jwl_window_user(const struct jwl_window *w);
