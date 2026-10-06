/* The clipboard (comp.h): wl_data_device_manager, wl_data_device,
 * wl_data_source and wl_data_offer, for the selection only (copy and paste
 * of text; docs/G1-PLAN.md, "As built: copy and paste").
 *
 * The model. The seat has one selection: a wl_data_source of some client's,
 * or none. A source offers MIME types; only the text ones in text_types[]
 * are kept, so only text is ever offered on. The rules (Wayland's, held
 * strictly):
 *   - set_selection is honoured only from the client with the keyboard
 *     focus, with the serial of an input event the seat sent it (a key or
 *     button press, wl_keyboard.enter: seat_input_serial_ok) that is no
 *     older than the current selection's; otherwise its source is told
 *     `cancelled` at once. A new selection cancels the old one's source;
 *   - the focused client's data devices hear the selection, as a new
 *     wl_data_offer with its types, right before wl_keyboard.enter
 *     (data_focus_enter) and again whenever it changes while focused; no
 *     other client hears of it;
 *   - the selection goes when its source does (wl_data_source.destroy, or
 *     its client going), and the focused client is told it is empty.
 *
 * The data. Wayland moves it through a pipe: the reader passes the write
 * end (wl_data_offer.receive), the compositor passes it on to the source
 * (wl_data_source.send), the source writes and closes it, the reader
 * reads to the end. Jam OS has no pipes: the reader passes a channel end
 * instead (the `fd` argument is a handle), keeping the other; the source
 * writes the data as channel messages (bytes only) and closes its end,
 * which the reader sees as the end of the data (peer closed), exactly a
 * pipe's EOF. The compositor never reads or writes the data, so a slow or
 * hostile source can't hold it up; the reader's side (libjwl) caps the
 * size and times a transfer out. A receive is passed on only when the
 * offer is the current selection's, the reader has the keyboard focus, the
 * type is one the source offered, the offer has had fewer than
 * DATA_RECEIVES_MAX, and the handle is a channel end (with read, write and
 * transfer rights: a fresh one's). The source gets it with write, wait and
 * transfer only, so it can't read what the reader writes back. A receive
 * that isn't passed on has its handle closed: the reader sees an empty
 * transfer. Nothing in any of this decides by the data itself.
 *
 * Drag and drop is not offered: start_drag uses its source up and cancels
 * it at once (the protocol's way of saying the drag never started). No
 * primary selection.
 *
 * Per-client objects live on lists by the client's slot (as seat.c's),
 * DATA_OBJS_MAX of each kind; an offer whose client never destroys its old
 * ones stops getting new offers (the selection is then said to be empty
 * for it) rather than growing. */
#include <jwl/wayland.h>
#include "seat.h"

enum data_kind { DATA_MANAGER, DATA_DEVICE, DATA_SOURCE, DATA_OFFER, DATA_KINDS };

/* One of a client's objects: the data libjwl's map holds for it. */
struct data_res {
    struct data_res *next;         /* the client's objects of this kind */
    struct comp_client *client;
    uint32_t id, version;
    enum data_kind kind;
    uint8_t types;                 /* SOURCE: bit n, text_types[n] offered */
    bool used;                     /* SOURCE: given to set_selection or start_drag */
    bool dnd;                      /* SOURCE: set_actions came: drag and drop only */
    uint8_t receives;              /* OFFER: receives passed on */
    struct data_res *source;       /* OFFER: whose data (NULL once that source went) */
};

struct data_client {
    struct data_res *res[DATA_KINDS];
    uint32_t nres[DATA_KINDS];
};

/* The text types passed on, the best first. */
static const char *const text_types[] = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT",
};
#define NTYPES (sizeof(text_types) / sizeof(text_types[0]))

static const char *const kind_names[DATA_KINDS] = {
    "wl_data_device_manager", "wl_data_device", "wl_data_source", "wl_data_offer",
};

static struct data_client dclients[COMP_CLIENTS_MAX];
static struct data_res *selection;     /* the selection's source, or NULL */
static uint32_t selection_serial;      /* the serial it was set with */

/* ---- objects ------------------------------------------------------------------------ */

static struct data_client *dc_of(const struct comp_client *cl)
{
    return &dclients[cl->slot];
}

/* A new object of kind for cl at id (version), entered as id's data. */
static status_t res_add(struct comp_client *cl, enum data_kind kind, uint32_t id,
                        uint32_t version, struct data_res **out)
{
    struct data_client *dc = dc_of(cl);
    if (dc->nres[kind] >= DATA_OBJS_MAX)
        return comp_error(cl, id, JWL_ERROR_NO_MEMORY, "too many %s objects (%u)",
                          kind_names[kind], DATA_OBJS_MAX);
    struct data_res *r = calloc(1, sizeof(*r));
    if (!r)
        return comp_no_memory(cl, id, "no memory for a clipboard object");
    *r = (struct data_res){ .next = dc->res[kind], .client = cl, .id = id, .version = version,
                            .kind = kind };
    dc->res[kind] = r;
    dc->nres[kind]++;
    *out = r;
    return jwl_map_set_data(&cl->conn->map, id, r);
}

static void res_free(struct data_res *r)
{
    struct data_client *dc = dc_of(r->client);
    for (struct data_res **p = &dc->res[r->kind]; *p; p = &(*p)->next) {
        if (*p == r) {
            *p = r->next;
            dc->nres[r->kind]--;
            break;
        }
    }
    free(r);
}

/* The client whose window has the keyboard focus, if it can be told anything. */
static struct comp_client *focused_client(void)
{
    struct comp_window *w = seat_focused();
    struct comp_client *cl = w ? w->surface->client : NULL;
    return client_alive(cl) ? cl : NULL;
}

/* ---- telling the focused client ----------------------------------------------------- */

/* A new offer of the selection on device d (its id in *out), its types
 * said; 0 in *out if cl may hold no more offers. */
static status_t make_offer(struct data_res *d, uint32_t *out)
{
    struct comp_client *cl = d->client;
    *out = 0;
    if (dc_of(cl)->nres[DATA_OFFER] >= DATA_OBJS_MAX)
        return OK;   /* it never destroys its old ones: no paste until it does */
    struct data_res *o = calloc(1, sizeof(*o));
    if (!o)
        return OK;   /* said to be empty: correct, only less useful */
    uint32_t id;
    status_t st = jwl_conn_make(cl->conn, &jwl_wl_data_offer_interface, d->version, o, &id);
    if (st == OK)
        st = jwl_wl_data_device_send_data_offer(cl->conn, d->id, id);
    if (st != OK) {
        free(o);
        return st;   /* the connection is dead: torn down after the turn */
    }
    struct data_client *dc = dc_of(cl);
    *o = (struct data_res){ .next = dc->res[DATA_OFFER], .client = cl, .id = id,
                            .version = d->version, .kind = DATA_OFFER, .source = selection };
    dc->res[DATA_OFFER] = o;
    dc->nres[DATA_OFFER]++;
    for (unsigned t = 0; t < NTYPES && st == OK; t++)
        if (selection->types & (1u << t))
            st = jwl_wl_data_offer_send_offer(cl->conn, id, text_types[t]);
    *out = id;
    return st;
}

/* The selection (or that there is none) to device d. */
static void tell_device(struct data_res *d)
{
    uint32_t offer = 0;
    if (selection && make_offer(d, &offer) != OK)
        return;
    (void)jwl_wl_data_device_send_selection(d->client->conn, d->id, offer);   /* dead: torn down */
}

static void tell_client(struct comp_client *cl)
{
    if (!client_alive(cl))
        return;
    for (struct data_res *d = dc_of(cl)->res[DATA_DEVICE]; d; d = d->next)
        tell_device(d);
}

void data_focus_enter(struct comp_client *cl)
{
    tell_client(cl);
}

/* src's client is told it is no longer the selection, or never was. */
static void cancel(struct data_res *src)
{
    if (client_alive(src->client))
        (void)jwl_wl_data_source_send_cancelled(src->client->conn, src->id);
}

/* src goes: offers of its data are left without it, and if it was the
 * selection the focused client hears there is none. */
static void source_gone(struct data_res *src)
{
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++)
        for (struct data_res *o = dclients[i].res[DATA_OFFER]; o; o = o->next)
            if (o->source == src)
                o->source = NULL;
    if (selection != src)
        return;
    selection = NULL;
    struct comp_client *f = focused_client();
    if (f && f != src->client)
        tell_client(f);
}

/* ---- wl_data_device_manager ---------------------------------------------------------- */

status_t data_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct data_res *r;
    return res_add(cl, DATA_MANAGER, id, version, &r);
}

static status_t on_create_source(void *data, uint32_t self, uint32_t id)
{
    const struct data_res *m = data;
    struct data_res *r;
    (void)self;
    return res_add(m->client, DATA_SOURCE, id, m->version, &r);
}

static status_t on_get_device(void *data, uint32_t self, uint32_t id, uint32_t seat)
{
    const struct data_res *m = data;
    struct data_res *d;
    (void)self, (void)seat;   /* one seat: libjwl checked it is a wl_seat */
    status_t st = res_add(m->client, DATA_DEVICE, id, m->version, &d);
    if (st == OK && focused_client() == m->client)
        tell_device(d);   /* it has the focus already: the selection now */
    return st;
}

static status_t on_release(void *data, uint32_t self)
{
    (void)self;
    res_free(data);   /* libjwl freed the id */
    return OK;
}

static const struct jwl_wl_data_device_manager_requests manager_ops = {
    .create_data_source = on_create_source, .get_data_device = on_get_device,
    .release = on_release,
};

status_t data_manager_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data knows its client */
    return jwl_wl_data_device_manager_dispatch_request(&manager_ops, m->data, m->id, m->opcode,
                                                       m->args);
}

/* ---- wl_data_device ------------------------------------------------------------------ */

/* The source object id of cl's, for a request that uses it up: NULL with
 * a protocol error posted if it was used before (or is drag and drop's,
 * for a selection). */
static struct data_res *use_source(struct comp_client *cl, uint32_t device, uint32_t id,
                                   bool for_selection)
{
    struct data_res *src = comp_object(cl, id, &jwl_wl_data_source_interface);
    if (!src) {
        comp_error(cl, device, JWL_ERROR_INVALID_OBJECT, "no wl_data_source %u", id);
        return NULL;
    }
    if (src->used) {
        comp_error(cl, device, JWL_WL_DATA_DEVICE_ERROR_USED_SOURCE,
                   "wl_data_source %u was used already", id);
        return NULL;
    }
    if (for_selection && src->dnd) {
        comp_error(cl, id, JWL_WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                   "a drag and drop source (set_actions) can't be the selection");
        return NULL;
    }
    src->used = true;
    return src;
}

/* Drag and drop isn't offered: the source (if any) is used up and
 * cancelled at once; nothing else happens (the icon gets no role). */
static status_t on_start_drag(void *data, uint32_t self, uint32_t source, uint32_t origin,
                              uint32_t icon, uint32_t serial)
{
    const struct data_res *d = data;
    (void)origin, (void)icon, (void)serial;
    if (!source)
        return OK;
    struct data_res *src = use_source(d->client, self, source, false);
    if (!src)
        return ERR_INVALID_ARGS;   /* posted */
    cancel(src);
    return OK;
}

/* May cl set the selection now with serial? */
static bool may_set(struct comp_client *cl, uint32_t serial)
{
    if (focused_client() != cl || !seat_input_serial_ok(cl, serial))
        return false;
    /* Not older than the selection there is: a late request doesn't
     * replace a newer copy. Serials wrap: the difference says which is newer. */
    return !selection || (int32_t)(serial - selection_serial) >= 0;
}

static status_t on_set_selection(void *data, uint32_t self, uint32_t source, uint32_t serial)
{
    const struct data_res *d = data;
    struct comp_client *cl = d->client;
    struct data_res *src = NULL;
    if (source && !(src = use_source(cl, self, source, true)))
        return ERR_INVALID_ARGS;   /* posted */
    if (!may_set(cl, serial)) {
        if (src)
            cancel(src);   /* refused: the source knows it isn't the selection */
        return OK;
    }
    struct data_res *old = selection;
    selection = src;
    selection_serial = serial;
    if (old && old != src)
        cancel(old);
    tell_client(cl);   /* the focused client: the new selection's offer */
    return OK;
}

static const struct jwl_wl_data_device_requests device_ops = {
    .start_drag = on_start_drag, .set_selection = on_set_selection, .release = on_release,
};

status_t data_device_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_data_device_dispatch_request(&device_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_data_source ------------------------------------------------------------------ */

static status_t on_source_offer(void *data, uint32_t self, const char *mime_type)
{
    struct data_res *src = data;
    (void)self;
    for (unsigned t = 0; t < NTYPES; t++)
        if (!strcmp(mime_type, text_types[t]))
            src->types |= (uint8_t)(1u << t);
    return OK;   /* not text: kept from every reader */
}

static status_t on_source_destroy(void *data, uint32_t self)
{
    (void)self;
    source_gone(data);
    res_free(data);
    return OK;
}

static status_t on_source_set_actions(void *data, uint32_t self, uint32_t dnd_actions)
{
    struct data_res *src = data;
    uint32_t all = JWL_WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                   JWL_WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
                   JWL_WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    if (dnd_actions & ~all)
        return comp_error(src->client, self, JWL_WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                          "actions 0x%x", dnd_actions);
    if (src->used || src->dnd)
        return comp_error(src->client, self, JWL_WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                          "set_actions only once, before start_drag");
    src->dnd = true;
    return OK;
}

static const struct jwl_wl_data_source_requests source_ops = {
    .offer = on_source_offer, .destroy = on_source_destroy, .set_actions = on_source_set_actions,
};

status_t data_source_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_data_source_dispatch_request(&source_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_data_offer ------------------------------------------------------------------- */

/* Is h a channel end we can read (and so pass on)? A read of nothing
 * says so without taking a message: a channel's queue answers
 * ERR_SHOULD_WAIT, or ERR_BUFFER_TOO_SMALL with something queued (left
 * there); any other object ERR_WRONG_TYPE. An empty message taken here
 * carried nothing. */
static bool is_channel(handle_t h)
{
    uint8_t none;
    uint32_t got[2] = { 0, 0 };
    struct channel_read_args a = { .h = h, .bytes_cap = 0, .bytes = (uint64_t)(uintptr_t)&none,
                                   .actual_bytes = (uint64_t)(uintptr_t)&got[0],
                                   .actual_handles = (uint64_t)(uintptr_t)&got[1] };
    status_t st = jam_channel_read(&a);
    return st == ERR_SHOULD_WAIT || st == ERR_BUFFER_TOO_SMALL || st == OK;
}

/* Which of text_types mime_type is, or NTYPES. */
static unsigned type_of(const char *mime_type)
{
    for (unsigned t = 0; t < NTYPES; t++)
        if (!strcmp(mime_type, text_types[t]))
            return t;
    return NTYPES;
}

/* May offer o's client read its data in mime_type now? */
static bool may_receive(struct data_res *o, const char *mime_type)
{
    unsigned t = type_of(mime_type);
    return o->source && o->source == selection && focused_client() == o->client &&
           t < NTYPES && (o->source->types & (1u << t)) && o->receives < DATA_RECEIVES_MAX;
}

static status_t on_receive(void *data, uint32_t self, const char *mime_type, handle_t fd)
{
    struct data_res *o = data;
    (void)self;
    handle_t w = HANDLE_INVALID;
    if (!may_receive(o, mime_type) || !is_channel(fd) ||
        jam_handle_replace(fd, RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER, &w) != OK) {
        jam_handle_close(fd);   /* the reader sees an empty transfer */
        return OK;
    }
    o->receives++;
    /* The source's own type string is ours: it offered exactly it. */
    (void)jwl_wl_data_source_send_send(o->source->client->conn, o->source->id,
                                       text_types[type_of(mime_type)], w);   /* consumes w */
    return OK;
}

static status_t on_accept(void *data, uint32_t self, uint32_t serial, const char *mime_type)
{
    (void)data, (void)self, (void)serial, (void)mime_type;
    return OK;   /* drag and drop's: a selection's offer has nothing to accept */
}

static status_t on_offer_destroy(void *data, uint32_t self)
{
    (void)self;
    res_free(data);   /* libjwl freed the id */
    return OK;
}

static status_t on_finish(void *data, uint32_t self)
{
    const struct data_res *o = data;
    return comp_error(o->client, self, JWL_WL_DATA_OFFER_ERROR_INVALID_FINISH,
                      "finish: a selection's offer, not a drop");
}

static status_t on_offer_set_actions(void *data, uint32_t self, uint32_t dnd_actions,
                                     uint32_t preferred_action)
{
    const struct data_res *o = data;
    (void)dnd_actions, (void)preferred_action;
    return comp_error(o->client, self, JWL_WL_DATA_OFFER_ERROR_INVALID_OFFER,
                      "set_actions: a selection's offer, not a drop");
}

static const struct jwl_wl_data_offer_requests offer_ops = {
    .accept = on_accept, .receive = on_receive, .destroy = on_offer_destroy,
    .finish = on_finish, .set_actions = on_offer_set_actions,
};

status_t data_offer_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_data_offer_dispatch_request(&offer_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- a client going ------------------------------------------------------------------ */

void data_teardown(struct comp_client *cl)
{
    struct data_client *dc = dc_of(cl);
    while (dc->res[DATA_SOURCE]) {
        source_gone(dc->res[DATA_SOURCE]);
        res_free(dc->res[DATA_SOURCE]);
    }
    for (unsigned k = 0; k < DATA_KINDS; k++)
        while (dc->res[k])
            res_free(dc->res[k]);
    *dc = (struct data_client){ 0 };
}
