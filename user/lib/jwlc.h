/* libjwl's client side, inside (<jwl_client.h> is the API): what
 * jwl_client.c (the connection, its globals, the event queue, reconnect),
 * jwl_shm.c (pools, buffers), jwl_window.c (windows) and jwl_seat.c
 * (keyboard, pointer) share.
 *
 * Ids. Every object of the program's keeps the id it has on the current
 * connection, 0 while it has none (before the client is ready, after a
 * connection is lost). When a connection becomes ready, jwlc_rebuild
 * makes every object with id 0 again, pools first, then buffers, then
 * windows. The map's data of each id points back at its object: the
 * client for the globals, the keyboard and the pointer; a struct
 * jwl_callback for a wl_callback; the buffer, the pool, the window.
 * An object is forgotten by the transport itself when its destructor is
 * sent or received (the tables' destructor bits). */
#pragma once

#include <jwl.h>
#include <jwl/cursor_shape_v1.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <jwl_client.h>

enum jwlc_state {
    JWLC_DOWN,       /* no connection: the next try at retry_at */
    JWLC_REGISTRY,   /* get_registry and sync sent: the globals are coming */
    JWLC_BINDING,    /* bound, sync sent: their first events are coming */
    JWLC_READY,
    JWLC_DEAD,       /* for good: why */
};

/* A global the registry offered that we want. */
struct jwlc_offer {
    uint32_t name;             /* the registry's name for it; 0: not offered */
    uint32_t version;          /* the version offered */
};

enum { JWLC_COMPOSITOR, JWLC_SHM, JWLC_WM_BASE, JWLC_SEAT, JWLC_OUTPUT, JWLC_CURSOR_SHAPE,
       JWLC_DATA, JWLC_GLOBALS };

/* A global the library binds (jwl_client.c's jwlc_wanted, by the enum). */
struct jwlc_want {
    const struct jwl_interface *iface;
    uint32_t want;                /* the most we bind it at */
    bool     needed;              /* no client without it */
};
extern const struct jwlc_want jwlc_wanted[JWLC_GLOBALS];

struct jwl_pool {
    struct jwl_client *c;
    struct jwl_pool   *next;          /* the client's pools */
    struct jwl_buffer *buffers;       /* this pool's */
    handle_t vmo;                     /* ours: may resize */
    uint64_t size;                    /* bytes, whole pages */
    uint8_t *data;                    /* mapped read-write here */
    uint32_t id;                      /* wl_shm_pool on this connection; 0: none */
};

struct jwl_buffer {
    struct jwl_pool   *pool;
    struct jwl_buffer *next;          /* the pool's buffers */
    uint64_t offset;
    int32_t  width, height, stride;
    uint32_t format;
    uint32_t id;                      /* wl_buffer on this connection; 0: none */
    bool     busy;                    /* attached, not released */
};

struct jwl_window {
    struct jwl_client *c;
    struct jwl_window *next;          /* the client's windows */
    void    *user;
    char     title[JWL_TEXT_MAX];
    char     app_id[JWL_TEXT_MAX];
    int32_t  want_w, want_h;          /* the size the program asked */
    bool     resizable, alpha;
    bool     fullscreen, maximized;   /* what the program asked last */
    uint32_t surface, xdg_surface, toplevel;   /* ids on this connection; 0: none */
    /* configure: the toplevel's part waits for xdg_surface.configure */
    int32_t  pend_w, pend_h;
    uint32_t pend_states;
    bool     configured;              /* a configure came on this connection */
    bool     ack_due;                 /* ack_serial not acked yet */
    uint32_t ack_serial;
    int32_t  width, height;           /* the size to draw at */
    uint32_t states;
    /* buffers: two slots of slot_bytes each in pool */
    struct jwl_pool   *pool;
    struct jwl_buffer *buf[2];
    uint64_t slot_bytes;
    int      began;                   /* the slot jwl_window_begin gave; -1: none */
    int      shown;                   /* the slot last presented; -1: none */
    bool     rebuilt;                 /* made again after a reconnect, not configured since */
    bool     frame_lost;              /* a frame callback went with the old connection */
    uint32_t press_serial;            /* the last button press on this window; 0: none */
    struct jwl_callback frame;       /* the frame callback waiting */
};

struct jwlc_seat {
    uint32_t keyboard, pointer;       /* ids on this connection; 0: none */
    uint32_t shape_dev;               /* the pointer's wp_cursor_shape_device_v1; 0: none */
    uint32_t enter_serial;            /* the pointer's last enter (set_shape's serial) */
    struct jwl_window *kb_focus, *ptr_focus;
    uint32_t mods;                    /* KEYMAP_MOD_* */
    uint32_t repeat_code;             /* the key repeating; 0: none */
    uint64_t repeat_next;             /* when it repeats next */
    int32_t  ptr_x, ptr_y;            /* the last position, fixed */
    int32_t  discrete[2];             /* axis_discrete waiting for its axis, per axis */
    uint32_t input_serial;            /* the last input event's (enter, key or button press) */
};

/* The clipboard (jwl_data.c). Offers the compositor introduced are kept
 * by id until a selection event names one (then it is the selection) or
 * another (then they are destroyed). */
#define JWLC_OFFERS 4u
struct jwlc_offer_rec {
    uint32_t id;                      /* wl_data_offer; 0: a free entry */
    uint8_t  types;                   /* bit n: jwlc_text_types[n] offered */
};
struct jwlc_clip {
    uint32_t device;                  /* wl_data_device on this connection; 0: none */
    uint32_t source;                  /* our wl_data_source, while it offers text; 0: none */
    char    *text;                    /* what it offers (malloc'd), and its bytes */
    size_t   len;
    struct jwlc_offer_rec offers[JWLC_OFFERS];   /* introduced, not the selection (yet) */
    struct jwlc_offer_rec selection;  /* the selection's offer; id 0: none */
    handle_t rx;                      /* a paste's end of its channel; HANDLE_INVALID: none */
    uint64_t rx_deadline;             /* when it is given up */
    char    *buf;                     /* what came so far, cap bytes of room */
    size_t   got, cap;
    char    *pasted;                  /* the last paste that ended OK (NUL-terminated) */
    size_t   pasted_len;
};

struct jwl_client {
    struct jwl_client_config cfg;
    const char *name;
    enum jwlc_state state;
    status_t why;                     /* DEAD: why; DOWN: what ended the last one */
    struct jwl_conn *conn;            /* NULL while DOWN or DEAD */
    uint64_t retry_at;                /* DOWN: the next try */
    uint32_t retry_ms;                /* the pause after the next failure */
    status_t fatal;                   /* OK; else never again, and why (wl_display.error
                                         came: ERR_INVALID_ARGS; a global missing:
                                         ERR_NOT_SUPPORTED) */
    handle_t port;                    /* jwl_client_bind_port's; HANDLE_INVALID: none */
    uint64_t port_key;
    struct jwl_client_info info;
    uint32_t registry;
    uint32_t global[JWLC_GLOBALS];    /* bound ids on this connection; 0: none */
    struct jwlc_offer offer[JWLC_GLOBALS];
    struct jwl_callback setup;       /* the sync of REGISTRY and BINDING */
    struct jwl_callback rt;          /* jwl_client_roundtrip's sync */
    struct jwlc_seat seat;
    struct jwlc_clip clip;
    uint32_t cursor_shape;            /* jwl_client_set_cursor's; 0: none */
    struct jwl_pool   *pools;
    struct jwl_window *windows;
    struct jwl_event q[JWL_EVENT_QUEUE];
    unsigned qhead, qlen;
};

/* jwl_events.c */
/* Queue ev for the program (motion merged into a queued motion). */
void     jwlc_queue(struct jwl_client *c, const struct jwl_event *ev);
/* Queue a connection event (DISCONNECTED, RECONNECTED, DEAD). */
void     jwlc_queue_conn(struct jwl_client *c, uint32_t type, status_t why);
/* Drop every queued event about w. */
void     jwlc_unqueue(struct jwl_client *c, const struct jwl_window *w);
/* Take message m (ERR_NOT_SUPPORTED: nobody did; its handles are the
 * caller's to close). */
status_t jwlc_handle(struct jwl_client *c, struct jwl_msg *m);

/* jwl_client.c */
/* After a send that named the new id made for it: if the send refused the
 * id itself, give it back (<jwl.h>, jwl_conn_make). Returns st. */
status_t jwlc_made(struct jwl_client *c, status_t st, uint32_t id);
/* A new id of iface at version for the next request, data its map data. */
status_t jwlc_make(struct jwl_client *c, const struct jwl_interface *iface, uint32_t version,
                   void *data, uint32_t *out);
/* The client is ready on a connection: requests can go. */
bool     jwlc_live(const struct jwl_client *c);
void     jwlc_log(const struct jwl_client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* jwl_shm.c */
status_t jwlc_pool_make(struct jwl_pool *p);
status_t jwlc_buffer_make(struct jwl_buffer *b);
/* Every pool's and buffer's id is 0 and no buffer busy (the connection
 * went). */
void     jwlc_shm_lost(struct jwl_client *c);
/* An event for a pool or buffer object (wl_buffer.release). */
status_t jwlc_shm_event(struct jwl_client *c, struct jwl_msg *m);

/* jwl_window.c */
status_t jwlc_window_make(struct jwl_window *w);
void     jwlc_windows_lost(struct jwl_client *c);
/* Events of wl_surface, xdg_surface, xdg_toplevel. */
status_t jwlc_window_event(struct jwl_client *c, struct jwl_msg *m);
/* A frame callback's done. */
void     jwlc_window_frame_done(struct jwl_window *w, uint32_t time, bool made_up);
/* The window whose wl_surface is id, NULL if none. */
struct jwl_window *jwlc_window_of_surface(struct jwl_client *c, uint32_t id);

/* jwl_seat.c */
/* wl_seat's capabilities: get or release the keyboard and the pointer. */
status_t jwlc_seat_caps(struct jwl_client *c, uint32_t caps);
void     jwlc_seat_lost(struct jwl_client *c);
/* Events of wl_keyboard and wl_pointer. */
status_t jwlc_seat_event(struct jwl_client *c, struct jwl_msg *m);
/* A window goes: the focus that was on it goes too. */
void     jwlc_seat_window_gone(struct jwl_client *c, const struct jwl_window *w);
/* Key repeat: the next repeat's time (DEADLINE_NEVER: none), and the
 * repeat events due by now. */
uint64_t jwlc_seat_deadline(const struct jwl_client *c);
void     jwlc_seat_tick(struct jwl_client *c);

/* jwl_data.c */
/* The globals are bound: the data device, if there is a clipboard. */
void     jwlc_clip_bound(struct jwl_client *c);
/* The connection went: the clipboard's ids, our selection, a paste. */
void     jwlc_clip_lost(struct jwl_client *c);
/* Events of wl_data_device, wl_data_offer and wl_data_source. */
status_t jwlc_clip_event(struct jwl_client *c, struct jwl_msg *m);
/* A paste's channel: what came, the end, the time running out. */
void     jwlc_clip_tick(struct jwl_client *c);
/* When a paste must be looked at next (DEADLINE_NEVER: none). */
uint64_t jwlc_clip_deadline(const struct jwl_client *c);
/* Free what the clipboard holds (jwl_client_destroy). */
void     jwlc_clip_free(struct jwl_client *c);
