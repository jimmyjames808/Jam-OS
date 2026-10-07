/* libjwl's client: the clipboard (<jwl_client.h>, "the clipboard"): the
 * data device, our selection's source, the offers the compositor makes,
 * and a paste's transfer over a channel.
 *
 * Copying: a wl_data_source offering the text types below, given to
 * wl_data_device.set_selection with the serial of the last input event.
 * The text is kept here; each wl_data_source.send hands us the write end
 * of a reader's channel, into which the text goes at once, in messages of
 * at most JWL_CLIP_CHUNK bytes, without waiting (a reader whose channel
 * is full gets less, then the end); then the end is closed. `cancelled`
 * (replaced, refused) frees the source and the text.
 *
 * Pasting: the compositor introduces offers (data_offer, then its types)
 * and names the selection's (selection, or 0 for none); every other offer
 * is destroyed as soon as it isn't the selection, so at most JWLC_OFFERS
 * are kept. A paste makes a channel, sends one end in
 * wl_data_offer.receive and reads the other as it comes (bound to the
 * program's port when it has one, else looked at every POLL_NS by the
 * dispatch's deadline), into a buffer that grows to JWL_CLIP_MAX at most,
 * until the end (the owner closed: done), JWL_CLIP_WAIT_NS (timed out),
 * too much, or a message with handles (both refused). Nothing here
 * waits. */
#include <jwl_client.h>
#include <os.h>
#include "jwlc.h"

#define POLL_NS (10 * NS_PER_MS)   /* a paste with no port to wake us: looked at this often */

/* The text types, the best first (the compositor's own list). */
static const char *const types[] = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT",
};
#define NTYPES (sizeof(types) / sizeof(types[0]))

static void queue_clip(struct jwl_client *c, uint32_t type, status_t status, uint32_t size)
{
    struct jwl_event ev = { .type = type };
    ev.clip.available = c->clip.selection.id && c->clip.selection.types;
    ev.clip.status = status;
    ev.clip.size = size;
    jwlc_queue(c, &ev);
}

/* ---- the device ------------------------------------------------------------------------ */

void jwlc_clip_bound(struct jwl_client *c)
{
    uint32_t mgr = c->global[JWLC_DATA], seat = c->global[JWLC_SEAT], id;
    if (!mgr || !seat)
        return;
    if (jwlc_make(c, &jwl_wl_data_device_interface, c->info.data_version, c, &id) == OK &&
        jwlc_made(c, jwl_wl_data_device_manager_get_data_device(c->conn, mgr, id, seat), id) == OK)
        c->clip.device = id;
}

/* Our source and its text go. */
static void drop_source(struct jwl_client *c, bool destroy)
{
    if (destroy && c->clip.source && c->conn)
        (void)jwl_wl_data_source_destroy(c->conn, c->clip.source);
    c->clip.source = 0;
    free(c->clip.text);
    c->clip.text = NULL;
    c->clip.len = 0;
}

/* A paste ends with status; its text kept if OK. */
static void paste_end(struct jwl_client *c, status_t status)
{
    struct jwlc_clip *k = &c->clip;
    if (k->rx == HANDLE_INVALID)
        return;
    /* Closing alone wouldn't unbind it: the binding would keep the channel
     * end, and its charge, on the port until the port goes. */
    if (c->port != HANDLE_INVALID)
        (void)jam_port_unbind(c->port, k->rx, c->port_key);
    jam_handle_close(k->rx);
    k->rx = HANDLE_INVALID;
    uint32_t size = 0;
    if (status == OK) {
        k->buf[k->got] = '\0';
        k->pasted = k->buf;
        k->pasted_len = k->got;
        size = (uint32_t)k->got;
    } else {
        free(k->buf);
    }
    k->buf = NULL;
    k->got = k->cap = 0;
    queue_clip(c, JWL_EV_PASTE, status, size);
}

void jwlc_clip_lost(struct jwl_client *c)
{
    bool had_text = c->clip.selection.id && c->clip.selection.types;
    if (c->clip.source)
        jwlc_queue(c, &(struct jwl_event){ .type = JWL_EV_COPY_CANCELLED });
    drop_source(c, false);
    paste_end(c, ERR_PEER_CLOSED);
    c->clip.device = 0;
    memset(c->clip.offers, 0, sizeof(c->clip.offers));
    c->clip.selection = (struct jwlc_offer_rec){ 0 };
    if (had_text)
        queue_clip(c, JWL_EV_SELECTION, OK, 0);   /* nothing to paste until told again */
}

void jwlc_clip_free(struct jwl_client *c)
{
    drop_source(c, false);
    paste_end(c, ERR_PEER_CLOSED);
    free(c->clip.pasted);
    c->clip.pasted = NULL;
}

/* ---- copying --------------------------------------------------------------------------- */

status_t jwl_clip_copy(struct jwl_client *c, const char *text, size_t n)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    if (!jwlc_live(c))
        return ERR_SHOULD_WAIT;
    if (!c->clip.device)
        return ERR_NOT_SUPPORTED;
    if (n > JWL_CLIP_MAX)
        return ERR_OUT_OF_RANGE;
    if (!c->seat.input_serial)
        return ERR_BAD_STATE;
    char *copy = malloc(n ? n : 1);
    if (!copy)
        return ERR_NO_MEMORY;
    memcpy(copy, text, n);
    drop_source(c, true);   /* ours before: replaced by this one */
    uint32_t id;
    status_t st = jwlc_make(c, &jwl_wl_data_source_interface, c->info.data_version, c, &id);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_data_device_manager_create_data_source(
                              c->conn, c->global[JWLC_DATA], id), id);
    for (unsigned t = 0; t < NTYPES && st == OK; t++)
        st = jwl_wl_data_source_offer(c->conn, id, types[t]);
    if (st == OK)
        st = jwl_wl_data_device_set_selection(c->conn, c->clip.device, id, c->seat.input_serial);
    if (st == OK)
        st = jwl_conn_flush(c->conn);
    if (st != OK) {
        free(copy);
        return st;   /* the connection's: seen at the next dispatch */
    }
    c->clip.source = id;
    c->clip.text = copy;
    c->clip.len = n;
    return OK;
}

/* wl_data_source.send: the text into the reader's end fd, then the end. */
static void send_text(struct jwl_client *c, uint32_t source, handle_t fd)
{
    if (source == c->clip.source && c->clip.text) {
        for (size_t at = 0; at < c->clip.len;) {
            size_t n = c->clip.len - at < JWL_CLIP_CHUNK ? c->clip.len - at : JWL_CLIP_CHUNK;
            if (jam_channel_write(fd, c->clip.text + at, (uint32_t)n, NULL, 0) != OK)
                break;   /* full, or the reader went: it gets what it got, then the end */
            at += n;
        }
    }
    jam_handle_close(fd);
}

static status_t on_target(void *data, uint32_t self, const char *mime_type)
{
    (void)data, (void)self, (void)mime_type;
    return OK;   /* drag and drop's */
}

static status_t on_send(void *data, uint32_t self, const char *mime_type, handle_t fd)
{
    (void)mime_type;   /* every type we offer is the same text */
    send_text(data, self, fd);
    return OK;
}

static status_t on_cancelled(void *data, uint32_t self)
{
    struct jwl_client *c = data;
    if (self == c->clip.source) {
        drop_source(c, true);
        jwlc_queue(c, &(struct jwl_event){ .type = JWL_EV_COPY_CANCELLED });
    } else if (c->conn) {
        (void)jwl_wl_data_source_destroy(c->conn, self);   /* an old one of ours */
    }
    return OK;
}

static status_t on_dnd_event(void *data, uint32_t self)
{
    (void)data, (void)self;
    return OK;
}

static status_t on_action(void *data, uint32_t self, uint32_t dnd_action)
{
    (void)data, (void)self, (void)dnd_action;
    return OK;
}

static const struct jwl_wl_data_source_events source_events = {
    .target = on_target, .send = on_send, .cancelled = on_cancelled,
    .dnd_drop_performed = on_dnd_event, .dnd_finished = on_dnd_event, .action = on_action,
};

/* ---- the compositor's offers ----------------------------------------------------------- */

static void destroy_offer(struct jwl_client *c, uint32_t id)
{
    if (id && c->conn)
        (void)jwl_wl_data_offer_destroy(c->conn, id);
}

static struct jwlc_offer_rec *find_offer(struct jwl_client *c, uint32_t id)
{
    for (unsigned i = 0; i < JWLC_OFFERS; i++)
        if (c->clip.offers[i].id == id && id)
            return &c->clip.offers[i];
    return NULL;
}

static status_t on_data_offer(void *data, uint32_t self, uint32_t id)
{
    struct jwl_client *c = data;
    (void)self;
    struct jwlc_offer_rec *o = NULL;
    for (unsigned i = 0; i < JWLC_OFFERS && !o; i++)
        if (!c->clip.offers[i].id)
            o = &c->clip.offers[i];
    if (!o) {   /* more introduced than named: the oldest goes */
        o = &c->clip.offers[0];
        destroy_offer(c, o->id);
    }
    *o = (struct jwlc_offer_rec){ .id = id };
    (void)jwl_map_set_data(&c->conn->map, id, c);
    return OK;
}

static status_t on_offer_type(void *data, uint32_t self, const char *mime_type)
{
    struct jwl_client *c = data;
    struct jwlc_offer_rec *o = find_offer(c, self);
    for (unsigned t = 0; o && t < NTYPES; t++)
        if (!strcmp(mime_type, types[t]))
            o->types |= (uint8_t)(1u << t);
    return OK;
}

static status_t on_source_actions(void *data, uint32_t self, uint32_t actions)
{
    (void)data, (void)self, (void)actions;
    return OK;
}

static const struct jwl_wl_data_offer_events offer_events = {
    .offer = on_offer_type, .source_actions = on_source_actions, .action = on_action,
};

/* The selection is now offer id (0: none): every other offer goes. */
static status_t on_selection(void *data, uint32_t self, uint32_t id)
{
    struct jwl_client *c = data;
    (void)self;
    struct jwlc_offer_rec next = { 0 }, *o = find_offer(c, id);
    if (o) {
        next = *o;
        o->id = 0;
    }
    destroy_offer(c, c->clip.selection.id);
    for (unsigned i = 0; i < JWLC_OFFERS; i++) {
        destroy_offer(c, c->clip.offers[i].id);
        c->clip.offers[i].id = 0;
    }
    c->clip.selection = next;
    queue_clip(c, JWL_EV_SELECTION, OK, 0);
    return OK;
}

static status_t on_dnd_enter(void *data, uint32_t self, uint32_t serial, uint32_t surface,
                             int32_t x, int32_t y, uint32_t id)
{
    (void)data, (void)self, (void)serial, (void)surface, (void)x, (void)y, (void)id;
    return OK;   /* no drags: the compositor offers none */
}

static status_t on_dnd_motion(void *data, uint32_t self, uint32_t time, int32_t x, int32_t y)
{
    (void)data, (void)self, (void)time, (void)x, (void)y;
    return OK;
}

static const struct jwl_wl_data_device_events device_events = {
    .data_offer = on_data_offer, .enter = on_dnd_enter, .leave = on_dnd_event,
    .motion = on_dnd_motion, .drop = on_dnd_event, .selection = on_selection,
};

status_t jwlc_clip_event(struct jwl_client *c, struct jwl_msg *m)
{
    if (m->iface == &jwl_wl_data_device_interface)
        return jwl_wl_data_device_dispatch_event(&device_events, c, m->id, m->opcode, m->args);
    if (m->iface == &jwl_wl_data_offer_interface)
        return jwl_wl_data_offer_dispatch_event(&offer_events, c, m->id, m->opcode, m->args);
    return jwl_wl_data_source_dispatch_event(&source_events, c, m->id, m->opcode, m->args);
}

/* ---- pasting ----------------------------------------------------------------------------- */

bool jwl_clip_available(const struct jwl_client *c)
{
    return c->clip.selection.id && c->clip.selection.types && jwlc_live(c);
}

const char *jwl_clip_pasted(const struct jwl_client *c, size_t *n)
{
    *n = c->clip.pasted ? c->clip.pasted_len : 0;
    return c->clip.pasted;
}

/* Our end of a new channel, the other sent in a receive of the best type. */
static status_t start_receive(struct jwl_client *c, handle_t *out)
{
    unsigned t = 0;
    while (!(c->clip.selection.types & (1u << t)))
        t++;   /* types is not 0: jwl_clip_available */
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jwl_wl_data_offer_receive(c->conn, c->clip.selection.id, types[t], theirs);
    if (st == OK)
        st = jwl_conn_flush(c->conn);
    if (st == OK && c->port != HANDLE_INVALID)
        st = jam_port_bind(c->port, mine, c->port_key, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    *out = mine;
    return OK;
}

status_t jwl_clip_paste(struct jwl_client *c)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    if (c->clip.rx != HANDLE_INVALID)
        return ERR_BAD_STATE;
    if (!jwl_clip_available(c))
        return ERR_NOT_FOUND;
    handle_t rx;
    status_t st = start_receive(c, &rx);
    if (st != OK)
        return st;
    free(c->clip.pasted);
    c->clip.pasted = NULL;
    c->clip.pasted_len = 0;
    c->clip.rx = rx;
    c->clip.rx_deadline = now() + JWL_CLIP_WAIT_NS;
    return OK;
}

/* Room for one more message of the most a transfer sends. */
static bool room(struct jwlc_clip *k)
{
    if (k->cap - k->got > JWL_CLIP_CHUNK)
        return true;
    size_t cap = k->cap ? k->cap * 2 : JWL_CLIP_CHUNK * 2;
    if (cap > JWL_CLIP_MAX + JWL_CLIP_CHUNK + 1)
        cap = JWL_CLIP_MAX + JWL_CLIP_CHUNK + 1;   /* the most it can hold before refusing */
    char *b = malloc(cap);
    if (!b)
        return false;
    if (k->got)
        memcpy(b, k->buf, k->got);
    free(k->buf);
    k->buf = b;
    k->cap = cap;
    return true;
}

/* One message of the transfer: OK, ERR_SHOULD_WAIT (nothing yet), or why
 * it ends (ERR_PEER_CLOSED: the text is all here). */
static status_t read_one(struct jwlc_clip *k)
{
    if (!room(k))
        return ERR_NO_MEMORY;
    uint32_t sizes[2] = { 0, 0 };
    handle_t h[4];
    struct channel_read_args a = {
        .h = k->rx, .bytes_cap = (uint32_t)(k->cap - k->got - 1),
        .bytes = (uint64_t)(uintptr_t)(k->buf + k->got),
        .actual_bytes = (uint64_t)(uintptr_t)&sizes[0], .handles = (uint64_t)(uintptr_t)h,
        .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&sizes[1],
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_BUFFER_TOO_SMALL)
        return ERR_INVALID_ARGS;   /* bigger than any message the owner may send, or handles */
    if (st != OK)
        return st;
    for (uint32_t i = 0; i < sizes[1] && i < 4; i++)
        jam_handle_close(h[i]);
    if (sizes[1])
        return ERR_INVALID_ARGS;   /* bytes only */
    k->got += sizes[0];
    return k->got > JWL_CLIP_MAX ? ERR_OUT_OF_RANGE : OK;
}

void jwlc_clip_tick(struct jwl_client *c)
{
    struct jwlc_clip *k = &c->clip;
    if (k->rx == HANDLE_INVALID)
        return;
    status_t st = OK;
    /* bounded: each message read grows got, which is capped */
    while (st == OK)
        st = read_one(k);
    if (st == ERR_SHOULD_WAIT && now() >= k->rx_deadline)
        st = ERR_TIMED_OUT;
    if (st == ERR_SHOULD_WAIT)
        return;
    paste_end(c, st == ERR_PEER_CLOSED ? OK : st);
}

uint64_t jwlc_clip_deadline(const struct jwl_client *c)
{
    if (c->clip.rx == HANDLE_INVALID)
        return DEADLINE_NEVER;
    if (c->port != HANDLE_INVALID)
        return c->clip.rx_deadline;
    uint64_t poll = now() + POLL_NS;
    return poll < c->clip.rx_deadline ? poll : c->clip.rx_deadline;
}
