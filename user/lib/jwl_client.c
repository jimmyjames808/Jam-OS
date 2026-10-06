/* libjwl's client (<jwl_client.h>): the connection to the compositor, its
 * globals, the event queue, and reconnecting.
 *
 * A client goes through the states of enum jwlc_state: DOWN (no channel;
 * the next try at retry_at), REGISTRY (wl_display.get_registry and a sync
 * sent: the globals arrive before the sync's done), BINDING (the wanted
 * globals bound and a second sync sent: their first events, the shm
 * formats, the seat's capabilities, the output's mode, arrive before its
 * done), READY, and DEAD for good. Nothing in a state change waits: each
 * step happens when jwl_client_dispatch reads the event it waited for, so
 * a reconnect never blocks the program's loop but in the connect function.
 *
 * A lost connection (the channel's peer closed, or the transport found
 * the compositor breaking the protocol) takes every id with it: pools,
 * buffers, windows and the seat forget theirs, the program hears
 * JWL_EV_DISCONNECTED, and jwlc_rebuild makes them again once the next
 * connection is READY. A connection the compositor ended with
 * wl_display.error is our own fault and would only end the same way
 * again: the client is DEAD instead. */
#include <jwl_client.h>
#include <os.h>
#include "jwlc.h"

#define RETRY_FIRST_MS 50u   /* the pause after the first failed try; doubles to the max */

/* What an untyped new id may name: everything this library binds. */
static const struct jwl_interface *const known[] = {
    &jwl_wl_compositor_interface, &jwl_wl_shm_interface,    &jwl_xdg_wm_base_interface,
    &jwl_wl_seat_interface,       &jwl_wl_output_interface,
};

/* The globals we bind, by enum index, and the most we bind each at. */
const struct jwlc_want jwlc_wanted[JWLC_GLOBALS] = {
    [JWLC_COMPOSITOR] = { &jwl_wl_compositor_interface, JWL_CLIENT_COMPOSITOR_VERSION, true },
    [JWLC_SHM]        = { &jwl_wl_shm_interface, JWL_CLIENT_SHM_VERSION, true },
    [JWLC_WM_BASE]    = { &jwl_xdg_wm_base_interface, JWL_CLIENT_WM_BASE_VERSION, false },
    [JWLC_SEAT]       = { &jwl_wl_seat_interface, JWL_CLIENT_SEAT_VERSION, false },
    [JWLC_OUTPUT]     = { &jwl_wl_output_interface, JWL_CLIENT_OUTPUT_VERSION, false },
    [JWLC_CURSOR_SHAPE] = { &jwl_wp_cursor_shape_manager_v1_interface,
                            JWL_CLIENT_CURSOR_SHAPE_VERSION, false },
};

void jwlc_log(const struct jwl_client *c, const char *fmt, ...)
{
    if (c->cfg.quiet)
        return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s: %s\n", c->name, line);
}

bool jwlc_live(const struct jwl_client *c)
{
    return c->state == JWLC_READY && c->conn;
}

/* ---- ids ---------------------------------------------------------------------------- */

status_t jwlc_make(struct jwl_client *c, const struct jwl_interface *iface, uint32_t version,
                   void *data, uint32_t *out)
{
    if (!c->conn)
        return ERR_PEER_CLOSED;
    return jwl_conn_make(c->conn, iface, version, data, out);
}

status_t jwlc_made(struct jwl_client *c, status_t st, uint32_t id)
{
    struct jwl_object *o = c->conn ? jwl_conn_object(c->conn, id) : NULL;
    if (st != OK && o && o->pending)
        (void)jwl_conn_delete(c->conn, id);   /* refused: no gap left in our ids */
    return st;
}

/* wl_display.sync, its done to land in cb. */
static status_t sync(struct jwl_client *c, struct jwl_callback *cb)
{
    uint32_t id;
    status_t st = jwlc_make(c, &jwl_wl_callback_interface, 1, cb, &id);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_display_sync(c->conn, JWL_DISPLAY_ID, id), id);
    if (st == OK) {
        cb->id = id;
        cb->done = false;
    }
    return st;
}

/* ---- connecting and losing it --------------------------------------------------------- */

static void bind_port(struct jwl_client *c)
{
    if (c->port == HANDLE_INVALID || !c->conn)
        return;
    status_t st = jam_port_bind(c->port, c->conn->ch, c->port_key, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK)
        jwlc_log(c, "can't bind the connection to the program's port: %s", status_str(st));
    /* Whatever arrived before the binding raised no edge: wake the loop. */
    struct port_packet p = { .key = c->port_key, .type = PORT_PACKET_USER };
    (void)jam_port_queue(c->port, &p);   /* a full port: the binding's edge comes anyway */
}

static void die(struct jwl_client *c, status_t why)
{
    c->state = JWLC_DEAD;
    c->why = why;
    jwlc_queue_conn(c, JWL_EV_DEAD, why);
    jwlc_log(c, "the connection to the compositor is gone for good (%s)", status_str(why));
}

/* The connection went (st: why): every id with it. */
static void lost(struct jwl_client *c, status_t st)
{
    if (c->port != HANDLE_INVALID)
        (void)jam_port_unbind(c->port, c->conn->ch, c->port_key);   /* closing unbinds anyway */
    if (c->conn->status == ERR_INVALID_ARGS)
        jwlc_log(c, "the compositor broke the protocol: %s", c->conn->error.text);
    jwl_conn_destroy(c->conn);
    c->conn = NULL;
    c->registry = 0;
    memset(c->global, 0, sizeof(c->global));
    c->setup.id = c->rt.id = 0;
    jwlc_seat_lost(c);
    jwlc_shm_lost(c);
    jwlc_windows_lost(c);
    jwlc_queue_conn(c, JWL_EV_DISCONNECTED, st);
    if (c->fatal != OK || c->cfg.no_reconnect) {
        die(c, c->fatal != OK ? c->fatal : st);
        return;
    }
    jwlc_log(c, "lost the compositor (%s): connecting again", status_str(st));
    c->state = JWLC_DOWN;
    c->why = st;
    c->retry_at = now() + (uint64_t)c->retry_ms * NS_PER_MS;
}

/* A connection on ch (consumed): the registry's requests go. */
static status_t start(struct jwl_client *c, handle_t ch)
{
    struct jwl_conn_config cc = { .ch = ch, .side = JWL_CLIENT,
                                  .display = &jwl_wl_display_interface, .known = known,
                                  .nknown = sizeof(known) / sizeof(known[0]) };
    status_t st = jwl_conn_create(&cc, &c->conn);
    if (st != OK) {
        c->conn = NULL;
        return st;
    }
    memset(c->offer, 0, sizeof(c->offer));
    c->info.generation++;
    c->info.shm_formats = 0;
    c->info.seat_caps = 0;
    uint32_t id = 0;
    st = jwlc_make(c, &jwl_wl_registry_interface, 1, c, &id);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_display_get_registry(c->conn, JWL_DISPLAY_ID, id), id);
    if (st == OK)
        st = sync(c, &c->setup);
    if (st == OK)
        st = jwl_conn_flush(c->conn);
    if (st != OK) {
        jwl_conn_destroy(c->conn);
        c->conn = NULL;
        return st;
    }
    c->registry = id;
    c->state = JWLC_REGISTRY;
    bind_port(c);
    return OK;
}

static void try_connect(struct jwl_client *c)
{
    handle_t ch = HANDLE_INVALID;
    status_t st = c->cfg.connect ? c->cfg.connect(c->cfg.connect_ctx, &ch)
                                 : svc_open(JWL_SERVICE, &ch);
    if (st == OK)
        st = start(c, ch);
    if (st == OK)
        return;
    if (c->cfg.no_reconnect) {
        die(c, st);
        return;
    }
    if (st != ERR_SHOULD_WAIT && c->retry_ms == 0)
        jwlc_log(c, "can't reach the compositor (%s): trying again", status_str(st));
    c->retry_ms = c->retry_ms ? c->retry_ms * 2 : RETRY_FIRST_MS;
    if (c->retry_ms > JWL_RECONNECT_MAX_MS)
        c->retry_ms = JWL_RECONNECT_MAX_MS;
    c->retry_at = now() + (uint64_t)c->retry_ms * NS_PER_MS;
}

/* ---- the setup steps -------------------------------------------------------------------- */

static void bind_globals(struct jwl_client *c)
{
    uint32_t *versions[JWLC_GLOBALS] = {
        [JWLC_COMPOSITOR] = &c->info.compositor_version, [JWLC_SHM] = &c->info.shm_version,
        [JWLC_WM_BASE] = &c->info.wm_base_version,       [JWLC_SEAT] = &c->info.seat_version,
        [JWLC_OUTPUT] = &c->info.output_version,
        [JWLC_CURSOR_SHAPE] = &c->info.cursor_shape_version,
    };
    for (unsigned i = 0; i < JWLC_GLOBALS; i++) {
        *versions[i] = 0;
        uint32_t v = c->offer[i].version, want = jwlc_wanted[i].want;
        if (v > want)
            v = want;   /* the lower of theirs and ours */
        if (!c->offer[i].name || !v)
            continue;
        uint32_t id;
        status_t st = jwlc_make(c, jwlc_wanted[i].iface, v, c, &id);
        if (st == OK)
            st = jwlc_made(c, jwl_wl_registry_bind(c->conn, c->registry, c->offer[i].name,
                                                   jwlc_wanted[i].iface->name, v, id), id);
        if (st == OK) {
            c->global[i] = id;
            *versions[i] = v;
        }
    }
}

/* Make every object of the program's that has no id on this connection. */
static void rebuild(struct jwl_client *c)
{
    for (struct jwl_pool *p = c->pools; p; p = p->next) {
        if (!p->id)
            (void)jwlc_pool_make(p);   /* a failure here is the connection's: seen next read */
        for (struct jwl_buffer *b = p->buffers; b; b = b->next)
            if (!b->id)
                (void)jwlc_buffer_make(b);
    }
    for (struct jwl_window *w = c->windows; w; w = w->next)
        if (!w->surface)
            (void)jwlc_window_make(w);
}

/* What the client can't do without, NULL if nothing: a needed global
 * (after the binds), a pixel format every compositor must offer (after
 * the globals' first events). */
static const char *missing(const struct jwl_client *c)
{
    for (unsigned i = 0; i < JWLC_GLOBALS; i++)
        if (jwlc_wanted[i].needed && !c->global[i])
            return jwlc_wanted[i].iface->name;
    uint32_t fmts = 1u << JWL_WL_SHM_FORMAT_XRGB8888 | 1u << JWL_WL_SHM_FORMAT_ARGB8888;
    if (c->state == JWLC_READY && !(c->info.shm_formats & fmts))
        return "wl_shm format xrgb8888 or argb8888";
    return NULL;
}

/* The setup's sync came back: the next step. A compositor without what
 * we need ends the client (c->fatal): another connection would offer no
 * more. */
static void advance(struct jwl_client *c)
{
    c->setup.done = false;
    if (c->state == JWLC_REGISTRY) {
        bind_globals(c);
        c->state = JWLC_BINDING;
        if (!missing(c))
            (void)sync(c, &c->setup);   /* a failure is the connection's: seen next read */
    } else {
        c->state = JWLC_READY;
    }
    const char *what = missing(c);
    if (what) {
        jwlc_log(c, "the compositor offers no %s", what);
        c->fatal = ERR_NOT_SUPPORTED;
        return;
    }
    if (c->state != JWLC_READY)
        return;
    c->retry_ms = 0;
    rebuild(c);
    if (c->info.generation > 1) {
        jwlc_log(c, "connected again (connection %u)", c->info.generation);
        jwlc_queue_conn(c, JWL_EV_RECONNECTED, OK);
    }
}

/* Every message waiting, until the channel is empty or the connection goes. */
static void read_all(struct jwl_client *c)
{
    while (c->conn) {
        struct jwl_msg m;
        status_t st = jwl_conn_next(c->conn, &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK) {
            lost(c, st);
            return;
        }
        if (jwlc_handle(c, &m) == ERR_NOT_SUPPORTED)
            jwl_msg_close_handles(&m);   /* nobody took them */
        if (c->setup.done)
            advance(c);
        if (c->fatal != OK) {
            /* wl_display.error (the compositor may keep its end open until
             * we close ours), or a global we need missing */
            lost(c, c->fatal);
            return;
        }
    }
}

/* ---- the API -------------------------------------------------------------------------- */

status_t jwl_client_dispatch(struct jwl_client *c)
{
    if (c->state == JWLC_DOWN && now() >= c->retry_at)
        try_connect(c);
    read_all(c);
    jwlc_seat_tick(c);
    if (c->conn) {
        status_t st = jwl_conn_flush(c->conn);
        if (st != OK)
            lost(c, st);
    }
    return c->state == JWLC_DEAD ? c->why : OK;
}

status_t jwl_client_flush(struct jwl_client *c)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    return c->conn ? jwl_conn_flush(c->conn) : ERR_PEER_CLOSED;
}

uint64_t jwl_client_deadline(const struct jwl_client *c)
{
    if (c->state == JWLC_DOWN)
        return c->retry_at;
    return c->state == JWLC_READY ? jwlc_seat_deadline(c) : DEADLINE_NEVER;
}

/* Until something may have happened: the channel, a timer, or deadline. */
static void wait_a_while(const struct jwl_client *c, uint64_t deadline)
{
    uint64_t d = jwl_client_deadline(c);
    if (deadline < d)
        d = deadline;
    if (c->conn) {
        signals_t seen;
        (void)jam_object_wait_one(c->conn->ch, SIG_READABLE | SIG_PEER_CLOSED, d, &seen);
    } else if (d != DEADLINE_NEVER) {
        (void)jam_nanosleep(d);
    }
}

status_t jwl_client_wait_event(struct jwl_client *c, uint64_t deadline, struct jwl_event *out)
{
    for (;;) {
        if (jwl_client_next_event(c, out) == OK)
            return OK;
        if (c->state == JWLC_DEAD)
            return c->why;
        (void)jwl_client_dispatch(c);
        if (jwl_client_next_event(c, out) == OK)
            return OK;
        if (c->state == JWLC_DEAD)
            continue;
        if (now() >= deadline)
            return ERR_TIMED_OUT;
        wait_a_while(c, deadline);
    }
}

status_t jwl_client_roundtrip(struct jwl_client *c, uint64_t deadline)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    if (!c->conn)
        return ERR_PEER_CLOSED;
    uint32_t gen = c->info.generation;
    status_t st = sync(c, &c->rt);
    if (st == OK)
        st = jwl_conn_flush(c->conn);
    while (st == OK && !c->rt.done) {
        if (c->info.generation != gen || !c->conn)
            return c->state == JWLC_DEAD ? c->why : ERR_PEER_CLOSED;
        if (now() >= deadline) {
            c->rt.id = 0;   /* a late done is dropped */
            return ERR_TIMED_OUT;
        }
        wait_a_while(c, deadline);
        st = jwl_client_dispatch(c);
    }
    return st;
}

status_t jwl_client_create(const struct jwl_client_config *cfg, struct jwl_client **out)
{
    struct jwl_client *c = calloc(1, sizeof(*c));
    if (!c)
        return ERR_NO_MEMORY;
    c->cfg = *cfg;
    c->name = cfg->name ? cfg->name : "jwl";
    c->port = HANDLE_INVALID;
    c->state = JWLC_DOWN;
    c->info.output_scale = 1;
    try_connect(c);
    if (c->state == JWLC_DEAD) {
        status_t st = c->why;
        free(c);
        return st;
    }
    *out = c;
    return OK;
}

status_t jwl_client_connect(const struct jwl_client_config *cfg, uint64_t deadline,
                            struct jwl_client **out)
{
    struct jwl_client *c = NULL;
    status_t st = jwl_client_create(cfg, &c);
    while (st == OK && c->state != JWLC_READY) {
        if (c->state == JWLC_DEAD)
            st = c->why;
        else if (now() >= deadline)
            st = ERR_TIMED_OUT;
        else
            wait_a_while(c, deadline);
        if (st == OK)
            st = jwl_client_dispatch(c);
    }
    if (st != OK) {
        jwl_client_destroy(c);
        return st;
    }
    *out = c;
    return OK;
}

void jwl_client_destroy(struct jwl_client *c)
{
    if (!c)
        return;
    while (c->windows)
        jwl_window_destroy(c->windows);
    while (c->pools) {
        while (c->pools->buffers)
            jwl_buffer_destroy(c->pools->buffers);
        jwl_pool_destroy(c->pools);
    }
    if (c->conn) {
        if (c->port != HANDLE_INVALID)
            (void)jam_port_unbind(c->port, c->conn->ch, c->port_key);
        (void)jwl_conn_flush(c->conn);   /* the destroys: the compositor frees at once */
        jwl_conn_destroy(c->conn);
    }
    free(c);
}

status_t jwl_client_status(const struct jwl_client *c)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    return c->state == JWLC_READY ? OK : ERR_SHOULD_WAIT;
}

const struct jwl_client_info *jwl_client_info(const struct jwl_client *c)
{
    return &c->info;
}

struct jwl_conn *jwl_client_conn(struct jwl_client *c)
{
    return c->conn;
}

handle_t jwl_client_channel(const struct jwl_client *c)
{
    return c->conn ? c->conn->ch : HANDLE_INVALID;
}

status_t jwl_client_bind_port(struct jwl_client *c, handle_t port, uint64_t key)
{
    if (c->port != HANDLE_INVALID && c->conn)
        (void)jam_port_unbind(c->port, c->conn->ch, c->port_key);
    c->port = port;
    c->port_key = key;
    if (!c->conn)
        return OK;
    status_t st = jam_port_bind(port, c->conn->ch, key, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK)
        c->port = HANDLE_INVALID;
    return st;
}

uint32_t jwl_client_global(const struct jwl_client *c, const struct jwl_interface *iface)
{
    for (unsigned i = 0; i < JWLC_GLOBALS; i++)
        if (jwlc_wanted[i].iface == iface)
            return c->conn ? c->global[i] : 0;
    return 0;
}

status_t jwl_client_frame(struct jwl_client *c, uint32_t surface, struct jwl_callback *cb)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    uint32_t id;
    status_t st = jwlc_make(c, &jwl_wl_callback_interface, c->info.compositor_version, cb, &id);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_surface_frame(c->conn, surface, id), id);
    if (st == OK) {
        cb->id = id;
        cb->done = false;
        cb->win = NULL;
    }
    return st;
}
