/* xdg-decoration (xdg.h): zxdg_decoration_manager_v1 and each toplevel's
 * zxdg_toplevel_decoration_v1, which decide who draws a window's title bar
 * and frame (docs/G2-PLAN.md, Q4).
 *
 * The policy, for each toplevel:
 *   - with no decoration object, its client draws its own (client_side):
 *     a client that never heard of the protocol (GTK) gets no second title
 *     bar over its own;
 *   - with one, the mode its client asked for (set_mode), or server_side
 *     when it asked for none (unset_mode, or nothing yet): the compositor's
 *     title bar, circles and frame, the look of every Jam OS window
 *     (libjwl asks server_side for each of its windows);
 *   - destroying the object goes back to client_side.
 * mode_for below is the whole policy. The mode is part of the toplevel's
 * configure (wm.h's WM_ST_CSD): a change goes out as a configure, and the
 * frame changes when the client acks it and commits, as a size does.
 * Every set_mode and unset_mode is answered by a configure event naming
 * the mode, then a configure, even when nothing changed, as the protocol
 * asks; a decoration object made before the toplevel's initial commit is
 * told with the first configure.
 *
 * The protocol's errors: a second decoration object for a toplevel
 * (already_constructed), one made for a toplevel with a buffer attached
 * or committed (unconfigured_buffer: version 1, the version offered), a
 * mode that is neither (invalid_mode), and the toplevel destroyed before
 * its decoration object (orphaned, xdgtop.c).
 *
 * Objects' data: a manager holds the client (it keeps nothing; the map
 * bounds how many); a decoration object its toplevel's struct xdg_surf,
 * which keeps the object's state (deco_*), so nothing here is allocated
 * and a client's teardown has nothing of ours to free. */
#include <jwl/wayland.h>
#include <jwl/xdg_decoration_unstable_v1.h>
#include <jwl/xdg_shell.h>
#include "xdg.h"

#define CLIENT_SIDE JWL_ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
#define SERVER_SIDE JWL_ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE

/* The mode a toplevel with a decoration object gets for what its client
 * asked (0: nothing). */
static uint32_t mode_for(uint32_t asked)
{
    return asked == CLIENT_SIDE ? CLIENT_SIDE : SERVER_SIDE;
}

bool xdg_deco_owed(const struct xdg_surf *x)
{
    return x->deco_id && !x->deco_told;
}

status_t xdg_deco_send(struct xdg_surf *x)
{
    if (!xdg_deco_owed(x))
        return OK;
    x->deco_told = mode_for(x->deco_asked);
    return jwl_zxdg_toplevel_decoration_v1_send_configure(x->cl->conn, x->deco_id, x->deco_told);
}

/* x's mode changed or was asked about: the window manager's next configure
 * carries it (wm_reconfigure; before the initial commit, the initial one). */
static void decide(struct xdg_surf *x)
{
    x->deco_told = 0;
    if (!x->ww)
        return;   /* inert: its wl_surface went */
    x->ww->csd = !x->deco_id || mode_for(x->deco_asked) == CLIENT_SIDE;
    wm_reconfigure(x->ww);
}

/* ---- zxdg_decoration_manager_v1 --------------------------------------------------------- */

status_t xdg_deco_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    return jwl_map_set_data(&cl->conn->map, id, cl);
}

static status_t on_manager_destroy(void *data, uint32_t self)
{
    (void)data, (void)self;
    return OK;   /* libjwl freed the id; the decoration objects it made stay */
}

static status_t on_get_toplevel_decoration(void *data, uint32_t self, uint32_t id,
                                           uint32_t toplevel)
{
    struct comp_client *cl = data;
    (void)self;
    struct xdg_surf *x = comp_object(cl, toplevel, &jwl_xdg_toplevel_interface);
    if (!x)
        return comp_error(cl, id, JWL_ERROR_INVALID_OBJECT, "no xdg_toplevel %u", toplevel);
    if (x->deco_id)
        return comp_error(cl, id, JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                          "xdg_toplevel %u has a decoration object already", toplevel);
    if (x->surface && (x->surface->buffer || x->surface->pending.buffer))
        return comp_error(cl, id, JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                          "xdg_toplevel %u has a buffer already", toplevel);
    status_t st = jwl_map_set_data(&cl->conn->map, id, x);
    if (st != OK)
        return st;
    x->deco_id = id;
    x->deco_asked = 0;
    decide(x);
    return OK;
}

static const struct jwl_zxdg_decoration_manager_v1_requests manager_ops = {
    .destroy = on_manager_destroy,
    .get_toplevel_decoration = on_get_toplevel_decoration,
};

status_t xdg_deco_manager_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_zxdg_decoration_manager_v1_dispatch_request(&manager_ops, m->data, m->id,
                                                          m->opcode, m->args);
}

/* ---- zxdg_toplevel_decoration_v1 -------------------------------------------------------- */

static status_t on_deco_destroy(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    x->deco_id = 0;   /* libjwl freed the id */
    x->deco_asked = 0;
    decide(x);        /* back to the client's own, from its next configure */
    return OK;
}

static status_t on_set_mode(void *data, uint32_t self, uint32_t mode)
{
    struct xdg_surf *x = data;
    if (mode != CLIENT_SIDE && mode != SERVER_SIDE)
        return comp_error(x->cl, self, JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
                          "decoration mode %u", mode);
    x->deco_asked = mode;
    decide(x);
    return OK;
}

static status_t on_unset_mode(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    (void)self;
    x->deco_asked = 0;
    decide(x);
    return OK;
}

static const struct jwl_zxdg_toplevel_decoration_v1_requests deco_ops = {
    .destroy = on_deco_destroy,
    .set_mode = on_set_mode,
    .unset_mode = on_unset_mode,
};

status_t xdg_deco_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_zxdg_toplevel_decoration_v1_dispatch_request(&deco_ops, m->data, m->id,
                                                           m->opcode, m->args);
}
