/* xdg-shell's global and its small objects (xdg.h): xdg_wm_base (bind,
 * get_xdg_surface, create_positioner, ping and pong), xdg_positioner,
 * each client's xdg-shell state and its teardown. Popups are xdgpopup.c's.
 *
 * A positioner keeps its rules (struct xdg_rules), checked as the protocol
 * says, each number held to +-XDG_PLACE_MAX; get_popup and reposition copy
 * them, so the positioner may change or go after. set_reactive is kept
 * (the popup is placed again when its parent moves); set_parent_size and
 * set_parent_configure are accepted and kept nowhere: a popup is placed
 * against its parent as it is when the popup is configured.
 *
 * Pings. A close request (the close circle, Super+Q) pings the client's xdg_wm_base;
 * a client that hasn't answered within XDG_PING_NS has its windows marked
 * not responding (the title bar says so) until the pong comes. Nothing
 * more: the compositor holds no job of anyone's, and the shell can kill. */
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include "xdg.h"

/* ---- each client's state ------------------------------------------------------------ */

struct xdg_client *xdg_client_of(struct comp_client *cl)
{
    struct xdg_client *xc = cl->xdg;
    if (!xc && (xc = calloc(1, sizeof(*xc)))) {
        xc->cl = cl;
        cl->xdg = xc;
    }
    return xc;
}

void xdg_surf_free(struct xdg_surf *x)
{
    struct xdg_client *xc = x->xc;
    for (struct xdg_surf **p = &xc->surfs; *p; p = &(*p)->next) {
        if (*p == x) {
            *p = x->next;
            break;
        }
    }
    xc->nsurfs--;
    if (x->base)
        x->base->nsurfaces--;
    free(x);
}

static void base_free(struct xdg_client *xc, struct xdg_base *b)
{
    for (struct xdg_base **p = &xc->bases; *p; p = &(*p)->next) {
        if (*p == b) {
            *p = b->next;
            break;
        }
    }
    xc->nbases--;
    free(b);
}

static void pos_free(struct xdg_pos *p)
{
    struct xdg_client *xc = p->xc;
    for (struct xdg_pos **q = &xc->positioners; *q; q = &(*q)->next) {
        if (*q == p) {
            *q = p->next;
            break;
        }
    }
    xc->npositioners--;
    free(p);
}

void xdg_teardown(struct comp_client *cl)
{
    struct xdg_client *xc = cl->xdg;
    if (!xc)
        return;
    while (xc->surfs) {
        struct xdg_surf *x = xc->surfs;
        xdg_surf_detach(x);
        xdg_surf_free(x);
    }
    while (xc->positioners)
        pos_free(xc->positioners);
    while (xc->bases)
        base_free(xc, xc->bases);
    free(xc);
    cl->xdg = NULL;
}

/* ---- pings -------------------------------------------------------------------------- */

void xdg_ping(struct xdg_surf *x)
{
    struct xdg_base *b = x->base;
    if (!b || b->ping)
        return;
    b->ping = comp_serial();
    b->ping_ns = now();
    (void)jwl_xdg_wm_base_send_ping(x->cl->conn, b->id, b->ping);   /* dead: torn down after */
}

/* Every toplevel of xc's marked responding or not. */
static void mark(struct xdg_client *xc, bool not_responding)
{
    for (struct xdg_surf *x = xc->surfs; x; x = x->next)
        if (x->ww)
            wm_set_not_responding(x->ww, not_responding);
}

void xdg_tick(uint64_t t)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = conn_client_at(i);
        struct xdg_client *xc = cl ? cl->xdg : NULL;
        for (struct xdg_base *b = xc ? xc->bases : NULL; b; b = b->next) {
            if (b->ping && !b->late && t - b->ping_ns >= XDG_PING_NS) {
                b->late = true;
                mark(xc, true);
            }
        }
    }
}

uint64_t xdg_deadline(void)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = conn_client_at(i);
        struct xdg_client *xc = cl ? cl->xdg : NULL;
        for (struct xdg_base *b = xc ? xc->bases : NULL; b; b = b->next)
            if (b->ping && !b->late && b->ping_ns + XDG_PING_NS < next)
                next = b->ping_ns + XDG_PING_NS;
    }
    return next;
}

/* ---- xdg_wm_base -------------------------------------------------------------------- */

status_t xdg_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    struct xdg_client *xc = xdg_client_of(cl);
    if (!xc)
        return comp_no_memory(cl, id, "no memory for xdg-shell");
    if (xc->nbases >= XDG_BASES_MAX)
        return comp_no_memory(cl, id, "too many xdg_wm_base objects (16)");
    struct xdg_base *b = calloc(1, sizeof(*b));
    if (!b)
        return comp_no_memory(cl, id, "no memory for an xdg_wm_base");
    b->xc = xc;
    b->id = id;
    b->next = xc->bases;
    xc->bases = b;
    xc->nbases++;
    return jwl_map_set_data(&cl->conn->map, id, b);
}

static status_t on_base_destroy(void *data, uint32_t self)
{
    struct xdg_base *b = data;
    if (b->nsurfaces)
        return comp_error(b->xc->cl, self, JWL_XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                          "xdg_wm_base %u destroyed with %u xdg_surfaces", self, b->nsurfaces);
    base_free(b->xc, b);   /* libjwl freed the id */
    return OK;
}

static status_t on_create_positioner(void *data, uint32_t self, uint32_t id)
{
    struct xdg_base *b = data;
    struct xdg_client *xc = b->xc;
    if (xc->npositioners >= XDG_POSITIONERS_MAX)
        return comp_no_memory(xc->cl, self, "too many xdg_positioners (64)");
    struct xdg_pos *p = calloc(1, sizeof(*p));
    if (!p)
        return comp_no_memory(xc->cl, self, "no memory for an xdg_positioner");
    p->xc = xc;
    p->id = id;
    p->next = xc->positioners;
    xc->positioners = p;
    xc->npositioners++;
    return jwl_map_set_data(&xc->cl->conn->map, id, p);
}

static status_t on_get_xdg_surface(void *data, uint32_t self, uint32_t id, uint32_t surface)
{
    struct xdg_base *b = data;
    (void)self;
    return xdg_surf_create(b->xc->cl, b, id, surface);
}

static status_t on_pong(void *data, uint32_t self, uint32_t serial)
{
    struct xdg_base *b = data;
    (void)self;
    if (!b->ping || serial != b->ping)
        return OK;   /* an old or made-up serial: nothing to answer */
    b->ping = 0;
    b->late = false;
    mark(b->xc, false);
    return OK;
}

static const struct jwl_xdg_wm_base_requests base_ops = {
    .destroy = on_base_destroy,
    .create_positioner = on_create_positioner,
    .get_xdg_surface = on_get_xdg_surface,
    .pong = on_pong,
};

status_t xdg_wm_base_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data has it */
    return jwl_xdg_wm_base_dispatch_request(&base_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- xdg_positioner ----------------------------------------------------------------- */

static status_t bad_input(const struct xdg_pos *p, const char *what, int64_t v)
{
    return comp_error(p->xc->cl, p->id, JWL_XDG_POSITIONER_ERROR_INVALID_INPUT, "%s %lld", what,
                      (long long)v);
}

static status_t on_pos_destroy(void *data, uint32_t self)
{
    (void)self;
    pos_free(data);   /* libjwl freed the id */
    return OK;
}

/* v held to +-XDG_PLACE_MAX: a size or place past it means nothing more on
 * an output of at most COMP_BUFFER_SIDE_MAX, and the sums of a few stay
 * far from overflowing (xdgplace.c). */
static int32_t held(int32_t v)
{
    return v < -XDG_PLACE_MAX ? -XDG_PLACE_MAX : v > XDG_PLACE_MAX ? XDG_PLACE_MAX : v;
}

static status_t on_set_size(void *data, uint32_t self, int32_t w, int32_t h)
{
    struct xdg_pos *p = data;
    (void)self;
    if (w < 1 || h < 1)
        return bad_input(p, "size", w < 1 ? w : h);
    p->rules.w = held(w);
    p->rules.h = held(h);
    p->rules.size_set = true;
    return OK;
}

static status_t on_set_anchor_rect(void *data, uint32_t self, int32_t x, int32_t y, int32_t w,
                                   int32_t h)
{
    struct xdg_pos *p = data;
    (void)self;
    if (w < 0 || h < 0)
        return bad_input(p, "anchor rect side", w < 0 ? w : h);
    p->rules.ax = held(x);
    p->rules.ay = held(y);
    p->rules.aw = held(w);
    p->rules.ah = held(h);
    p->rules.rect_set = true;
    return OK;
}

static status_t on_set_anchor(void *data, uint32_t self, uint32_t anchor)
{
    struct xdg_pos *p = data;
    (void)self;
    if (anchor > JWL_XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT)
        return bad_input(p, "anchor", anchor);
    p->rules.anchor = anchor;
    return OK;
}

static status_t on_set_gravity(void *data, uint32_t self, uint32_t gravity)
{
    struct xdg_pos *p = data;
    (void)self;
    if (gravity > JWL_XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)
        return bad_input(p, "gravity", gravity);
    p->rules.gravity = gravity;
    return OK;
}

static status_t on_set_constraint_adjustment(void *data, uint32_t self, uint32_t bits)
{
    struct xdg_pos *p = data;
    (void)self;
    uint32_t all = JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y * 2 - 1;
    if (bits & ~all)
        return bad_input(p, "constraint adjustment", bits);
    p->rules.adjust = bits;
    return OK;
}

static status_t on_set_offset(void *data, uint32_t self, int32_t x, int32_t y)
{
    struct xdg_pos *p = data;
    (void)self;
    p->rules.dx = held(x);
    p->rules.dy = held(y);
    return OK;
}

static status_t on_set_reactive(void *data, uint32_t self)
{
    struct xdg_pos *p = data;
    (void)self;
    p->rules.reactive = true;
    return OK;
}

static status_t on_set_parent_size(void *data, uint32_t self, int32_t w, int32_t h)
{
    (void)data, (void)self, (void)w, (void)h;
    return OK;   /* a hint for placing it against a parent not drawn yet */
}

static status_t on_set_parent_configure(void *data, uint32_t self, uint32_t serial)
{
    (void)data, (void)self, (void)serial;
    return OK;
}

static const struct jwl_xdg_positioner_requests pos_ops = {
    .destroy = on_pos_destroy,
    .set_size = on_set_size,
    .set_anchor_rect = on_set_anchor_rect,
    .set_anchor = on_set_anchor,
    .set_gravity = on_set_gravity,
    .set_constraint_adjustment = on_set_constraint_adjustment,
    .set_offset = on_set_offset,
    .set_reactive = on_set_reactive,
    .set_parent_size = on_set_parent_size,
    .set_parent_configure = on_set_parent_configure,
};

status_t xdg_positioner_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data has it */
    return jwl_xdg_positioner_dispatch_request(&pos_ops, m->data, m->id, m->opcode, m->args);
}

const struct xdg_rules *xdg_rules_of(struct xdg_surf *x, uint32_t positioner)
{
    struct xdg_pos *p = comp_object(x->cl, positioner, &jwl_xdg_positioner_interface);
    if (!p || !p->rules.size_set || !p->rules.rect_set) {
        (void)comp_error(x->cl, x->base->id, JWL_XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                         "xdg_positioner %u has no size or anchor rect", positioner);
        return NULL;
    }
    return &p->rules;
}

