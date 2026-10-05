/* The compositor's globals (comp.h): wl_display (sync, get_registry),
 * wl_registry (the globals and bind), wl_compositor (it makes surfaces
 * and regions), wl_region's requests, wl_output, and wl_callback as sync
 * answers it.
 *
 * The globals never come and go (one output, one seat). A row with no
 * bind function is not advertised yet: the seat (seat.c) and xdg-shell
 * (xdg.c) fill theirs. bind is checked against the row it names: the
 * interface's name and a version from 1 to ours (libjwl has already
 * checked the version against the interface's tables and entered the id).
 *
 * Objects' data: wl_display, wl_registry and wl_compositor hold the client
 * itself; a wl_region its struct comp_regobj; a wl_output its struct
 * comp_outres. */
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include "comp.h"

#define OUTPUT_MILLIHZ 60000   /* the mode we say: there is no vsync to tell (G1-PLAN) */

static status_t bind_compositor(struct comp_client *cl, uint32_t id, uint32_t version);
static status_t bind_output(struct comp_client *cl, uint32_t id, uint32_t version);

static const struct global {
    uint32_t name;                       /* wl_registry's number for it */
    const struct jwl_interface *iface;
    uint32_t version;                    /* the newest we offer */
    status_t (*bind)(struct comp_client *cl, uint32_t id, uint32_t version);
} globals[] = {
    { 1, &jwl_wl_compositor_interface, COMP_COMPOSITOR_VERSION, bind_compositor },
    { 2, &jwl_wl_shm_interface, COMP_SHM_VERSION, shm_bind },
    { 3, &jwl_wl_output_interface, COMP_OUTPUT_VERSION, bind_output },
    { 4, &jwl_wl_seat_interface, COMP_SEAT_VERSION, NULL },          /* seat.c */
    { 5, &jwl_xdg_wm_base_interface, COMP_XDG_WM_VERSION, xdg_bind },
};
#define NGLOBALS (sizeof(globals) / sizeof(globals[0]))

/* ---- wl_display --------------------------------------------------------------------- */

static status_t on_sync(void *data, uint32_t self, uint32_t callback)
{
    struct comp_client *cl = data;
    (void)self;
    return jwl_wl_callback_send_done(cl->conn, callback, comp_serial());   /* frees the id */
}

static status_t on_get_registry(void *data, uint32_t self, uint32_t registry)
{
    struct comp_client *cl = data;
    (void)self;
    status_t st = jwl_map_set_data(&cl->conn->map, registry, cl);
    for (unsigned i = 0; i < NGLOBALS && st == OK; i++)
        if (globals[i].bind)
            st = jwl_wl_registry_send_global(cl->conn, registry, globals[i].name,
                                             globals[i].iface->name, globals[i].version);
    return st;
}

static const struct jwl_wl_display_requests display_ops = {
    .sync = on_sync, .get_registry = on_get_registry,
};

status_t display_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_wl_display_dispatch_request(&display_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_registry -------------------------------------------------------------------- */

static status_t on_bind(void *data, uint32_t self, uint32_t name, const char *interface,
                        uint32_t version, uint32_t id)
{
    struct comp_client *cl = data;
    const struct global *g = NULL;
    for (unsigned i = 0; i < NGLOBALS; i++)
        if (globals[i].name == name && globals[i].bind)
            g = &globals[i];
    if (!g)
        return comp_error(cl, self, JWL_ERROR_INVALID_OBJECT, "no global %u", name);
    if (strcmp(interface, g->iface->name))
        return comp_error(cl, self, JWL_ERROR_INVALID_OBJECT, "global %u is %s, not %s", name,
                          g->iface->name, interface);
    if (version > g->version)
        return comp_error(cl, self, JWL_ERROR_INVALID_OBJECT, "%s version %u: we offer %u",
                          interface, version, g->version);
    return g->bind(cl, id, version);
}

static const struct jwl_wl_registry_requests registry_ops = { .bind = on_bind };

status_t registry_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_wl_registry_dispatch_request(&registry_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_compositor and wl_region ---------------------------------------------------- */

static status_t bind_compositor(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    return jwl_map_set_data(&cl->conn->map, id, cl);
}

static status_t on_create_surface(void *data, uint32_t self, uint32_t id)
{
    (void)self;
    return surface_create(data, id);
}

static status_t on_create_region(void *data, uint32_t self, uint32_t id)
{
    struct comp_client *cl = data;
    if (cl->nregions >= COMP_REGIONS_MAX)
        return comp_no_memory(cl, self, "too many regions (64)");
    struct comp_regobj *r = calloc(1, sizeof(*r));
    if (!r)
        return comp_no_memory(cl, self, "no memory for a region");
    r->client = cl;
    r->id = id;
    region_init(&r->region, &cl->rects);
    r->next = cl->regions;
    cl->regions = r;
    cl->nregions++;
    return jwl_map_set_data(&cl->conn->map, id, r);
}

static const struct jwl_wl_compositor_requests compositor_ops = {
    .create_surface = on_create_surface, .create_region = on_create_region,
};

status_t compositor_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_wl_compositor_dispatch_request(&compositor_ops, m->data, m->id, m->opcode, m->args);
}

static void regobj_free(struct comp_regobj *r)
{
    struct comp_client *cl = r->client;
    for (struct comp_regobj **p = &cl->regions; *p; p = &(*p)->next) {
        if (*p == r) {
            *p = r->next;
            break;
        }
    }
    cl->nregions--;
    region_fini(&r->region);
    free(r);
}

static status_t on_region_destroy(void *data, uint32_t self)
{
    (void)self;
    regobj_free(data);   /* libjwl freed the id */
    return OK;
}

/* A region change's refusal as the protocol error it is. */
static status_t region_status(struct comp_regobj *r, uint32_t self, status_t st)
{
    if (st == ERR_NO_RESOURCES)
        return comp_no_memory(r->client, self, "a region over 256 boxes (or 4096 in all)");
    if (st != OK)
        return comp_no_memory(r->client, self, "no memory for a region");
    return OK;
}

static status_t on_region_add(void *data, uint32_t self, int32_t x, int32_t y, int32_t w,
                              int32_t h)
{
    struct comp_regobj *r = data;
    return region_status(r, self, region_add(&r->region, box_make(x, y, w, h)));
}

static status_t on_region_subtract(void *data, uint32_t self, int32_t x, int32_t y, int32_t w,
                                   int32_t h)
{
    struct comp_regobj *r = data;
    return region_status(r, self, region_subtract(&r->region, box_make(x, y, w, h)));
}

static const struct jwl_wl_region_requests region_ops = {
    .destroy = on_region_destroy, .add = on_region_add, .subtract = on_region_subtract,
};

status_t region_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_region_dispatch_request(&region_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_output ---------------------------------------------------------------------- */

/* What a new wl_output is told: the one screen, its one mode. */
static status_t output_describe(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct jwl_conn *c = cl->conn;
    status_t st = jwl_wl_output_send_geometry(c, id, 0, 0, 0, 0, JWL_WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                              "Jam OS", comp.headless ? "headless" : "screen",
                                              JWL_WL_OUTPUT_TRANSFORM_NORMAL);
    if (st == OK)
        st = jwl_wl_output_send_mode(c, id,
                                     JWL_WL_OUTPUT_MODE_CURRENT | JWL_WL_OUTPUT_MODE_PREFERRED,
                                     scene.width, scene.height, OUTPUT_MILLIHZ);
    if (st == OK && version >= 2)
        st = jwl_wl_output_send_scale(c, id, 1);
    if (st == OK && version >= 2)
        st = jwl_wl_output_send_done(c, id);
    return st;
}

static status_t bind_output(struct comp_client *cl, uint32_t id, uint32_t version)
{
    if (cl->noutputs >= COMP_OUTPUTS_MAX)
        return comp_no_memory(cl, id, "too many wl_output objects (16)");
    struct comp_outres *o = calloc(1, sizeof(*o));
    if (!o)
        return comp_no_memory(cl, id, "no memory for a wl_output");
    o->client = cl;
    o->id = id;
    o->next = cl->outputs;
    cl->outputs = o;
    cl->noutputs++;
    status_t st = jwl_map_set_data(&cl->conn->map, id, o);
    return st == OK ? output_describe(cl, id, version) : st;
}

static void outres_free(struct comp_outres *o)
{
    struct comp_client *cl = o->client;
    for (struct comp_outres **p = &cl->outputs; *p; p = &(*p)->next) {
        if (*p == o) {
            *p = o->next;
            break;
        }
    }
    cl->noutputs--;
    free(o);
}

static status_t on_output_release(void *data, uint32_t self)
{
    (void)self;
    outres_free(data);   /* libjwl freed the id */
    return OK;
}

static const struct jwl_wl_output_requests output_ops = { .release = on_output_release };

status_t output_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_output_dispatch_request(&output_ops, m->data, m->id, m->opcode, m->args);
}

void display_teardown(struct comp_client *cl)
{
    while (cl->regions)
        regobj_free(cl->regions);
    while (cl->outputs)
        outres_free(cl->outputs);
}
