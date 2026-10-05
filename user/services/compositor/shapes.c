/* wp-cursor-shape-v1 (seat.h; third_party/wayland-protocols, staging): a
 * client asks for one of the compositor's cursors by name instead of
 * drawing its own (the terminal and Jamjar ask for the text bar).
 *
 * The global, wp_cursor_shape_manager_v1, makes a wp_cursor_shape_device_v1
 * for one of the client's wl_pointers (a tablet tool's device can be made,
 * since the protocol has the request, but Jam OS has no tablets: it stays
 * inert). set_shape, with the serial of the client's latest
 * wl_pointer.enter (else ignored, as the protocol says), replaces whatever
 * the client set before, a surface or a shape, and shows while the pointer
 * is over its window; a shape the protocol doesn't have is the
 * invalid_shape error. The CSS names map onto the set (cursors.c): the
 * resize ones by direction, wait and progress to busy, pointer and grab to
 * the hand, move and the all-ways ones to move, the text ones to the text
 * bar, the rest (help, crosshair, copy, ...) to the arrow, there being no
 * picture for them yet.
 *
 * Each manager and device is a seat object (seat_res, SEAT_OBJS_MAX of a
 * kind a client), gone with its client (seat_teardown). */
#include <jwl/cursor_shape_v1.h>
#include <jwl/wayland.h>
#include "seat.h"

#define SHAPE_LAST JWL_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE

/* The set's shape for each of the protocol's (by its number). */
static const uint8_t shape_of[SHAPE_LAST + 1] = {
    [1] = CURSOR_ARROW,  [2] = CURSOR_ARROW,  [3] = CURSOR_ARROW,  [4] = CURSOR_HAND,
    [5] = CURSOR_BUSY,   [6] = CURSOR_BUSY,   [7] = CURSOR_ARROW,  [8] = CURSOR_ARROW,
    [9] = CURSOR_TEXT,   [10] = CURSOR_TEXT,  [11] = CURSOR_ARROW, [12] = CURSOR_ARROW,
    [13] = CURSOR_MOVE,  [14] = CURSOR_ARROW, [15] = CURSOR_ARROW, [16] = CURSOR_HAND,
    [17] = CURSOR_HAND,  [18] = CURSOR_RESIZE_EW, [19] = CURSOR_RESIZE_NS,
    [20] = CURSOR_RESIZE_NESW, [21] = CURSOR_RESIZE_NWSE, [22] = CURSOR_RESIZE_NS,
    [23] = CURSOR_RESIZE_NWSE, [24] = CURSOR_RESIZE_NESW, [25] = CURSOR_RESIZE_EW,
    [26] = CURSOR_RESIZE_EW, [27] = CURSOR_RESIZE_NS, [28] = CURSOR_RESIZE_NESW,
    [29] = CURSOR_RESIZE_NWSE, [30] = CURSOR_RESIZE_EW, [31] = CURSOR_RESIZE_NS,
    [32] = CURSOR_MOVE,  [33] = CURSOR_ARROW, [34] = CURSOR_ARROW, [35] = CURSOR_ARROW,
    [36] = CURSOR_MOVE,
};

status_t shapes_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct seat_res *r;
    return seat_res_add(cl, SEAT_SHAPES, id, version, &r);
}

/* ---- the manager ------------------------------------------------------------------------- */

static status_t on_manager_destroy(void *data, uint32_t self)
{
    (void)self;
    seat_res_free(data);   /* libjwl freed the id */
    return OK;
}

/* A device for pointer (0: a tablet tool's, inert), at the manager's version. */
static status_t new_device(const struct seat_res *m, uint32_t id, uint32_t pointer)
{
    struct seat_res *r;
    status_t st = seat_res_add(m->client, SEAT_SHAPE_DEV, id, m->version, &r);
    if (st == OK)
        r->pointer = pointer;
    return st;
}

static status_t on_get_pointer(void *data, uint32_t self, uint32_t id, uint32_t pointer)
{
    (void)self;
    return new_device(data, id, pointer);   /* the codec checked it is a live wl_pointer */
}

static status_t on_get_tablet_tool(void *data, uint32_t self, uint32_t id, uint32_t tool)
{
    (void)self;
    (void)tool;
    return new_device(data, id, 0);
}

static const struct jwl_wp_cursor_shape_manager_v1_requests manager_ops = {
    .destroy = on_manager_destroy, .get_pointer = on_get_pointer,
    .get_tablet_tool_v2 = on_get_tablet_tool,
};

status_t shapes_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data knows its client */
    return jwl_wp_cursor_shape_manager_v1_dispatch_request(&manager_ops, m->data, m->id,
                                                           m->opcode, m->args);
}

/* ---- a device ---------------------------------------------------------------------------- */

static status_t on_device_destroy(void *data, uint32_t self)
{
    (void)self;
    seat_res_free(data);
    return OK;
}

static status_t on_set_shape(void *data, uint32_t self, uint32_t serial, uint32_t shape)
{
    struct seat_res *r = data;
    struct comp_client *cl = r->client;
    bool since2 = shape >= JWL_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK;
    if (shape < 1 || shape > SHAPE_LAST || (since2 && r->version < 2))
        return comp_error(cl, self, JWL_WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                          "no cursor shape %u at version %u", shape, r->version);
    struct seat_client *sc = seat_of(cl);
    if (!r->pointer || !comp_object(cl, r->pointer, &jwl_wl_pointer_interface) ||
        serial != sc->enter_serial)
        return OK;   /* inert, its pointer gone, or not after the latest enter: ignored */
    sc->shape_set = true;
    sc->shape = (enum cursor_shape)shape_of[shape];
    sc->cursor_set = false;   /* a shape replaces a surface */
    sc->cursor = NULL;
    seat_cursor_changed();
    return OK;
}

static const struct jwl_wp_cursor_shape_device_v1_requests device_ops = {
    .destroy = on_device_destroy, .set_shape = on_set_shape,
};

status_t shape_device_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wp_cursor_shape_device_v1_dispatch_request(&device_ops, m->data, m->id, m->opcode,
                                                          m->args);
}
