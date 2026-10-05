/* wl_surface (comp.h): pending state, commit, frame callbacks.
 *
 * Requests change only the pending state (struct comp_state); commit
 * applies all of it at once, as the protocol's double-buffering says: the
 * new buffer (the old one released when nothing shows it any more), the
 * damage (onto the output, where the surface's window is), the opaque and
 * input regions, the frame callbacks; then the role, if any, sees the
 * result (xdg_toplevel maps, configures, places). A surface without a role
 * shows nothing.
 *
 * Frame callbacks are answered (wl_callback.done with the paint's time in
 * ms) after the first paint following their commit while the surface is
 * visible; while it isn't, at most once every COMP_HIDDEN_FRAME_NS, so a
 * hidden animation slows down rather than spinning or stopping.
 *
 * Only scale 1 and the normal transform: anything else is a protocol
 * error (wl_surface's invalid_scale or invalid_transform for values the
 * protocol forbids, `implementation` for the rest), never a wrong picture.
 * At scale 1 and the normal transform buffer and surface coordinates are
 * the same, so damage and damage_buffer share one list. */
#include <jwl/wayland.h>
#include "comp.h"

#define TRANSFORM_MAX 7   /* wl_output.transform's last value (flipped_270) */

/* ---- making and losing surfaces ---------------------------------------------------- */

status_t surface_create(struct comp_client *cl, uint32_t id)
{
    if (cl->nsurfaces >= COMP_SURFACES_MAX)
        return comp_no_memory(cl, id, "too many surfaces (64)");
    struct comp_surface *s = calloc(1, sizeof(*s));
    if (!s)
        return comp_no_memory(cl, id, "no memory for a surface");
    s->client = cl;
    s->id = id;
    s->input_all = s->pending.input_all = true;
    damage_init(&s->pending.damage, s->pending.damage_boxes, COMP_SURFACE_DAMAGE_MAX);
    region_init(&s->pending.opaque, &cl->rects);
    region_init(&s->pending.input, &cl->rects);
    region_init(&s->opaque, &cl->rects);
    region_init(&s->input, &cl->rects);
    s->next = cl->surfaces;
    cl->surfaces = s;
    cl->nsurfaces++;
    return jwl_map_set_data(&cl->conn->map, id, s);
}

/* s goes. dead: its client went, so nothing is sent. */
static void surface_free(struct comp_surface *s, bool dead)
{
    struct comp_client *cl = s->client;
    if (s->role_ops && s->role_ops->gone)
        s->role_ops->gone(s, dead);
    if (s->window)
        window_destroy(s->window);
    for (uint32_t i = 0; !dead && i < s->nframes + s->npending; i++)
        (void)jwl_conn_delete(cl->conn, s->frames[i]);   /* never answered: just gone */
    if (s->buffer) {
        buffer_unshow(s->buffer);
        buffer_unref(s->buffer);
    }
    if (s->pending.buffer)
        buffer_unref(s->pending.buffer);
    region_fini(&s->pending.opaque);
    region_fini(&s->pending.input);
    region_fini(&s->opaque);
    region_fini(&s->input);
    for (struct comp_surface **p = &cl->surfaces; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    cl->nsurfaces--;
    free(s);
}

void surfaces_teardown(struct comp_client *cl)
{
    while (cl->surfaces)
        surface_free(cl->surfaces, true);
}

status_t surface_set_role(struct comp_surface *s, enum comp_role role,
                          const struct comp_role_ops *ops, void *data)
{
    if (s->role_ops || (s->role != COMP_ROLE_NONE && s->role != role))
        return ERR_BAD_STATE;
    s->role = role;
    s->role_ops = ops;
    s->role_data = data;
    return OK;
}

void surface_drop_role(struct comp_surface *s)
{
    if (s->window)
        window_destroy(s->window);
    s->role_ops = NULL;
    s->role_data = NULL;
}

/* ---- requests --------------------------------------------------------------------- */

static status_t on_destroy(void *data, uint32_t self)
{
    (void)self;
    surface_free(data, false);   /* libjwl freed the id */
    return OK;
}

static status_t on_attach(void *data, uint32_t self, uint32_t buffer, int32_t x, int32_t y)
{
    struct comp_surface *s = data;
    struct comp_buffer *b = NULL;
    if (buffer) {
        b = comp_object(s->client, buffer, &jwl_wl_buffer_interface);
        if (!b)
            return comp_error(s->client, self, JWL_ERROR_INVALID_OBJECT, "no buffer %u", buffer);
        buffer_ref(b);
    }
    if (s->pending.buffer)
        buffer_unref(s->pending.buffer);
    s->pending.buffer = b;
    s->pending.attached = true;
    s->pending.dx = x;
    s->pending.dy = y;
    return OK;
}

static status_t on_damage(void *data, uint32_t self, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct comp_surface *s = data;
    (void)self;
    damage_add(&s->pending.damage, box_make(x, y, w, h));
    return OK;
}

static status_t on_frame(void *data, uint32_t self, uint32_t callback)
{
    struct comp_surface *s = data;
    if (s->nframes + s->npending >= COMP_FRAMES_MAX)
        return comp_no_memory(s->client, self, "too many frame callbacks (64)");
    s->frames[s->nframes + s->npending++] = callback;
    return OK;
}

/* set_opaque_region and set_input_region: the region's pixels, copied now
 * (the region may change or go before the commit). */
static status_t set_region(struct comp_surface *s, uint32_t self, uint32_t region,
                           struct comp_region *into)
{
    struct comp_regobj *r = NULL;
    if (region && !(r = comp_object(s->client, region, &jwl_wl_region_interface)))
        return comp_error(s->client, self, JWL_ERROR_INVALID_OBJECT, "no region %u", region);
    status_t st = OK;
    if (r)
        st = region_copy(into, &r->region);
    else
        region_clear(into);
    if (st == ERR_NO_RESOURCES)
        return comp_no_memory(s->client, self, "regions over 4096 boxes in all");
    return st == OK ? OK : comp_no_memory(s->client, self, "no memory for a region");
}

static status_t on_set_opaque_region(void *data, uint32_t self, uint32_t region)
{
    struct comp_surface *s = data;
    s->pending.opaque_set = true;
    return set_region(s, self, region, &s->pending.opaque);
}

static status_t on_set_input_region(void *data, uint32_t self, uint32_t region)
{
    struct comp_surface *s = data;
    s->pending.input_set = true;
    s->pending.input_all = !region;
    return set_region(s, self, region, &s->pending.input);
}

static status_t on_set_buffer_transform(void *data, uint32_t self, int32_t transform)
{
    struct comp_surface *s = data;
    if (transform < 0 || transform > TRANSFORM_MAX)
        return comp_error(s->client, self, JWL_WL_SURFACE_ERROR_INVALID_TRANSFORM,
                          "transform %d", transform);
    if (transform != JWL_WL_OUTPUT_TRANSFORM_NORMAL)
        return comp_error(s->client, self, JWL_ERROR_IMPLEMENTATION,
                          "only the normal transform: the output is never rotated");
    return OK;
}

static status_t on_set_buffer_scale(void *data, uint32_t self, int32_t scale)
{
    struct comp_surface *s = data;
    if (scale < 1)
        return comp_error(s->client, self, JWL_WL_SURFACE_ERROR_INVALID_SCALE, "scale %d", scale);
    if (scale != 1)
        return comp_error(s->client, self, JWL_ERROR_IMPLEMENTATION,
                          "only scale 1: the output's scale is 1");
    return OK;
}

/* ---- commit ----------------------------------------------------------------------- */

/* The pending buffer becomes the current one. */
static void apply_buffer(struct comp_surface *s)
{
    struct comp_state *p = &s->pending;
    struct comp_buffer *old = s->buffer, *b = p->buffer;
    int32_t w = b ? b->width : 0, h = b ? b->height : 0;
    bool resized = w != s->width || h != s->height;
    if (s->window && resized)
        window_damage(s->window);   /* the frame it had */
    if (b)
        buffer_show(b);   /* before the old one goes: the same buffer again isn't released */
    s->buffer = b;        /* the pending reference moves to the current state */
    p->buffer = NULL;
    if (old) {
        buffer_unshow(old);
        buffer_unref(old);
    }
    s->width = w;
    s->height = h;
    s->dx = p->dx;
    s->dy = p->dy;
    if (s->window && resized)
        window_damage(s->window);   /* the frame it has now */
}

static status_t apply_regions(struct comp_surface *s)
{
    struct comp_state *p = &s->pending;
    status_t st = OK;
    if (p->opaque_set)
        st = region_copy(&s->opaque, &p->opaque);
    if (st == OK && p->input_set) {
        s->input_all = p->input_all;
        st = region_copy(&s->input, &p->input);
    }
    p->opaque_set = p->input_set = false;
    if (st == ERR_NO_RESOURCES)
        return comp_no_memory(s->client, s->id, "regions over 4096 boxes in all");
    return st == OK ? OK : comp_no_memory(s->client, s->id, "no memory for a region");
}

static status_t on_commit(void *data, uint32_t self)
{
    struct comp_surface *s = data;
    struct comp_state *p = &s->pending;
    (void)self;
    status_t st = apply_regions(s);
    if (st != OK)
        return st;
    if (p->attached)
        apply_buffer(s);
    p->attached = false;
    p->dx = p->dy = 0;
    if (s->window)
        for (uint32_t i = 0; i < p->damage.n; i++)
            window_damage_surface(s->window, p->damage.b[i]);
    damage_clear(&p->damage);
    s->nframes += s->npending;   /* committed: answered after the next paint */
    s->npending = 0;
    s->commits++;
    return s->role_ops && s->role_ops->commit ? s->role_ops->commit(s) : OK;
}

static const struct jwl_wl_surface_requests surface_ops = {
    .destroy = on_destroy,
    .attach = on_attach,
    .damage = on_damage,
    .frame = on_frame,
    .set_opaque_region = on_set_opaque_region,
    .set_input_region = on_set_input_region,
    .commit = on_commit,
    .set_buffer_transform = on_set_buffer_transform,
    .set_buffer_scale = on_set_buffer_scale,
    .damage_buffer = on_damage,   /* the same coordinates at scale 1 (above) */
};

status_t surface_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_surface_dispatch_request(&surface_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- frame callbacks ------------------------------------------------------------------ */

/* s's committed callbacks answered with time t; the ones asked for since
 * the last commit move up. */
static void answer_frames(struct comp_surface *s, uint64_t t)
{
    struct jwl_conn *c = s->client->conn;
    for (uint32_t i = 0; i < s->nframes; i++)
        (void)jwl_wl_callback_send_done(c, s->frames[i], comp_ms(t));   /* a dead conn: torn down */
    memmove(s->frames, s->frames + s->nframes, s->npending * sizeof(s->frames[0]));
    s->nframes = 0;
}

void surfaces_frame_done(uint64_t t, bool painted)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = conn_client_at(i);
        for (struct comp_surface *s = cl ? cl->surfaces : NULL; s; s = s->next) {
            if (!s->nframes)
                continue;
            if (surface_visible(s)) {
                if (painted)
                    answer_frames(s, t);
            } else if (t - s->hidden_done_ns >= COMP_HIDDEN_FRAME_NS) {
                s->hidden_done_ns = t;
                answer_frames(s, t);
            }
        }
    }
}

uint64_t surfaces_hidden_deadline(void)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = conn_client_at(i);
        for (struct comp_surface *s = cl ? cl->surfaces : NULL; s; s = s->next) {
            uint64_t due = s->hidden_done_ns + COMP_HIDDEN_FRAME_NS;
            if (s->nframes && !surface_visible(s) && due < next)
                next = due;
        }
    }
    return next;
}

bool surfaces_waiting_paint(void)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        struct comp_client *cl = conn_client_at(i);
        for (struct comp_surface *s = cl ? cl->surfaces : NULL; s; s = s->next)
            if (s->nframes && surface_visible(s))
                return true;
    }
    return false;
}
