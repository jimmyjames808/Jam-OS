/* libjwl's transport (<jwl.h>): a Wayland connection over a channel.
 *
 * Receiving: one channel message (a batch) at a time into c->in, its
 * header checked (magic, reserved, the handle count it claims against the
 * handles it carries, the acknowledgement), then its messages decoded one
 * per jwl_conn_next. A batch's handles are taken by its 'h' arguments in
 * order; a batch whose messages leave handles unused is refused when its
 * last byte is used up, before the next batch is read.
 *
 * Sending: messages are encoded into c->out behind room for the header,
 * which is written when the batch goes (so a held batch carries the
 * newest acknowledgement). A batch goes when it is full or at
 * jwl_conn_flush. The compositor writes at most JWL_WINDOW batches past
 * the client's last acknowledgement; the rest wait in c->held, in order,
 * up to JWL_HELD_MAX bytes, and past that the client is disconnected with
 * no_memory. A client's batches have no window (they are charged to the
 * client's own job), but a channel that is full holds them the same way.
 *
 * Dying: the first error that ends a connection is kept in c->status and
 * every call returns it from then on; every handle still in the
 * connection (unread arguments, unwritten batches) is closed at once. The
 * channel itself stays open until jwl_conn_destroy, so a compositor's
 * wl_display.error is read by the client before it sees the close. */
#include <jwl.h>
#include <os.h>

/* A sealed batch not written yet. */
struct jwl_held {
    struct jwl_held *next;
    uint32_t len, nh;                   /* bytes (the header's room included), handles */
    handle_t h[JWL_BATCH_HANDLES];
    uint8_t  bytes[];
};

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void close_handles(const handle_t *h, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(h[i]);
}

static void drop_out(struct jwl_conn *c)
{
    close_handles(c->out_h, c->out_nh);
    c->out_len = JWL_HEADER_BYTES;
    c->out_nh = 0;
}

static void drop_held(struct jwl_conn *c)
{
    while (c->held) {
        struct jwl_held *b = c->held;
        c->held = b->next;
        close_handles(b->h, b->nh);
        free(b);
    }
    c->held_tail = NULL;
    c->held_bytes = 0;
}

/* The connection ends with st (the first reason is kept). */
static status_t die(struct jwl_conn *c, status_t st)
{
    if (c->status == OK)
        c->status = st;
    drop_out(c);
    drop_held(c);
    close_handles(c->in_h + c->in_hat, c->in_nh - c->in_hat);
    c->in_hat = c->in_nh;
    c->in_at = c->in_len;
    return c->status;
}

/* The peer broke the protocol: the compositor tells the client, a client
 * just stops. */
static status_t broken(struct jwl_conn *c, const struct jwl_error *err)
{
    if (c->side == JWL_SERVER)
        return jwl_conn_post_error(c, err->object, err->code, err->text);
    c->error = *err;
    return die(c, ERR_INVALID_ARGS);
}

static status_t broken_batch(struct jwl_conn *c, const char *what)
{
    struct jwl_error err = { .object = JWL_DISPLAY_ID, .code = JWL_ERROR_INVALID_METHOD };
    snprintf(err.text, sizeof(err.text), "a batch %s", what);
    return broken(c, &err);
}

static bool window_open(const struct jwl_conn *c)
{
    return c->side == JWL_CLIENT || c->nsent - c->peer_acked < JWL_WINDOW;
}

/* ---- the connection -------------------------------------------------------------- */

static bool is_message(const struct jwl_message *m, const char *name, const char *sig)
{
    return m->name && m->signature && !strcmp(m->name, name) && !strcmp(m->signature, sig);
}

/* wl_display's table is what the transport itself sends and reads. */
static status_t check_tables(const struct jwl_conn_config *cfg)
{
    const struct jwl_interface *d = cfg->display;
    if (jwl_interface_check(d) != OK || strcmp(d->name, "wl_display") || d->nevents < 2 ||
        !is_message(&d->events[0], "error", "ous") || !is_message(&d->events[1], "delete_id", "u"))
        return ERR_INVALID_ARGS;
    if (cfg->nknown && !cfg->known)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < cfg->nknown; i++)
        if (jwl_interface_check(cfg->known[i]) != OK)
            return ERR_INVALID_ARGS;
    return cfg->side == JWL_CLIENT || cfg->side == JWL_SERVER ? OK : ERR_INVALID_ARGS;
}

status_t jwl_conn_create(const struct jwl_conn_config *cfg, struct jwl_conn **out)
{
    status_t st = check_tables(cfg);
    struct jwl_conn *c = st == OK ? calloc(1, sizeof(*c)) : NULL;
    if (st == OK && !c)
        st = ERR_NO_MEMORY;
    if (st == OK)
        st = jwl_map_init(&c->map, cfg->side, cfg->display, cfg->known, cfg->nknown);
    if (st != OK) {
        free(c);
        jam_handle_close(cfg->ch);
        return st;
    }
    c->ch = cfg->ch;
    c->side = cfg->side;
    c->display = cfg->display;
    c->out_len = JWL_HEADER_BYTES;
    *out = c;
    return OK;
}

void jwl_conn_destroy(struct jwl_conn *c)
{
    if (!c)
        return;
    die(c, ERR_BAD_STATE);
    jam_handle_close(c->ch);
    jwl_map_free(&c->map);
    free(c);
}

bool jwl_conn_backlogged(const struct jwl_conn *c)
{
    return c->side == JWL_SERVER && (c->held || !window_open(c));
}

/* ---- receiving -------------------------------------------------------------------- */

static status_t check_header(struct jwl_conn *c)
{
    if (c->in_len < JWL_HEADER_BYTES)
        return broken_batch(c, "shorter than its header");
    uint32_t acked = rd32(c->in + 4);
    if (rd32(c->in) != JWL_MAGIC)
        return broken_batch(c, "without the JWL1 header");
    if (rd32(c->in + 12) != 0)
        return broken_batch(c, "header with its reserved word set");
    if (rd32(c->in + 8) != c->in_nh)
        return broken_batch(c, "header naming more or fewer handles than it carries");
    if (c->side == JWL_CLIENT) {
        if (acked != 0)
            return broken_batch(c, "from the compositor acknowledging");
        c->nread++;
        return OK;
    }
    if (acked - c->peer_acked > c->nsent - c->peer_acked)
        return broken_batch(c, "acknowledging batches never sent");
    c->peer_acked = acked;
    return OK;
}

static status_t read_batch(struct jwl_conn *c)
{
    uint32_t nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = c->ch,
        .bytes_cap = JWL_BATCH_MAX,
        .bytes = (uint64_t)(uintptr_t)c->in,
        .actual_bytes = (uint64_t)(uintptr_t)&nb,
        .handles = (uint64_t)(uintptr_t)c->in_h,
        .handles_cap = JWL_BATCH_HANDLES,
        .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_SHOULD_WAIT)
        return st;
    if (st == ERR_BUFFER_TOO_SMALL)
        return broken_batch(c, "over 16 KiB or 16 handles");
    if (st != OK)
        return die(c, st);
    c->in_len = nb;
    c->in_at = nb < JWL_HEADER_BYTES ? nb : JWL_HEADER_BYTES;
    c->in_nh = nh;
    c->in_hat = 0;
    c->stats.batches_in++;
    return check_header(c);
}

/* A message the transport takes itself: the compositor's delete_id, on a
 * client. */
static status_t take_delete_id(struct jwl_conn *c, const struct jwl_msg *m)
{
    if (jwl_map_delete_id(&c->map, m->args[0].u) == OK)
        return OK;
    struct jwl_error err = { .object = JWL_DISPLAY_ID, .code = JWL_ERROR_INVALID_OBJECT };
    snprintf(err.text, sizeof(err.text), "delete_id for id %u, not one in use", m->args[0].u);
    return broken(c, &err);
}

status_t jwl_conn_next(struct jwl_conn *c, struct jwl_msg *out)
{
    for (;;) {
        if (c->status != OK)
            return c->status;
        if (c->in_at >= c->in_len) {
            if (c->in_hat < c->in_nh)
                return broken_batch(c, "with handles its messages don't use");
            status_t st = read_batch(c);
            if (st != OK)
                return st;
            continue;
        }
        struct jwl_in in = { .buf = c->in, .len = c->in_len, .at = c->in_at,
                             .h = c->in_h, .nh = c->in_nh, .hat = c->in_hat };
        struct jwl_error err;
        status_t st = jwl_decode(&in, &c->map, out, &err);
        if (st != OK)
            return broken(c, &err);
        c->in_at = (uint32_t)in.at;
        c->in_hat = in.hat;
        c->stats.msgs_in++;
        if (out->dead_target) {
            jwl_msg_close_handles(out);
            c->stats.dropped++;
            continue;
        }
        if (c->side == JWL_CLIENT && out->id == JWL_DISPLAY_ID && out->opcode == 1) {
            st = take_delete_id(c, out);
            if (st != OK)
                return st;
            continue;
        }
        return OK;
    }
}

/* ---- sending ---------------------------------------------------------------------- */

/* Write one batch (buf has room for the header at its start). OK;
 * ERR_SHOULD_WAIT: the channel or our job can't take it now (it stays
 * ours, handles too); anything else kills the connection. */
static status_t write_batch(struct jwl_conn *c, uint8_t *buf, uint32_t len, const handle_t *h,
                            uint32_t nh)
{
    wr32(buf, JWL_MAGIC);
    wr32(buf + 4, c->side == JWL_CLIENT ? c->nread : 0);
    wr32(buf + 8, nh);
    wr32(buf + 12, 0);
    status_t st = jam_channel_write(c->ch, buf, len, h, nh);
    if (st == ERR_SHOULD_WAIT || st == ERR_NO_MEMORY)
        return ERR_SHOULD_WAIT;
    if (st != OK)
        return die(c, st);
    if (c->side == JWL_CLIENT)
        c->told = c->nread;
    else
        c->nsent++;
    c->stats.batches_out++;
    return OK;
}

static status_t overflow(struct jwl_conn *c)
{
    if (c->side == JWL_CLIENT)
        return die(c, ERR_NO_MEMORY);
    return jwl_conn_post_error(c, JWL_DISPLAY_ID, JWL_ERROR_NO_MEMORY,
                               "the client reads too slowly: 64 KiB of events held");
}

/* The batch being filled joins the held ones. */
static status_t hold(struct jwl_conn *c)
{
    if (c->out_len > JWL_HELD_MAX - c->held_bytes)
        return overflow(c);
    struct jwl_held *b = malloc(sizeof(*b) + c->out_len);
    if (!b)
        return overflow(c);
    b->next = NULL;
    b->len = c->out_len;
    b->nh = c->out_nh;
    memcpy(b->h, c->out_h, c->out_nh * sizeof(handle_t));
    memcpy(b->bytes, c->out, c->out_len);
    if (c->held_tail)
        c->held_tail->next = b;
    else
        c->held = b;
    c->held_tail = b;
    c->held_bytes += b->len;
    if (c->held_bytes > c->stats.held_peak)
        c->stats.held_peak = c->held_bytes;
    c->out_len = JWL_HEADER_BYTES;   /* its handles are the held batch's now */
    c->out_nh = 0;
    return OK;
}

/* The batch being filled goes, or is held. */
static status_t seal(struct jwl_conn *c)
{
    if (c->out_len == JWL_HEADER_BYTES)
        return OK;
    status_t st = ERR_SHOULD_WAIT;
    if (!c->held && window_open(c))
        st = write_batch(c, c->out, c->out_len, c->out_h, c->out_nh);
    if (st == ERR_SHOULD_WAIT)
        return hold(c);
    if (st == OK) {
        c->out_len = JWL_HEADER_BYTES;
        c->out_nh = 0;
    }
    return st;
}

/* Held batches, oldest first, while the window and the channel take them. */
static status_t drain(struct jwl_conn *c)
{
    while (c->held && window_open(c)) {
        struct jwl_held *b = c->held;
        status_t st = write_batch(c, b->bytes, b->len, b->h, b->nh);
        if (st == ERR_SHOULD_WAIT)
            return OK;
        if (st != OK)
            return st;
        c->held = b->next;
        if (!c->held)
            c->held_tail = NULL;
        c->held_bytes -= b->len;
        free(b);
    }
    return OK;
}

status_t jwl_conn_flush(struct jwl_conn *c)
{
    if (c->status != OK)
        return c->status;
    status_t st = drain(c);
    if (st == OK)
        st = seal(c);
    if (st == OK && c->side == JWL_CLIENT && !c->held && c->nread - c->told >= JWL_WINDOW / 2) {
        uint8_t ack[JWL_HEADER_BYTES];
        (void)write_batch(c, ack, sizeof(ack), NULL, 0);   /* a full channel: next turn */
    }
    return c->status;
}

/* The message opcode of iface for this side to send, and its signature. */
static status_t send_message(const struct jwl_conn *c, const struct jwl_interface *iface,
                             uint16_t opcode, const struct jwl_message **m, struct jwl_sig *sig)
{
    bool client = c->side == JWL_CLIENT;
    if (!iface || opcode >= (client ? iface->nrequests : iface->nevents))
        return ERR_NOT_SUPPORTED;
    *m = &(client ? iface->requests : iface->events)[opcode];
    return jwl_sig_parse((*m)->signature, sig) == OK ? OK : ERR_INVALID_ARGS;
}

/* The object, its version, and every object argument live and of its type. */
static status_t send_checks(struct jwl_conn *c, const struct jwl_interface *iface, uint32_t id,
                            const struct jwl_message *m, const struct jwl_sig *sig,
                            const union jwl_arg *args)
{
    const struct jwl_object *o = jwl_map_get(&c->map, id);
    if (!o)
        return ERR_NOT_FOUND;
    if (!jwl_interface_same(o->iface, iface))
        return ERR_WRONG_TYPE;
    if (sig->since > o->version)
        return ERR_NOT_SUPPORTED;
    for (unsigned i = 0; i < sig->n; i++) {
        if (sig->type[i] != 'o' || !args[i].o)
            continue;
        const struct jwl_object *a = jwl_map_get(&c->map, args[i].o);
        const struct jwl_interface *t = m->types ? m->types[i] : NULL;
        if (!a || (t && !jwl_interface_same(a->iface, t)))
            return ERR_INVALID_ARGS;
    }
    return OK;
}

/* Each new id of the message, made in our range and written into args. */
static status_t make_ids(struct jwl_conn *c, const struct jwl_message *m,
                         const struct jwl_sig *sig, uint32_t version, union jwl_arg *args)
{
    for (unsigned i = 0; i < sig->n; i++) {
        if (sig->type[i] != 'n')
            continue;
        const struct jwl_interface *t = m->types ? m->types[i] : NULL;
        status_t st;
        if (t) {
            st = jwl_map_new(&c->map, t, version, NULL, &args[i].n);
        } else if (!args[i].any.iface || !args[i].any.version ||
                   args[i].any.version > args[i].any.iface->version) {
            st = ERR_INVALID_ARGS;
        } else {
            st = jwl_map_new(&c->map, args[i].any.iface, args[i].any.version, NULL,
                             &args[i].any.id);
        }
        if (st != OK) {
            for (unsigned j = 0; j < i; j++)
                if (sig->type[j] == 'n')
                    jwl_map_unmake(&c->map, m->types && m->types[j] ? args[j].n : args[j].any.id);
            return st;
        }
    }
    return OK;
}

static void unmake_ids(struct jwl_conn *c, const struct jwl_message *m, const struct jwl_sig *sig,
                       const union jwl_arg *args)
{
    for (unsigned i = 0; i < sig->n; i++)
        if (sig->type[i] == 'n')
            jwl_map_unmake(&c->map, m->types && m->types[i] ? args[i].n : args[i].any.id);
}

static status_t encode(struct jwl_conn *c, const struct jwl_message *m, uint32_t id,
                       uint16_t opcode, const union jwl_arg *args, unsigned nargs)
{
    for (int tries = 0; tries < 2; tries++) {
        struct jwl_out o = { .buf = c->out, .cap = JWL_BATCH_MAX, .len = c->out_len,
                             .h = c->out_h, .hcap = JWL_BATCH_HANDLES, .nh = c->out_nh };
        status_t st = jwl_encode(&o, m, id, opcode, args, nargs);
        if (st == OK) {
            c->out_len = (uint32_t)o.len;
            c->out_nh = o.nh;
            c->stats.msgs_out++;
            return OK;
        }
        if (st != ERR_BUFFER_TOO_SMALL || tries)
            return st;
        st = seal(c);   /* full: send what is there and start a new batch */
        if (st != OK)
            return st;
    }
    return ERR_INTERNAL;
}

static void close_arg_handles(const struct jwl_sig *sig, union jwl_arg *args, unsigned nargs)
{
    for (unsigned i = 0; i < sig->n && i < nargs; i++)
        if (sig->type[i] == 'h' && args[i].h != HANDLE_INVALID)
            jam_handle_close(args[i].h);
}

status_t jwl_conn_send(struct jwl_conn *c, const struct jwl_interface *iface, uint32_t id,
                       uint16_t opcode, union jwl_arg *args, unsigned nargs)
{
    const struct jwl_message *m;
    struct jwl_sig sig;
    status_t st = send_message(c, iface, opcode, &m, &sig);
    if (st != OK)
        return st;
    st = c->status;
    if (st == OK && nargs != sig.n)
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = send_checks(c, iface, id, m, &sig, args);
    if (st == OK)
        st = make_ids(c, m, &sig, jwl_map_get(&c->map, id)->version, args);
    if (st != OK) {
        close_arg_handles(&sig, args, nargs);
        return st;
    }
    st = encode(c, m, id, opcode, args, nargs);
    if (st != OK) {
        if (c->status == OK)
            unmake_ids(c, m, &sig, args);
        close_arg_handles(&sig, args, nargs);
    }
    return st;
}

status_t jwl_conn_delete(struct jwl_conn *c, uint32_t id)
{
    if (c->status != OK)
        return c->status;
    status_t st = jwl_map_remove(&c->map, id);
    if (st != OK || c->side != JWL_SERVER || id > JWL_CLIENT_ID_MAX)
        return st;
    union jwl_arg a = { .u = id };
    return jwl_conn_send(c, c->display, JWL_DISPLAY_ID, 1, &a, 1);
}

/* ---- errors ----------------------------------------------------------------------- */

status_t jwl_conn_post_error(struct jwl_conn *c, uint32_t object, uint32_t code, const char *text)
{
    if (c->side != JWL_SERVER)
        return ERR_NOT_SUPPORTED;
    if (c->status != OK)
        return c->status;
    c->error.object = object;
    c->error.code = code;
    /* printable ASCII only: the text may quote the client's own bytes */
    size_t i = 0;
    for (; text && text[i] && i + 1 < sizeof(c->error.text); i++)
        c->error.text[i] = text[i] >= 0x20 && text[i] < 0x7f ? text[i] : '?';
    c->error.text[i] = 0;
    die(c, ERR_INVALID_ARGS);   /* nothing not yet written goes */
    if (!jwl_map_get(&c->map, object))
        object = JWL_DISPLAY_ID;
    union jwl_arg a[3] = { { .o = object }, { .u = code }, { .s = c->error.text } };
    uint8_t buf[JWL_HEADER_BYTES + 32 + sizeof(c->error.text)];
    struct jwl_out o = { .buf = buf, .cap = sizeof(buf), .len = JWL_HEADER_BYTES };
    if (jwl_encode(&o, &c->display->events[0], JWL_DISPLAY_ID, 0, a, 3) == OK) {
        memset(buf, 0, JWL_HEADER_BYTES);
        wr32(buf, JWL_MAGIC);
        /* Past the window on purpose: one last small batch before the
         * close. If even that can't go, the close alone tells the client. */
        if (jam_channel_write(c->ch, buf, (uint32_t)o.len, NULL, 0) == OK)
            c->stats.batches_out++;
    }
    return c->status;
}
