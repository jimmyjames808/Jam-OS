/* The compositor's connections (comp.h): one per opener of /svc/wayland,
 * made by the svc protocol's connect on the channel init keeps, each a
 * libjwl connection (<jwl.h>) on a channel of its own.
 *
 * A turn serves every client that has messages, COMP_BUDGET_MSGS or
 * COMP_BUDGET_NS each, whichever ends first, so one client writing flat
 * out delays the others by at most that; what is left waits for its next
 * turn (`more`, and the loop doesn't sleep). Each message goes to its
 * interface's request function (the table below; later tracks add their
 * interfaces' rows). A request that breaks the protocol, or a cap, ends
 * the connection with wl_display.error (comp_error), and the client is
 * torn down in the same turn: every object it made goes, its windows'
 * places are damaged, nothing of it is left (client_teardown).
 *
 * Message bytes. Everything we write to a client is charged to our job
 * until the client reads it; libjwl's window bounds that to JWL_WINDOW
 * batches a connection. A client we disconnect may never read what we
 * wrote, so its slot is kept ("lingering", nothing else of it) until it
 * closes its end too: what stays charged to us is bounded by
 * COMP_CLIENTS_MAX windows, however often a client reconnects. Each slot's
 * port binding on its channel end is charged to us too (about 1.2 KiB):
 * free_client unbinds it, as closing the handle doesn't.
 *
 * Destructors are libjwl's: a destructor request reaches its handler with
 * the id already freed (delete_id queued) and the object's data in the
 * message, for the handler to free; an event that destroys its object
 * (wl_callback.done) frees the id as it is sent. */
#include <stdarg.h>
#include <jwl/cursor_shape_v1.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include "comp.h"

#define LOG_PER_S 10u   /* disconnect lines a second at most (a client may reconnect in a loop) */

static struct comp_client *clients[COMP_CLIENTS_MAX];

/* What a bind's untyped new_id may name: the globals' interfaces. */
static const struct jwl_interface *const known[] = {
    &jwl_wl_compositor_interface, &jwl_wl_shm_interface,  &jwl_wl_output_interface,
    &jwl_wl_seat_interface,       &jwl_xdg_wm_base_interface,
    &jwl_wp_cursor_shape_manager_v1_interface, &jwl_wl_data_device_manager_interface,
};

/* Each interface's requests. */
static const struct {
    const struct jwl_interface *iface;
    status_t (*fn)(struct comp_client *cl, struct jwl_msg *m);
} requests[] = {
    { &jwl_wl_display_interface, display_request },
    { &jwl_wl_registry_interface, registry_request },
    { &jwl_wl_compositor_interface, compositor_request },
    { &jwl_wl_region_interface, region_request },
    { &jwl_wl_output_interface, output_request },
    { &jwl_wl_surface_interface, surface_request },
    { &jwl_wl_shm_interface, shm_request },
    { &jwl_wl_shm_pool_interface, pool_request },
    { &jwl_wl_buffer_interface, buffer_request },
    { &jwl_wl_seat_interface, seat_request },
    { &jwl_wl_keyboard_interface, keyboard_request },
    { &jwl_wl_pointer_interface, pointer_request },
    { &jwl_xdg_wm_base_interface, xdg_wm_base_request },
    { &jwl_xdg_positioner_interface, xdg_positioner_request },
    { &jwl_xdg_surface_interface, xdg_surface_request },
    { &jwl_xdg_toplevel_interface, xdg_toplevel_request },
    { &jwl_xdg_popup_interface, xdg_popup_request },
    { &jwl_wp_cursor_shape_manager_v1_interface, shapes_request },
    { &jwl_wp_cursor_shape_device_v1_interface, shape_device_request },
    { &jwl_wl_data_device_manager_interface, data_manager_request },
    { &jwl_wl_data_device_interface, data_device_request },
    { &jwl_wl_data_source_interface, data_source_request },
    { &jwl_wl_data_offer_interface, data_offer_request },
};

/* What goes with a client, module by module (later tracks add theirs:
 * roles before surfaces, so a role sees its surface still there; the
 * clipboard first, so a selection of its own is gone before its windows
 * going move the focus to another client, which then hears it is empty). */
static void (*const teardowns[])(struct comp_client *) = {
    data_teardown, xdg_teardown, surfaces_teardown, shm_teardown, display_teardown,
    seat_teardown,
};

static const char *const gone_names[COMP_GONE_COUNT] = { "closed", "protocol error", "too slow" };

/* ---- connecting -------------------------------------------------------------------- */

static status_t make_conn(handle_t mine, struct comp_client *cl)
{
    struct jwl_conn_config cfg = { .ch = mine, .side = JWL_SERVER,
                                   .display = &jwl_wl_display_interface, .known = known,
                                   .nknown = sizeof(known) / sizeof(known[0]) };
    status_t st = jwl_conn_create(&cfg, &cl->conn);   /* consumes mine */
    if (st == OK)
        st = jwl_map_set_data(&cl->conn->map, JWL_DISPLAY_ID, cl);
    return st;
}

static void free_client(struct comp_client *cl)
{
    jwl_conn_destroy(cl->conn);
    if (cl->raw != HANDLE_INVALID) {
        /* Closing the handle doesn't unbind it: the port's binding holds
         * the channel end and its charge (about 1.2 KiB of our message
         * bytes) until the port goes. A fired ONCE binding is gone already. */
        (void)jam_port_unbind(comp.port, cl->raw, 1 + cl->slot);
        jam_handle_close(cl->raw);
    }
    clients[cl->slot] = NULL;
    free(cl);
}

status_t conn_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    unsigned slot = 0;
    while (slot < COMP_CLIENTS_MAX && clients[slot])
        slot++;
    if (slot == COMP_CLIENTS_MAX) {
        comp.stats.refused++;
        return ERR_NO_RESOURCES;
    }
    struct comp_client *cl = calloc(1, sizeof(*cl));
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = cl ? jam_channel_create(&mine, &theirs) : ERR_NO_MEMORY;
    if (!cl)
        return st;
    cl->slot = slot;
    cl->raw = HANDLE_INVALID;
    clients[slot] = cl;
    if (st == OK)
        st = jam_handle_duplicate(mine, RIGHT_SAME, &cl->raw);
    if (st == OK)
        st = make_conn(mine, cl);
    else if (mine != HANDLE_INVALID)
        jam_handle_close(mine);
    if (st == OK)
        st = jam_port_bind(comp.port, cl->raw, 1 + slot, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        if (theirs != HANDLE_INVALID)
            jam_handle_close(theirs);
        free_client(cl);
        return st;
    }
    comp.stats.clients++;
    *out = theirs;
    return OK;
}

struct comp_client *conn_client_at(unsigned slot)
{
    struct comp_client *cl = slot < COMP_CLIENTS_MAX ? clients[slot] : NULL;
    return cl && cl->conn ? cl : NULL;
}

/* ---- errors and objects ------------------------------------------------------------ */

status_t comp_error(struct comp_client *cl, uint32_t object, uint32_t code, const char *fmt, ...)
{
    char text[sizeof(cl->conn->error.text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    (void)jwl_conn_post_error(cl->conn, object, code, text);   /* dead either way */
    return ERR_INVALID_ARGS;
}

status_t comp_no_memory(struct comp_client *cl, uint32_t object, const char *what)
{
    return comp_error(cl, object, JWL_ERROR_NO_MEMORY, "%s", what);
}

void *comp_object(struct comp_client *cl, uint32_t id, const struct jwl_interface *iface)
{
    struct jwl_object *o = jwl_conn_object(cl->conn, id);
    return o && o->iface == iface && !o->pending ? o->data : NULL;
}

/* ---- serving ------------------------------------------------------------------------ */

/* m to its interface's request function. The connection is dead after a
 * protocol error (the function posted it, or we do here). */
static void dispatch(struct comp_client *cl, struct jwl_msg *m)
{
    status_t st = ERR_NOT_SUPPORTED;
    for (unsigned i = 0; i < sizeof(requests) / sizeof(requests[0]); i++)
        if (requests[i].iface == m->iface)
            st = requests[i].fn(cl, m);
    if (st == OK || cl->conn->status != OK)
        return;
    if (st == ERR_NOT_SUPPORTED) {
        jwl_msg_close_handles(m);   /* no handler took them */
        comp_error(cl, m->id, JWL_ERROR_INVALID_METHOD, "%s.%s is not offered", m->iface->name,
                   m->msg->name);
        return;
    }
    comp_error(cl, m->id, JWL_ERROR_IMPLEMENTATION, "%s.%s failed: %s", m->iface->name,
               m->msg->name, status_str(st));
}

static void serve_client(struct comp_client *cl)
{
    uint64_t t0 = now();
    cl->more = false;
    for (unsigned n = 0; n < COMP_BUDGET_MSGS; n++) {
        struct jwl_msg m;
        status_t st = jwl_conn_next(cl->conn, &m);
        if (st == ERR_SHOULD_WAIT) {
            cl->ready = false;
            return;
        }
        if (st != OK)
            return;   /* dead: torn down after the turn */
        dispatch(cl, &m);
        if (cl->conn->status != OK || now() - t0 >= COMP_BUDGET_NS)
            break;
    }
    cl->more = cl->conn->status == OK;
}

static enum comp_gone why_gone(const struct jwl_conn *c)
{
    if (c->status == ERR_PEER_CLOSED || c->status == ERR_BAD_STATE)
        return COMP_GONE_CLOSED;
    /* libjwl's own words when the held bytes overflow (jwl_transport.c) */
    static const char slow[] = "the client reads too slowly";
    return c->error.code == JWL_ERROR_NO_MEMORY && !strncmp(c->error.text, slow, sizeof(slow) - 1)
               ? COMP_GONE_SLOW
               : COMP_GONE_PROTOCOL;
}

static void log_gone(const struct comp_client *cl, enum comp_gone why)
{
    static uint64_t window_start;
    static unsigned lines, quiet;
    uint64_t t = now();
    if (t - window_start >= NS_PER_S) {
        if (quiet)
            printf("compositor: %u more clients gone\n", quiet);
        window_start = t;
        lines = quiet = 0;
    }
    if (lines++ >= LOG_PER_S) {
        quiet++;
        return;
    }
    if (why == COMP_GONE_CLOSED)
        printf("compositor: client %u gone (%s)\n", cl->slot, gone_names[why]);
    else
        printf("compositor: client %u gone (%s): object %u code %u: %s\n", cl->slot,
               gone_names[why], cl->conn->error.object, cl->conn->error.code,
               cl->conn->error.text);
}

/* cl's connection is dead: everything of it goes, in this turn. */
static void client_die(struct comp_client *cl)
{
    enum comp_gone why = why_gone(cl->conn);
    comp.stats.gone[why]++;
    log_gone(cl, why);
    for (unsigned i = 0; i < sizeof(teardowns) / sizeof(teardowns[0]); i++)
        teardowns[i](cl);
    jwl_conn_destroy(cl->conn);   /* our raw duplicate keeps the channel end open */
    cl->conn = NULL;
    signals_t seen = 0;
    (void)jam_object_wait_one(cl->raw, SIG_PEER_CLOSED, 0, &seen);   /* a look, no wait */
    if (seen & SIG_PEER_CLOSED) {
        free_client(cl);
        return;
    }
    /* It may still read what we wrote: keep the end until it closes its own. */
    cl->lingering = true;
    (void)jam_port_unbind(comp.port, cl->raw, 1 + cl->slot);
    if (jam_port_bind(comp.port, cl->raw, 1 + cl->slot, SIG_PEER_CLOSED, PORT_BIND_ONCE) != OK)
        free_client(cl);   /* can't watch it: let it go now */
}

void conn_ready(uint64_t key)
{
    struct comp_client *cl = key >= 1 && key <= COMP_CLIENTS_MAX ? clients[key - 1] : NULL;
    if (!cl)
        return;
    if (!cl->lingering) {
        cl->ready = true;
        return;
    }
    signals_t seen = 0;
    (void)jam_object_wait_one(cl->raw, SIG_PEER_CLOSED, 0, &seen);
    if (seen & SIG_PEER_CLOSED)
        free_client(cl);   /* else a packet of its old binding: still lingering */
}

void conn_serve_all(void)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = clients[i];
        if (cl && cl->conn && (cl->ready || cl->more))
            serve_client(cl);
    }
    conn_flush_all();
}

void conn_flush_all(void)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = clients[i];
        if (!cl || !cl->conn)
            continue;
        if (jwl_conn_flush(cl->conn) != OK)
            client_die(cl);
    }
}

bool conn_more(void)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++)
        if (clients[i] && clients[i]->conn && clients[i]->more)
            return true;
    return false;
}

bool conn_held(void)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        const struct jwl_conn *c = clients[i] ? clients[i]->conn : NULL;
        if (c && c->held && c->nsent - c->peer_acked < JWL_WINDOW)
            return true;   /* the window is open: a full channel held it back */
    }
    return false;
}
