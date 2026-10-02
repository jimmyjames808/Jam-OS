/* netlog's core (<netlog.h>): the datagrams, the two log sources, and
 * the go-back-N sender.
 *
 * The sender keeps no copy of what it sent: the log is its own buffer.
 * A stream's position is a position in the source (the kernel's byte
 * count for the live log), so going back is just reading again from the
 * acked offset, and whatever the ring dropped meanwhile shows up as a
 * read that starts later than asked (NETLOG_F_LOST). Acks are trusted
 * only as far as what was sent: an ack past the highest byte ever sent,
 * for another boot or stream, or of the wrong shape is ignored; an ack
 * below the last one (an old one, reordered) changes nothing. */
#include <crashlog.h>
#include <netlog.h>
#include <wire.h>

/* ---- the datagrams ---------------------------------------------------------------- */

static bool data_ok(const struct netlog_data *d)
{
    return d->stream < NETLOG_STREAMS && !(d->flags & ~NETLOG_F_ALL) &&
           d->length <= NETLOG_TEXT_MAX && (d->length || (d->flags & NETLOG_F_END)) &&
           d->acked <= d->offset;
}

status_t netlog_data_encode(const struct netlog_data *d, uint8_t *out, size_t *len)
{
    if (!data_ok(d) || (d->length && !d->text))
        return ERR_INVALID_ARGS;
    memset(out, 0, NETLOG_DATA_HDR);
    wire_put32(out, NETLOG_MAGIC);
    out[4] = NETLOG_VERSION;
    out[5] = NETLOG_DATA;
    out[6] = d->stream;
    out[7] = d->flags;
    wire_put64(out + 8, d->boot_id);
    wire_put64(out + 16, d->offset);
    wire_put64(out + 24, d->acked);
    wire_put32(out + 32, d->seq);
    wire_put16(out + 36, d->length);
    if (d->length)
        memmove(out + NETLOG_DATA_HDR, d->text, d->length);   /* text may already be there */
    *len = NETLOG_DATA_HDR + d->length;
    return OK;
}

status_t netlog_data_decode(const void *dgram, size_t len, struct netlog_data *out)
{
    const uint8_t *p = dgram;
    if (!p || len < NETLOG_DATA_HDR || len > NETLOG_DATA_MAX || wire_get32(p) != NETLOG_MAGIC ||
        p[4] != NETLOG_VERSION || p[5] != NETLOG_DATA || wire_get16(p + 38))
        return ERR_INVALID_ARGS;
    struct netlog_data d = {
        .stream = p[6], .flags = p[7], .boot_id = wire_get64(p + 8),
        .offset = wire_get64(p + 16), .acked = wire_get64(p + 24), .seq = wire_get32(p + 32),
        .length = wire_get16(p + 36),
        .text = p + NETLOG_DATA_HDR,
    };
    if (len != NETLOG_DATA_HDR + (size_t)d.length || !data_ok(&d) ||
        d.offset > UINT64_MAX - d.length)
        return ERR_INVALID_ARGS;
    *out = d;
    return OK;
}

void netlog_ack_encode(uint8_t stream, uint64_t boot_id, uint64_t acked, uint8_t *out)
{
    memset(out, 0, NETLOG_ACK_SIZE);
    wire_put32(out, NETLOG_MAGIC);
    out[4] = NETLOG_VERSION;
    out[5] = NETLOG_ACK;
    out[6] = stream;
    wire_put64(out + 8, boot_id);
    wire_put64(out + 16, acked);
}

status_t netlog_ack_decode(const void *dgram, size_t len, uint8_t *stream, uint64_t *boot_id,
                           uint64_t *acked)
{
    const uint8_t *p = dgram;
    if (!p || len != NETLOG_ACK_SIZE || wire_get32(p) != NETLOG_MAGIC ||
        p[4] != NETLOG_VERSION || p[5] != NETLOG_ACK || p[6] >= NETLOG_STREAMS || p[7])
        return ERR_INVALID_ARGS;
    *stream = p[6];
    *boot_id = wire_get64(p + 8);
    *acked = wire_get64(p + 16);
    return OK;
}

/* ---- the sources ------------------------------------------------------------------ */

static int64_t klog_src_read(void *ctx, uint64_t pos, void *buf, uint64_t cap, uint64_t *first)
{
    return jam_klog_read((handle_t)(uintptr_t)ctx, pos, buf, cap, first);
}

void netlog_klog_source(handle_t reader, struct netlog_source *out)
{
    *out = (struct netlog_source){
        .ctx = (void *)(uintptr_t)reader, .read = klog_src_read, .end = UINT64_MAX,
    };
}

static int64_t vmo_src_read(void *ctx, uint64_t pos, void *buf, uint64_t cap, uint64_t *first)
{
    const struct netlog_vmo_ctx *v = ctx;
    *first = pos;
    if (pos >= v->len)
        return 0;
    uint64_t n = v->len - pos < cap ? v->len - pos : cap;
    status_t st = jam_vmo_read(v->vmo, v->base + pos, buf, n);
    return st == OK ? (int64_t)n : st;
}

status_t netlog_crash_source(handle_t vmo, struct netlog_vmo_ctx *ctx,
                             struct netlog_source *out, char name[32])
{
    struct crashlog_header h;
    uint64_t size = 0;
    status_t st = jam_vmo_get_size(vmo, &size);
    if (st == OK)
        st = size < sizeof(h) ? ERR_INVALID_ARGS : jam_vmo_read(vmo, 0, &h, sizeof(h));
    if (st != OK || !crashlog_header_ok(&h, size))
        return ERR_INVALID_ARGS;
    _Static_assert(sizeof(h.name) <= 32, "the name fits the caller's buffer");
    memcpy(name, h.name, sizeof(h.name));
    *ctx = (struct netlog_vmo_ctx){ .vmo = vmo, .base = sizeof(h), .len = h.text_len };
    *out = (struct netlog_source){ .ctx = ctx, .read = vmo_src_read, .end = h.text_len };
    return OK;
}

/* ---- the sender ------------------------------------------------------------------- */

/* One line about a change of state, with one number in it. */
static void say(struct netlog *n, const char *fmt, unsigned long v)
{
    char line[96];
    snprintf(line, sizeof(line), fmt, v);
    n->io.say(n->io.ctx, line);
}

static void stream_on(struct netlog_stream *s, const struct netlog_source *src)
{
    memset(s, 0, sizeof(*s));
    s->on = true;
    s->src = *src;
    s->done = src->end == 0;   /* an empty crash log: nothing to send */
}

void netlog_start(struct netlog *n, const struct netlog_io *io, uint64_t boot_id,
                  const struct netlog_source *live)
{
    memset(n, 0, sizeof(*n));
    n->io = *io;
    n->boot_id = boot_id;
    n->wait = NETLOG_RESEND;
    stream_on(&n->s[NETLOG_LIVE], live);
}

void netlog_add_crash(struct netlog *n, const struct netlog_source *crash)
{
    stream_on(&n->s[NETLOG_CRASH], crash);
}

static bool unacked(const struct netlog *n)
{
    for (unsigned i = 0; i < NETLOG_STREAMS; i++)
        if (n->s[i].on && !n->s[i].done && n->s[i].high > n->s[i].acked)
            return true;
    return false;
}

/* No progress for the whole wait: every stream back to its acked offset,
 * the wait doubled; said once when the Mac has gone quiet. */
static void go_back(struct netlog *n)
{
    for (unsigned i = 0; i < NETLOG_STREAMS; i++)
        n->s[i].next = n->s[i].acked;
    n->resent_rounds++;
    n->retry_at = 0;
    n->wait = n->wait * 2 > NETLOG_WAIT_MAX ? NETLOG_WAIT_MAX : n->wait * 2;
    if (++n->quiet == NETLOG_SILENT_AFTER && !n->silent) {
        n->silent = true;
        say(n, "netlog: the Mac doesn't answer: trying again, up to every %lu s",
            (unsigned long)(NETLOG_WAIT_MAX / NS_PER_S));
    }
}

/* Cut a full read after its last newline, so a datagram holds whole lines
 * when it can. */
static int64_t whole_lines(const uint8_t *text, int64_t got)
{
    for (int64_t i = got; i > 0; i--)
        if (text[i - 1] == '\n')
            return i;
    return got;
}

/* One datagram of stream i from s->next, if there is text and room.
 * false: nothing to send now. */
static bool send_one(struct netlog *n, unsigned i)
{
    struct netlog_stream *s = &n->s[i];
    uint64_t window = n->silent ? NETLOG_TEXT_MAX : NETLOG_WINDOW;
    if (s->next - s->acked >= window || s->next >= s->src.end)
        return false;
    uint64_t cap = window - (s->next - s->acked);
    cap = cap < NETLOG_TEXT_MAX ? cap : NETLOG_TEXT_MAX;
    cap = s->src.end - s->next < cap ? s->src.end - s->next : cap;
    uint8_t *text = n->buf + NETLOG_DATA_HDR;
    uint64_t first = s->next;
    int64_t got = s->src.read(s->src.ctx, s->next, text, cap, &first);
    if (got <= 0 || (uint64_t)got > cap || first < s->next || first > s->src.end - (uint64_t)got)
        return false;   /* nothing yet (or a broken source: nothing believed) */
    if (first > s->next) {
        n->lost_bytes += first - s->next;
        s->lost = true;
    }
    if ((uint64_t)got == cap && first + (uint64_t)got < s->src.end)
        got = whole_lines(text, got);
    struct netlog_data d = {
        .stream = (uint8_t)i, .boot_id = n->boot_id, .offset = first, .acked = s->acked,
        .seq = s->seq++,
        .length = (uint16_t)got, .text = text,
        .flags = (uint8_t)((s->lost ? NETLOG_F_LOST : 0) |
                           (first + (uint64_t)got == s->src.end ? NETLOG_F_END : 0)),
    };
    size_t len = 0;
    if (netlog_data_encode(&d, n->buf, &len) == OK)
        (void)n->io.send(n->io.ctx, n->buf, len);   /* a failed send is a lost datagram */
    n->sent++;
    s->lost = false;
    s->next = first + (uint64_t)got;
    if (s->next > s->high)
        s->high = s->next;
    return true;
}

uint64_t netlog_poll(struct netlog *n, uint64_t now)
{
    if (n->retry_at && now >= n->retry_at)
        go_back(n);
    unsigned budget = NETLOG_BURST;
    bool more = false;
    for (unsigned i = 0; i < NETLOG_STREAMS; i++) {
        if (!n->s[i].on || n->s[i].done)
            continue;
        while (budget && send_one(n, i))
            budget--;
        more |= !budget;
    }
    if (!n->retry_at && unacked(n))
        n->retry_at = now + n->wait;
    uint64_t next = more ? now + NETLOG_PACE : UINT64_MAX;
    return n->retry_at && n->retry_at < next ? n->retry_at : next;
}

void netlog_ack(struct netlog *n, const void *dgram, size_t len, uint64_t now)
{
    uint8_t i = 0;
    uint64_t boot = 0, acked = 0;
    if (netlog_ack_decode(dgram, len, &i, &boot, &acked) != OK || boot != n->boot_id ||
        !n->s[i].on || acked > n->s[i].high) {
        n->ignored++;
        return;
    }
    struct netlog_stream *s = &n->s[i];
    n->acks++;
    n->quiet = 0;
    n->wait = NETLOG_RESEND;   /* the Mac is there: what is missing was lost on the way */
    if (n->silent) {
        n->silent = false;
        n->io.say(n->io.ctx, "netlog: the Mac answers again");
    }
    if (acked <= s->acked) {
        /* nothing new (a repeat: the Mac waits for a lost datagram; or an
         * old ack overtaken): go back no later than the first wait */
        if (n->retry_at > now + n->wait)
            n->retry_at = now + n->wait;
        return;
    }
    s->acked = acked;
    if (s->next < acked)
        s->next = acked;
    n->retry_at = unacked(n) ? now + n->wait : 0;
    if (acked == s->src.end && !s->done) {
        s->done = true;
        say(n, "netlog: the last boot's log is sent (%lu bytes)", (unsigned long)acked);
    }
}
