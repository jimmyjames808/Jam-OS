/* The seat (seat.h): the wl_seat global and its objects, each client's
 * seat state, and the seat's part of the loop's turn.
 *
 * One seat, "seat0", with a keyboard and a pointer: both capabilities are
 * always advertised, whether a keyboard or mouse is plugged in yet or not
 * (one may come at any time, and every client of ours wants both), and
 * wl_touch is never offered (get_touch is the protocol's
 * missing_capability error).
 *
 * A client may hold SEAT_OBJS_MAX objects of each kind (wl_seat,
 * wl_keyboard, wl_pointer): more is no_memory, as every cap. The objects
 * hold no authority: the keyboard focus and the pointer decide what each
 * one is sent, never the client.
 *
 * Per-client state lives in a table by the client's slot, so it never
 * fails to exist; seat_teardown clears it for the slot's next client. */
#include <jwl/wayland.h>
#include "seat.h"

static struct seat_client clients[COMP_CLIENTS_MAX];
static const char *const kind_names[SEAT_KINDS] = { "wl_seat", "wl_keyboard", "wl_pointer",
                                                    "wp_cursor_shape_manager_v1",
                                                    "wp_cursor_shape_device_v1" };
uint64_t seat_keys, seat_reserved;

struct seat_client *seat_of(struct comp_client *cl)
{
    struct seat_client *sc = &clients[cl->slot];
    cl->seat = sc;
    return sc;
}

bool client_alive(const struct comp_client *cl)
{
    return cl && cl->conn && cl->conn->status == OK;
}

bool surface_live(const struct comp_surface *s)
{
    if (!s || !client_alive(s->client))
        return false;
    const struct jwl_object *o = jwl_conn_object(s->client->conn, s->id);
    return o && o->iface == &jwl_wl_surface_interface && !o->pending && o->data == s;
}

/* ---- the objects ------------------------------------------------------------------ */

status_t seat_res_add(struct comp_client *cl, enum seat_kind kind, uint32_t id, uint32_t version,
                      struct seat_res **out)
{
    struct seat_client *sc = seat_of(cl);
    if (sc->nres[kind] >= SEAT_OBJS_MAX)
        return comp_error(cl, id, JWL_ERROR_NO_MEMORY, "too many %s objects (%u)",
                          kind_names[kind], SEAT_OBJS_MAX);
    struct seat_res *r = calloc(1, sizeof(*r));
    if (!r)
        return comp_no_memory(cl, id, "no memory for a seat object");
    *r = (struct seat_res){ .next = sc->res[kind], .client = cl, .id = id, .version = version,
                            .kind = kind };
    sc->res[kind] = r;
    sc->nres[kind]++;
    *out = r;
    return jwl_map_set_data(&cl->conn->map, id, r);
}

void seat_res_free(struct seat_res *r)
{
    struct seat_client *sc = seat_of(r->client);
    for (struct seat_res **p = &sc->res[r->kind]; *p; p = &(*p)->next) {
        if (*p == r) {
            *p = r->next;
            sc->nres[r->kind]--;
            break;
        }
    }
    free(r);
}

/* ---- wl_seat ---------------------------------------------------------------------- */

status_t seat_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct seat_res *r;
    status_t st = seat_res_add(cl, SEAT_SEAT, id, version, &r);
    if (st == OK)
        st = jwl_wl_seat_send_capabilities(cl->conn, id, JWL_WL_SEAT_CAPABILITY_POINTER |
                                                             JWL_WL_SEAT_CAPABILITY_KEYBOARD);
    if (st == OK && version >= JWL_WL_SEAT_EV_NAME_SINCE)
        st = jwl_wl_seat_send_name(cl->conn, id, SEAT_NAME);
    return st;
}

/* A typed new id takes its parent's version: the seat's. */
static status_t on_get_pointer(void *data, uint32_t self, uint32_t id)
{
    const struct seat_res *r = data;
    (void)self;
    return pointer_create(r->client, id, r->version);
}

static status_t on_get_keyboard(void *data, uint32_t self, uint32_t id)
{
    const struct seat_res *r = data;
    (void)self;
    return keyboard_create(r->client, id, r->version);
}

static status_t on_get_touch(void *data, uint32_t self, uint32_t id)
{
    const struct seat_res *r = data;
    (void)id;
    return comp_error(r->client, self, JWL_WL_SEAT_ERROR_MISSING_CAPABILITY,
                      "the seat has no touch devices");
}

static status_t on_release(void *data, uint32_t self)
{
    (void)self;
    seat_res_free(data);   /* libjwl freed the id */
    return OK;
}

static const struct jwl_wl_seat_requests seat_ops = {
    .get_pointer = on_get_pointer, .get_keyboard = on_get_keyboard,
    .get_touch = on_get_touch, .release = on_release,
};

status_t seat_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data knows its client */
    return jwl_wl_seat_dispatch_request(&seat_ops, m->data, m->id, m->opcode, m->args);
}

static const struct jwl_wl_keyboard_requests keyboard_ops = { .release = on_release };

status_t keyboard_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_keyboard_dispatch_request(&keyboard_ops, m->data, m->id, m->opcode, m->args);
}

void seat_teardown(struct comp_client *cl)
{
    struct seat_client *sc = seat_of(cl);
    pointer_client_gone(cl);
    for (unsigned k = 0; k < SEAT_KINDS; k++)
        while (sc->res[k])
            seat_res_free(sc->res[k]);
    *sc = (struct seat_client){ 0 };
    cl->seat = NULL;
}

/* ---- the seat in the loop ----------------------------------------------------------- */

status_t seat_init(void)
{
    pointer_init_position();
    status_t st = keyboard_init();
    return st == OK ? ctl_init() : st;
}

void seat_packet(uint64_t key)
{
    if (key >= SEAT_KEY_SRC && key < SEAT_KEY_INIT)
        sources_packet((unsigned)(key - SEAT_KEY_SRC));
    else
        ctl_packet(key);
}

void seat_serve(void)
{
    sources_serve();
    ctl_serve();
}

void seat_turn(void)
{
    pointer_turn();
}

uint64_t seat_deadline(void)
{
    return sources_more() || ctl_more() ? 0 : ctl_deadline();
}

void seat_window_mapped(struct comp_window *w)
{
    focus_window_mapped(w);
}

void seat_window_gone(struct comp_window *w)
{
    pointer_window_gone(w);
    focus_window_gone(w);
}
