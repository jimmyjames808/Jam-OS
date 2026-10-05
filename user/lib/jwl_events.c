/* libjwl's client: the events (<jwl_client.h>). What the compositor
 * sends to the client's own objects (wl_display's error, the registry's
 * globals, wl_shm's formats, wl_output's mode, wl_seat's capabilities,
 * xdg_wm_base's ping, the callbacks) is taken here; the rest is handed to
 * the files that own the objects (jwl_shm.c, jwl_window.c, jwl_seat.c).
 * What the program is to hear goes into the client's queue, a ring of
 * JWL_EVENT_QUEUE events: pointer motion merges into a motion queued
 * last, a destroyed window's events are blanked, and a full queue drops
 * the event and counts it. */
#include <jwl_client.h>
#include <os.h>
#include "jwlc.h"

/* ---- the event queue ---------------------------------------------------------------- */

void jwlc_queue(struct jwl_client *c, const struct jwl_event *ev)
{
    if (ev->type == JWL_EV_POINTER_MOTION && c->qlen) {
        struct jwl_event *last = &c->q[(c->qhead + c->qlen - 1) % JWL_EVENT_QUEUE];
        if (last->type == JWL_EV_POINTER_MOTION && last->win == ev->win) {
            *last = *ev;   /* only the newest position matters */
            return;
        }
    }
    if (c->qlen == JWL_EVENT_QUEUE) {
        if (c->info.events_dropped++ == 0)
            jwlc_log(c, "the program doesn't take its events: dropping them");
        return;
    }
    c->q[(c->qhead + c->qlen) % JWL_EVENT_QUEUE] = *ev;
    c->qlen++;
}

void jwlc_unqueue(struct jwl_client *c, const struct jwl_window *w)
{
    for (unsigned i = 0; i < c->qlen; i++) {
        struct jwl_event *e = &c->q[(c->qhead + i) % JWL_EVENT_QUEUE];
        if (e->win == w) {
            e->type = JWL_EV_NONE;   /* skipped by jwl_client_next_event */
            e->win = NULL;
        }
    }
}

void jwlc_queue_conn(struct jwl_client *c, uint32_t type, status_t why)
{
    struct jwl_event ev = { .type = type };
    ev.conn.why = why;
    ev.conn.generation = c->info.generation;
    jwlc_queue(c, &ev);
}

status_t jwl_client_next_event(struct jwl_client *c, struct jwl_event *out)
{
    while (c->qlen) {
        struct jwl_event *e = &c->q[c->qhead];
        c->qhead = (c->qhead + 1) % JWL_EVENT_QUEUE;
        c->qlen--;
        if (e->type != JWL_EV_NONE) {
            *out = *e;
            return OK;
        }
    }
    return ERR_SHOULD_WAIT;
}

/* ---- events of the client's own objects ---------------------------------------------- */

static status_t display_error(void *data, uint32_t self, uint32_t object, uint32_t code,
                              const char *text)
{
    struct jwl_client *c = data;
    (void)self;
    c->fatal = ERR_INVALID_ARGS;   /* we close it: read_all */
    c->info.error_object = object;
    c->info.error_code = code;
    snprintf(c->info.error_text, sizeof(c->info.error_text), "%s", text ? text : "");
    jwlc_log(c, "the compositor says object %u broke the protocol (code %u): %s", object, code,
             c->info.error_text);
    return OK;
}

static status_t registry_global(void *data, uint32_t self, uint32_t name, const char *iface,
                                uint32_t version)
{
    struct jwl_client *c = data;
    (void)self;
    for (unsigned i = 0; i < JWLC_GLOBALS; i++)
        if (!strcmp(iface, jwlc_wanted[i].iface->name) && !c->offer[i].name && name) {
            c->offer[i].name = name;
            c->offer[i].version = version;
        }
    return OK;
}

static status_t registry_global_remove(void *data, uint32_t self, uint32_t name)
{
    struct jwl_client *c = data;
    (void)self;
    for (unsigned i = 0; i < JWLC_GLOBALS; i++)
        if (c->offer[i].name == name)
            jwlc_log(c, "the compositor took %s away: kept", jwlc_wanted[i].iface->name);
    return OK;
}

static status_t shm_format(void *data, uint32_t self, uint32_t format)
{
    struct jwl_client *c = data;
    (void)self;
    if (format < 32)
        c->info.shm_formats |= 1u << format;
    return OK;
}

static status_t output_mode(void *data, uint32_t self, uint32_t flags, int32_t w, int32_t h,
                            int32_t refresh)
{
    struct jwl_client *c = data;
    (void)self;
    if (flags & JWL_WL_OUTPUT_MODE_CURRENT) {
        c->info.output_width = w;
        c->info.output_height = h;
        c->info.output_refresh = refresh;
    }
    return OK;
}

static status_t output_scale(void *data, uint32_t self, int32_t factor)
{
    struct jwl_client *c = data;
    (void)self;
    c->info.output_scale = factor;
    return OK;
}

static status_t output_ignored(void *data, uint32_t self)
{
    (void)data;
    (void)self;
    return OK;
}

static status_t output_geometry(void *data, uint32_t self, int32_t x, int32_t y, int32_t pw,
                                int32_t ph, int32_t subpixel, const char *make, const char *model,
                                int32_t transform)
{
    (void)data, (void)self, (void)x, (void)y, (void)pw, (void)ph, (void)subpixel;
    (void)make, (void)model, (void)transform;
    return OK;
}

static status_t seat_capabilities(void *data, uint32_t self, uint32_t caps)
{
    (void)self;
    return jwlc_seat_caps(data, caps);
}

static status_t seat_name(void *data, uint32_t self, const char *name)
{
    (void)data, (void)self, (void)name;
    return OK;
}

static status_t wm_base_ping(void *data, uint32_t self, uint32_t serial)
{
    struct jwl_client *c = data;
    c->info.pings++;
    return jwl_xdg_wm_base_pong(c->conn, self, serial);
}

static const struct jwl_wl_display_events display_events = { .error = display_error };
static const struct jwl_wl_registry_events registry_events = {
    .global = registry_global, .global_remove = registry_global_remove,
};
static const struct jwl_wl_shm_events shm_events = { .format = shm_format };
static const struct jwl_wl_output_events output_events = {
    .geometry = output_geometry, .mode = output_mode, .done = output_ignored,
    .scale = output_scale,
};
static const struct jwl_wl_seat_events seat_events = {
    .capabilities = seat_capabilities, .name = seat_name,
};
static const struct jwl_xdg_wm_base_events wm_base_events = { .ping = wm_base_ping };

/* wl_callback.done, a destructor: the transport has forgotten the
 * callback already; m->data is still whose it was. */
static status_t callback_done(struct jwl_client *c, const struct jwl_msg *m)
{
    struct jwl_callback *cb = m->data;
    (void)c;
    if (!cb || cb->id != m->id)
        return OK;   /* a frame of a window since destroyed, a roundtrip given up */
    cb->id = 0;
    cb->done = true;
    cb->time = m->args[0].u;
    if (cb->win)
        jwlc_window_frame_done(cb->win, cb->time, false);
    return OK;
}

status_t jwlc_handle(struct jwl_client *c, struct jwl_msg *m)
{
    const struct jwl_interface *i = m->iface;
    uint32_t op = m->opcode;
    const union jwl_arg *a = m->args;
    if (i == &jwl_wl_callback_interface)
        return callback_done(c, m);
    if (i == &jwl_wl_display_interface)
        return jwl_wl_display_dispatch_event(&display_events, c, m->id, op, a);
    if (i == &jwl_wl_registry_interface)
        return jwl_wl_registry_dispatch_event(&registry_events, c, m->id, op, a);
    if (i == &jwl_wl_shm_interface)
        return jwl_wl_shm_dispatch_event(&shm_events, c, m->id, op, a);
    if (i == &jwl_wl_output_interface)
        return jwl_wl_output_dispatch_event(&output_events, c, m->id, op, a);
    if (i == &jwl_wl_seat_interface)
        return jwl_wl_seat_dispatch_event(&seat_events, c, m->id, op, a);
    if (i == &jwl_xdg_wm_base_interface)
        return jwl_xdg_wm_base_dispatch_event(&wm_base_events, c, m->id, op, a);
    if (i == &jwl_wl_buffer_interface || i == &jwl_wl_shm_pool_interface)
        return jwlc_shm_event(c, m);
    if (i == &jwl_wl_surface_interface || i == &jwl_xdg_surface_interface ||
        i == &jwl_xdg_toplevel_interface)
        return jwlc_window_event(c, m);
    if (i == &jwl_wl_keyboard_interface || i == &jwl_wl_pointer_interface)
        return jwlc_seat_event(c, m);
    return ERR_NOT_SUPPORTED;
}
