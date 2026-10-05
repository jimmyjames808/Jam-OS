/* xdg_surface and xdg_toplevel (xdg.h): the toplevel role, its configures
 * and its commits, between the client and the window manager (wm.h).
 *
 * The sequence the protocol asks for, and what each step does here:
 *   1. get_xdg_surface, get_toplevel, then the initial commit, with no
 *      buffer (one attached before the first configure was acked is the
 *      unconfigured_buffer error): the window manager's configure goes out
 *      (xdg_toplevel.configure, then xdg_surface.configure with a serial).
 *   2. ack_configure(serial): a serial we sent and the client hasn't
 *      acked; anything else is invalid_serial. It acks the older ones too.
 *   3. A commit with a buffer: the toplevel is mapped, placed for the
 *      states of the newest configure acked before it.
 *   4. A commit with no buffer unmaps it: back to step 1.
 * Configures are sent when the window manager's wish changes (repeats
 * dropped), at most XDG_CONFIGS_MAX unacked: past that the newest wish
 * waits for an ack, so a client that never acks costs a few bytes, not a
 * queue.
 *
 * Min and max size and the window geometry are double-buffered: checked
 * and applied at commit. Title and app id apply at once. */
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include "xdg.h"

_Static_assert(WM_EDGE_TOP == JWL_XDG_TOPLEVEL_RESIZE_EDGE_TOP &&
               WM_EDGE_BOTTOM == JWL_XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM &&
               WM_EDGE_LEFT == JWL_XDG_TOPLEVEL_RESIZE_EDGE_LEFT &&
               WM_EDGE_RIGHT == JWL_XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
               "the window manager's edges are xdg_toplevel's");

static const struct comp_role_ops toplevel_ops;

static struct jwl_conn *conn_of(const struct xdg_surf *x)
{
    return x->cl->conn;
}

static struct comp_client *client_of(const struct xdg_surf *x)
{
    return x->cl;
}

/* ---- configures --------------------------------------------------------------------- */

static bool same(const struct wm_config *a, const struct wm_config *b)
{
    return a->width == b->width && a->height == b->height && a->states == b->states;
}

static void send(struct xdg_surf *x, const struct wm_config *cfg)
{
    static const struct { uint32_t bit, state; } map[] = {
        { WM_ST_MAXIMIZED, JWL_XDG_TOPLEVEL_STATE_MAXIMIZED },
        { WM_ST_FULLSCREEN, JWL_XDG_TOPLEVEL_STATE_FULLSCREEN },
        { WM_ST_RESIZING, JWL_XDG_TOPLEVEL_STATE_RESIZING },
        { WM_ST_ACTIVATED, JWL_XDG_TOPLEVEL_STATE_ACTIVATED },
    };
    uint32_t states[4], n = 0;
    for (unsigned i = 0; i < 4; i++)
        if (cfg->states & map[i].bit)
            states[n++] = map[i].state;
    uint32_t serial = comp_serial();
    status_t st = jwl_xdg_toplevel_send_configure(conn_of(x), x->role_id, cfg->width,
                                                  cfg->height, n ? states : NULL, n * 4);
    if (st == OK)
        st = jwl_xdg_surface_send_configure(conn_of(x), x->id, serial);
    if (st != OK)
        return;   /* a dead connection: torn down after this turn */
    x->sent[x->nsent++] = (struct xdg_sent){ serial, *cfg, false };
    x->last = *cfg;
    x->has_last = true;
    x->dirty = false;
}

/* The window manager's wish, sent now if the client may hear it. */
static void op_configure(void *ctx, const struct wm_config *cfg)
{
    struct xdg_surf *x = ctx;
    x->wanted = *cfg;
    if (!x->initial || !x->surface || !x->role_id)
        return;   /* the initial commit sends it */
    if (x->has_last && same(cfg, &x->last)) {
        x->dirty = false;
        return;
    }
    if (x->nsent == XDG_CONFIGS_MAX) {
        x->dirty = true;
        return;
    }
    send(x, cfg);
}

static void op_close(void *ctx)
{
    struct xdg_surf *x = ctx;
    if (!x->surface || !x->role_id)
        return;
    (void)jwl_xdg_toplevel_send_close(conn_of(x), x->role_id);   /* dead: torn down after */
    xdg_ping(x);
}

static const struct wm_ops ops = { .configure = op_configure, .close = op_close };

/* ---- the roles' commits --------------------------------------------------------------- */

static status_t bare_commit(struct comp_surface *s)
{
    struct xdg_surf *x = s->role_data;
    return comp_error(s->client, x->id, JWL_XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                      "a commit on xdg_surface %u, which has no role", x->id);
}

static void bare_gone(struct comp_surface *s, bool dead)
{
    struct xdg_surf *x = s->role_data;
    (void)dead;
    x->surface = NULL;   /* inert */
}

const struct comp_role_ops xdg_bare_ops = { "xdg_surface", bare_commit, bare_gone };

/* The double-buffered state, checked and applied. */
static status_t apply_pending(struct xdg_surf *x)
{
    if (x->geom_set)
        x->geom = x->geom_pending;
    x->geom_set = false;
    if (!x->limits_set)
        return OK;
    x->limits_set = false;
    if ((x->max_w && x->min_w > x->max_w) || (x->max_h && x->min_h > x->max_h))
        return comp_error(client_of(x), x->role_id, JWL_XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                          "max size %dx%d under min size %dx%d", x->max_w, x->max_h, x->min_w,
                          x->min_h);
    wm_set_limits(x->ww, x->min_w, x->min_h, x->max_w, x->max_h);
    return OK;
}

/* The initial commit: the first configure goes. */
static status_t initial_commit(struct xdg_surf *x)
{
    if (x->surface->buffer)
        return comp_error(client_of(x), x->id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "a buffer on xdg_surface %u before its first configure", x->id);
    x->initial = true;
    x->configured = false;
    x->has_last = false;   /* sent even if it says what an earlier one did */
    if (x->nsent == XDG_CONFIGS_MAX) {   /* the client must hear this one: the oldest goes */
        x->nsent--;
        memmove(x->sent, x->sent + 1, x->nsent * sizeof(x->sent[0]));
    }
    wm_reconfigure(x->ww);
    return OK;
}

/* A commit without a buffer on a mapped toplevel: unmapped, and the
 * configures in flight can't map it again. */
static void unmap(struct xdg_surf *x)
{
    wm_unmap(x->ww);
    x->initial = x->configured = x->acked_new = false;
    x->shown = 0;
    for (uint32_t i = 0; i < x->nsent; i++)
        x->sent[i].stale = true;
}

static status_t toplevel_commit(struct comp_surface *s)
{
    struct xdg_surf *x = s->role_data;
    status_t st = apply_pending(x);
    if (st != OK)
        return st;
    if (!x->initial)
        return initial_commit(x);
    if (!s->buffer) {
        if (x->ww->win)
            unmap(x);
        return OK;
    }
    if (!x->configured)
        return comp_error(s->client, x->id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "a buffer on xdg_surface %u before a configure was acked", x->id);
    if (x->acked_new) {
        x->shown = x->acked.states;
        x->acked_new = false;
    }
    if (wm_commit(x->ww, x->shown) != OK)
        return comp_no_memory(s->client, x->role_id, "no memory for a window");
    return OK;
}

/* The wl_surface went with its toplevel alive: wl_surface's
 * defunct_role_object (wayland.xml), unless the whole client went. */
static void toplevel_gone(struct comp_surface *s, bool dead)
{
    struct xdg_surf *x = s->role_data;
    if (x->ww)
        wm_destroy(x->ww);
    x->ww = NULL;
    x->surface = NULL;   /* inert, with its toplevel */
    if (!dead)
        (void)comp_error(x->cl, s->id, JWL_WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                         "wl_surface %u destroyed before its xdg_toplevel", s->id);
}

static const struct comp_role_ops toplevel_ops = { "xdg_toplevel", toplevel_commit,
                                                   toplevel_gone };

/* ---- xdg_surface ------------------------------------------------------------------- */

status_t xdg_surf_create(struct comp_client *cl, struct xdg_base *b, uint32_t id,
                         uint32_t surface)
{
    struct xdg_client *xc = b->xc;
    struct comp_surface *s = comp_object(cl, surface, &jwl_wl_surface_interface);
    if (!s)
        return comp_error(cl, b->id, JWL_ERROR_INVALID_OBJECT, "no wl_surface %u", surface);
    if (xc->nsurfs >= COMP_SURFACES_MAX)
        return comp_no_memory(cl, id, "too many xdg_surfaces (64)");
    if (s->role_ops || s->role == COMP_ROLE_CURSOR)
        return comp_error(cl, b->id, JWL_XDG_WM_BASE_ERROR_ROLE,
                          "wl_surface %u has another role", surface);
    if (s->buffer || s->pending.buffer)
        return comp_error(cl, id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "wl_surface %u has a buffer already", surface);
    struct xdg_surf *x = calloc(1, sizeof(*x));
    if (!x)
        return comp_no_memory(cl, id, "no memory for an xdg_surface");
    x->cl = cl;
    x->xc = xc;
    x->base = b;
    x->id = id;
    x->surface = s;
    x->next = xc->surfs;
    xc->surfs = x;
    xc->nsurfs++;
    b->nsurfaces++;
    s->role_ops = &xdg_bare_ops;
    s->role_data = x;
    return jwl_map_set_data(&cl->conn->map, id, x);
}

void xdg_surf_detach(struct xdg_surf *x)
{
    if (x->ww)
        wm_destroy(x->ww);
    x->ww = NULL;
    if (x->surface) {
        x->surface->role_ops = NULL;
        x->surface->role_data = NULL;
    }
    x->surface = NULL;
}

static status_t on_xs_destroy(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    if (x->role_id)
        return comp_error(x->cl, self, JWL_XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                          "xdg_surface %u destroyed before its role object", self);
    xdg_surf_detach(x);   /* the role object, if any, is gone already */
    xdg_surf_free(x);     /* libjwl freed the id */
    return OK;
}

/* A role may be given to x's surface: OK, or the protocol error posted. */
static status_t role_ok(struct xdg_surf *x, enum comp_role role)
{
    struct comp_surface *s = x->surface;
    if (x->constructed)
        return comp_error(s->client, x->id, JWL_XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                          "xdg_surface %u has a role object already", x->id);
    if (s->role != COMP_ROLE_NONE && s->role != role)
        return comp_error(s->client, x->base->id, JWL_XDG_WM_BASE_ERROR_ROLE,
                          "wl_surface %u had another role", s->id);
    if (s->buffer)
        return comp_error(s->client, x->id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "a buffer on xdg_surface %u before its role", x->id);
    return OK;
}

static status_t on_get_toplevel(void *data, uint32_t self, uint32_t id)
{
    struct xdg_surf *x = data;
    if (!x->surface)
        return ERR_BAD_STATE;   /* its wl_surface went: an implementation error */
    status_t st = role_ok(x, COMP_ROLE_XDG_TOPLEVEL);
    if (st != OK)
        return st;
    struct comp_surface *s = x->surface;
    x->ww = wm_create(s, &ops, x);
    if (!x->ww)
        return comp_no_memory(s->client, self, "no memory for a toplevel");
    x->constructed = true;
    x->role = XDG_ROLE_TOPLEVEL;
    x->role_id = id;
    s->role = COMP_ROLE_XDG_TOPLEVEL;
    s->role_ops = &toplevel_ops;
    return jwl_map_set_data(&x->cl->conn->map, id, x);
}

static status_t on_get_popup(void *data, uint32_t self, uint32_t id, uint32_t parent,
                             uint32_t positioner)
{
    struct xdg_surf *x = data;
    (void)self;
    if (!x->surface)
        return ERR_BAD_STATE;
    status_t st = role_ok(x, COMP_ROLE_XDG_POPUP);
    return st == OK ? xdg_popup_create(x, id, parent, positioner) : st;
}

/* "A role must be assigned before any other requests are made to the
 * xdg_surface object" (xdg-shell.xml). */
static status_t not_constructed(struct xdg_surf *x, const char *what)
{
    return comp_error(x->cl, x->id, JWL_XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                      "%s on xdg_surface %u before its role", what, x->id);
}

static status_t on_set_window_geometry(void *data, uint32_t self, int32_t gx, int32_t gy,
                                       int32_t w, int32_t h)
{
    struct xdg_surf *x = data;
    if (!x->surface)
        return OK;
    if (!x->constructed)
        return not_constructed(x, "set_window_geometry");
    if (w <= 0 || h <= 0)
        return comp_error(client_of(x), self, JWL_XDG_SURFACE_ERROR_INVALID_SIZE,
                          "window geometry %dx%d", w, h);
    x->geom_pending = box_make(gx, gy, w, h);
    x->geom_set = true;
    return OK;
}

static status_t on_ack_configure(void *data, uint32_t self, uint32_t serial)
{
    struct xdg_surf *x = data;
    if (!x->surface)
        return OK;
    if (!x->constructed)
        return not_constructed(x, "ack_configure");
    uint32_t i = 0;
    while (i < x->nsent && x->sent[i].serial != serial)
        i++;
    if (i == x->nsent)
        return comp_error(client_of(x), self, JWL_XDG_SURFACE_ERROR_INVALID_SERIAL,
                          "no configure %u to ack", serial);
    if (!x->sent[i].stale) {
        x->acked = x->sent[i].cfg;
        x->acked_new = x->configured = true;
    }
    x->nsent -= i + 1;   /* it and every older one */
    memmove(x->sent, x->sent + i + 1, x->nsent * sizeof(x->sent[0]));
    if (x->dirty && x->initial)
        op_configure(x, &x->wanted);
    return OK;
}

static const struct jwl_xdg_surface_requests xs_ops = {
    .destroy = on_xs_destroy,
    .get_toplevel = on_get_toplevel,
    .get_popup = on_get_popup,
    .set_window_geometry = on_set_window_geometry,
    .ack_configure = on_ack_configure,
};

status_t xdg_surface_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_xdg_surface_dispatch_request(&xs_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- xdg_toplevel ------------------------------------------------------------------ */

static status_t on_top_destroy(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_destroy(x->ww);
    x->ww = NULL;
    x->role_id = 0;   /* libjwl freed the id */
    x->initial = x->configured = x->acked_new = x->dirty = false;
    x->nsent = 0;
    if (x->surface)
        x->surface->role_ops = &xdg_bare_ops;   /* its role stays a toplevel's */
    return OK;
}

static status_t on_set_parent(void *data, uint32_t self, uint32_t parent)
{
    struct xdg_surf *x = data;
    if (x->surface && parent == self)
        return comp_error(client_of(x), self, JWL_XDG_TOPLEVEL_ERROR_INVALID_PARENT,
                          "a toplevel can't be its own parent");
    return OK;   /* kept nowhere: G1 places no window by its parent */
}

static status_t on_set_title(void *data, uint32_t self, const char *title)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_set_title(x->ww, title);
    return OK;
}

static status_t on_set_app_id(void *data, uint32_t self, const char *app_id)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_set_app_id(x->ww, app_id);
    return OK;
}

static status_t on_show_window_menu(void *data, uint32_t self, uint32_t seat, uint32_t serial,
                                    int32_t mx, int32_t my)
{
    (void)data, (void)self, (void)seat, (void)serial, (void)mx, (void)my;
    return OK;   /* no window menu in G1: ignored, as the protocol allows */
}

/* move and resize: only for a press the seat says is the client's and
 * still held, from where the pointer is now. Anything else is ignored, as
 * the protocol allows. */
static status_t grab_for(struct xdg_surf *x, uint32_t serial, uint32_t edges)
{
    if (!x->ww || !x->ww->win || !seat_button_serial_ok(client_of(x), serial))
        return OK;
    if (edges)
        (void)wm_begin_resize(x->ww->win, edges, cursor.x, cursor.y);   /* refused: nothing */
    else
        (void)wm_begin_move(x->ww->win, cursor.x, cursor.y);
    return OK;
}

static status_t on_move(void *data, uint32_t self, uint32_t seat, uint32_t serial)
{
    (void)self, (void)seat;
    return grab_for(data, serial, 0);
}

static status_t on_resize(void *data, uint32_t self, uint32_t seat, uint32_t serial,
                          uint32_t edges)
{
    struct xdg_surf *x = data;
    (void)seat;
    bool ok = edges <= JWL_XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT && edges != 3 && edges != 7;
    if (x->surface && !ok)
        return comp_error(client_of(x), self, JWL_XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
                          "resize edge %u", edges);
    return edges ? grab_for(x, serial, edges) : OK;
}

static status_t limits(struct xdg_surf *x, uint32_t self, int32_t w, int32_t h, bool max)
{
    if (!x->surface)
        return OK;
    if (w < 0 || h < 0)
        return comp_error(client_of(x), self, JWL_XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                          "%s size %dx%d", max ? "max" : "min", w, h);
    *(max ? &x->max_w : &x->min_w) = w;
    *(max ? &x->max_h : &x->min_h) = h;
    x->limits_set = true;
    return OK;
}

static status_t on_set_max_size(void *data, uint32_t self, int32_t w, int32_t h)
{
    return limits(data, self, w, h, true);
}

static status_t on_set_min_size(void *data, uint32_t self, int32_t w, int32_t h)
{
    return limits(data, self, w, h, false);
}

static status_t on_set_maximized(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_request_maximized(x->ww, true);
    return OK;
}

static status_t on_unset_maximized(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_request_maximized(x->ww, false);
    return OK;
}

static status_t on_set_fullscreen(void *data, uint32_t self, uint32_t output)
{
    struct xdg_surf *x = data;
    (void)self, (void)output;   /* one output */
    if (x->ww)
        wm_request_fullscreen(x->ww, true);
    return OK;
}

static status_t on_unset_fullscreen(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    if (x->ww)
        wm_request_fullscreen(x->ww, false);
    return OK;
}

static status_t on_set_minimized(void *data, uint32_t self)
{
    (void)data, (void)self;
    return OK;   /* no minimising in G1: ignored, as the protocol allows */
}

static const struct jwl_xdg_toplevel_requests top_ops = {
    .destroy = on_top_destroy,
    .set_parent = on_set_parent,
    .set_title = on_set_title,
    .set_app_id = on_set_app_id,
    .show_window_menu = on_show_window_menu,
    .move = on_move,
    .resize = on_resize,
    .set_max_size = on_set_max_size,
    .set_min_size = on_set_min_size,
    .set_maximized = on_set_maximized,
    .unset_maximized = on_unset_maximized,
    .set_fullscreen = on_set_fullscreen,
    .unset_fullscreen = on_unset_fullscreen,
    .set_minimized = on_set_minimized,
};

status_t xdg_toplevel_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_xdg_toplevel_dispatch_request(&top_ops, m->data, m->id, m->opcode, m->args);
}
