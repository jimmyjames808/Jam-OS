/* hda: the driver's loop. One thread, one port; everything it waits for
 * arrives there:
 *   KEY_SERVE   DR_SERVE (devmgr's channel, shared by every client):
 *               dump through the caller's ops, open_output and query here
 *   KEY_QUERY   a query channel (hda.query's): the caller's ops, but
 *               open_output and query are refused, so its holder (the
 *               shell's `hda`, through the mixer) can't take the stream.
 *               The key carries the slot and a generation
 *   KEY_STREAM  the open stream's channel (open_output's `stream`):
 *               start, stop, position, wait_period; its peer closing
 *               closes the stream. The key carries a generation, so a
 *               packet for a channel already closed is ignored.
 *   KEY_IRQ     the controller's MSI (DR_IRQ(0)), bound PERSISTENT: acked,
 *               then INTSTS read: the stream's status cleared (SIS bit),
 *               the RIRB drained (CIS)
 *
 * The controller's one MSI carries stream buffer completions (one per
 * period while the stream runs) and RIRB responses. Commands are still
 * answered by polling (ctrl.c: a verb takes microseconds); the RIRB
 * interrupt (INTCTL.CIE, on from the loop's start when the rings and the
 * MSI are there) is for what no command waits for: unsolicited responses,
 * the jacks' (jack.c). Since RINTCNT is 1 it also fires for every answer,
 * which the drain finds already taken; that costs a wake per verb, and
 * proves the interrupt works before any jack needs it.
 *
 * The jacks run at every turn of the loop: the unsolicited responses
 * queued, the debounce reads due and the poll. Their deadline is part of
 * the port wait's.
 *
 * Every wait has a deadline: while the stream runs the port wait ends at
 * least once a period, so a lost interrupt costs a period of latency and
 * the position is read often enough never to wrap unseen. wait_period is
 * answered from the loop (its reply is deferred until the period has
 * played), bounded by STALL_PERIODS without progress. */
#include <idl/hda.h>
#include "hda.h"

#define KEY_SERVE  1u
#define KEY_IRQ    2u
#define KEY_STREAM 3u          /* | generation << 8 */
#define KEY_QUERY  4u          /* | slot << 8 | generation << 16 */
#define DRAIN_MAX  64          /* messages taken from one channel per turn */
#define QUERY_MAX  8           /* query channels open at once (hda.idl) */

/* Which channel a message came on: what it may ask. */
enum chan_kind {
    CH_SERVE,    /* DR_SERVE: everything but the stream's methods */
    CH_STREAM,   /* the stream channel: start, stop, position, wait_period */
    CH_QUERY,    /* a query channel: DR_SERVE's but open_output and query */
};

struct query_ch {
    handle_t ch;               /* our end, or HANDLE_INVALID: a free slot */
    uint64_t key;              /* its port key */
    bool     pending;          /* it may have messages */
};

struct loop {
    struct hda   *h;
    struct stream st;
    const struct hda_ops *ops; /* the caller's (dump), with its ctx */
    void         *ctx;
    handle_t      port, irq;   /* irq: DR_IRQ(0), or HANDLE_INVALID (polled) */
    handle_t      serve;       /* DR_SERVE */
    handle_t      stream_ch;   /* our end of the open stream's channel, or HANDLE_INVALID */
    uint32_t      gen;         /* the stream channel's generation (its port key) */
    bool          serve_pending, stream_pending, serve_closed;
    struct query_ch query[QUERY_MAX];   /* hda.query's channels */
    uint32_t      query_gen;   /* the last query channel's generation (its port key) */
    /* a wait_period waiting for its period (one at a time) */
    bool          waiting;
    uint32_t      wait_txid;
    uint64_t      wait_until;  /* frames */
    uint64_t      irqs, stream_irqs, rirb_irqs, spurious;
    struct jacks *js;
    struct jack_io io;
};

/* ---- the stream channel ----------------------------------------------------- */

static void stream_ch_close(struct loop *l, const char *why)
{
    if (l->stream_ch == HANDLE_INVALID)
        return;
    drv_handle_close(l->stream_ch);   /* its port binding goes with it */
    l->stream_ch = HANDLE_INVALID;
    l->stream_pending = false;
    l->waiting = false;
    l->gen++;
    stream_close(l->h, &l->st, why);
}

static status_t do_open_output(void *ctx, uint32_t rate, uint8_t channels, uint8_t bits,
                               handle_t *out_stream, handle_t *out_ring, uint32_t *out_size,
                               uint32_t *out_period)
{
    struct loop *l = ctx;
    if (rate != STREAM_RATE || channels != 2)
        return ERR_NOT_SUPPORTED;
    if (l->st.open)
        return ERR_BAD_STATE;
    handle_t ours, theirs;
    status_t st = drv_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    l->gen++;
    st = drv_port_bind(l->port, ours, KEY_STREAM | (uint64_t)l->gen << 8,
                       SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = stream_open(l->h, &l->st, bits, out_ring);
    if (st != OK) {
        drv_handle_close(ours);
        drv_handle_close(theirs);
        return st;
    }
    l->stream_ch = ours;
    *out_stream = theirs;
    *out_size = l->st.ring_bytes;
    *out_period = l->st.period_bytes;
    return OK;
}

static status_t do_start(void *ctx)
{
    struct loop *l = ctx;
    return stream_start(l->h, &l->st);
}

static status_t do_stop(void *ctx)
{
    struct loop *l = ctx;
    return stream_stop(l->h, &l->st);
}

static status_t do_position(void *ctx, uint64_t *out_frames, uint32_t *out_offset)
{
    struct loop *l = ctx;
    stream_update(l->h, &l->st);
    *out_frames = l->st.played / l->st.frame_bytes;
    *out_offset = l->st.last_off;
    return OK;
}

/* ---- query channels ------------------------------------------------------------ */

/* hda.query: a channel of our own that answers the caller's ops alone. */
static status_t do_query(void *ctx, handle_t *out_channel)
{
    struct loop *l = ctx;
    struct query_ch *q = NULL;
    for (unsigned i = 0; i < QUERY_MAX && !q; i++)
        if (l->query[i].ch == HANDLE_INVALID)
            q = &l->query[i];
    if (!q)
        return ERR_NO_RESOURCES;
    handle_t ours, theirs;
    status_t st = drv_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    uint64_t key = KEY_QUERY | (uint64_t)(q - l->query) << 8 | (uint64_t)++l->query_gen << 16;
    st = drv_port_bind(l->port, ours, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_handle_close(ours);
        drv_handle_close(theirs);
        return st;
    }
    *q = (struct query_ch){ .ch = ours, .key = key };
    *out_channel = theirs;
    return OK;
}

static void query_close(struct query_ch *q)
{
    if (q->ch == HANDLE_INVALID)
        return;
    drv_handle_close(q->ch);   /* its port binding goes with it */
    *q = (struct query_ch){ .ch = HANDLE_INVALID };
}

static const struct hda_ops serve_ops = { .open_output = do_open_output, .query = do_query };
static const struct hda_ops stream_ops = {
    .start = do_start,
    .stop = do_stop,
    .position = do_position,
};

/* ---- wait_period ---------------------------------------------------------------- */

static void wait_reply(struct loop *l, status_t st)
{
    struct hda_wait_period_rep r = { .txid = l->wait_txid, .status = st };
    uint32_t n = sizeof(struct idl_rep_hdr);
    if (st == OK) {
        r.frames = l->st.played / l->st.frame_bytes;
        r.offset = l->st.last_off;
        n = sizeof(r);
    }
    l->waiting = false;
    (void)drv_channel_write(l->stream_ch, &r, n, NULL, 0);   /* the client gone: nobody waits */
}

/* Answer the waiting wait_period if its period has played, the stream
 * stopped, or it made no progress for STALL_PERIODS. */
static void wait_check(struct loop *l)
{
    if (!l->waiting)
        return;
    if (l->st.played / l->st.frame_bytes >= l->wait_until) {
        wait_reply(l, OK);
    } else if (!l->st.running) {
        wait_reply(l, ERR_BAD_STATE);
    } else if (drv_clock_ns() - l->st.progress_ns > STALL_PERIODS * PERIOD_NS) {
        drv_log("stream: no progress for %u periods: the DMA engine stalled", STALL_PERIODS);
        wait_reply(l, ERR_TIMED_OUT);
    }
}

static void wait_begin(struct loop *l, const struct hda_wait_period_req *q)
{
    uint64_t pf = PERIOD_FRAMES;
    if (l->waiting || !l->st.running) {
        idl_reply_status(l->stream_ch, q, sizeof(*q), ERR_BAD_STATE);
        return;
    }
    if (q->after > UINT64_MAX - pf) {
        idl_reply_status(l->stream_ch, q, sizeof(*q), ERR_INVALID_ARGS);
        return;
    }
    l->waiting = true;
    l->wait_txid = q->txid;
    l->wait_until = (q->after / pf + 1) * pf;
    stream_update(l->h, &l->st);
    wait_check(l);
}

/* ---- serving ------------------------------------------------------------------- */

/* One message from ch, a channel of that kind: OK once handled, else
 * drv_channel_read's status. */
static status_t serve_one(struct loop *l, handle_t ch, enum chan_kind kind)
{
    bool stream = kind == CH_STREAM;
    _Alignas(8) uint8_t q[HDA_REQ_MAX + 8];
    _Alignas(8) uint8_t r[HDA_REP_MAX];
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *hdr = (const void *)q;
    if (stream && n == sizeof(struct hda_wait_period_req) && hdr->ordinal == HDA_WAIT_PERIOD) {
        wait_begin(l, (const void *)q);
        return OK;
    }
    const struct hda_ops *ops = stream ? &stream_ops : l->ops;
    void *ctx = stream ? (void *)l : l->ctx;
    bool ours = n >= sizeof(*hdr) && (hdr->ordinal == HDA_OPEN_OUTPUT ||
                                      hdr->ordinal == HDA_QUERY);
    if (ours && kind == CH_QUERY) {
        idl_reply_status(ch, q, n, ERR_ACCESS_DENIED);   /* never the stream */
        return OK;
    }
    if (ours && kind == CH_SERVE) {
        ops = &serve_ops;
        ctx = l;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = hda_dispatch(ops, ctx, q, n, r, rhs, &rhn);
    if (!rn || drv_channel_write(ch, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: they're still ours */
    return OK;
}

/* Up to DRAIN_MAX messages from DR_SERVE, the stream channel or query
 * channel q (kind CH_QUERY). */
static void serve_some(struct loop *l, enum chan_kind kind, struct query_ch *q)
{
    handle_t ch = kind == CH_STREAM ? l->stream_ch : kind == CH_QUERY ? q->ch : l->serve;
    bool *pending = kind == CH_STREAM ? &l->stream_pending
                  : kind == CH_QUERY  ? &q->pending : &l->serve_pending;
    *pending = false;
    for (int i = 0; i < DRAIN_MAX; i++) {
        status_t st = serve_one(l, ch, kind);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED && kind == CH_STREAM)
            stream_ch_close(l, "the client closed its channel");
        else if (st == ERR_PEER_CLOSED && kind == CH_QUERY)
            query_close(q);
        else if (st == ERR_PEER_CLOSED)
            l->serve_closed = true;
        else if (st != ERR_SHOULD_WAIT)
            drv_log("reading a channel: %s", status_str(st));
        return;
    }
    *pending = true;   /* more may be queued: the binding fires on edges only */
}

/* The query channels with messages; true if any may have more. */
static bool serve_queries(struct loop *l)
{
    bool more = false;
    for (unsigned i = 0; i < QUERY_MAX; i++) {
        struct query_ch *q = &l->query[i];
        if (q->ch != HANDLE_INVALID && q->pending)
            serve_some(l, CH_QUERY, q);
        more |= q->ch != HANDLE_INVALID && q->pending;
    }
    return more;
}

/* ---- the loop ------------------------------------------------------------------ */

/* The MSI: acked first (a fire after this queues a new packet), then the
 * controller's status read and cleared. */
static void irq_take(struct loop *l, const struct port_packet *p)
{
    l->irqs += p->signal.count;
    (void)drv_interrupt_ack(l->irq);
    uint32_t is = drv_read32(l->h->regs, HDA_INTSTS);
    bool stream = l->st.open && l->st.sd < HDA_MAX_STREAMS && (is & (1u << l->st.sd));
    if (stream) {
        stream_status(l->h, &l->st);
        l->stream_irqs++;
    }
    /* CIS, or no bit at all while CIE is the only other source: an answer
     * a command polled for took RINTFL before this ran. Either way the
     * RIRB's interrupt reached here, which is what jack.c waits to see. */
    if ((is & INTSTS_CIS) || (!stream && l->h->unsol_on)) {
        hda_rirb_irq(l->h);
        l->rirb_irqs++;
        l->js->irq_seen++;
    } else if (!stream) {
        l->spurious++;
    }
}

static void packet(struct loop *l, const struct port_packet *p)
{
    if (p->key == KEY_SERVE)
        l->serve_pending = true;
    else if (p->key == KEY_IRQ)
        irq_take(l, p);
    else if (p->key == (KEY_STREAM | (uint64_t)l->gen << 8) && l->stream_ch != HANDLE_INVALID)
        l->stream_pending = true;
    else if ((p->key & 0xff) == KEY_QUERY)
        for (unsigned i = 0; i < QUERY_MAX; i++)
            if (l->query[i].ch != HANDLE_INVALID && l->query[i].key == p->key)
                l->query[i].pending = true;   /* else a closed one's: stale */
}

static status_t setup(struct loop *l, const struct driver_start *ds)
{
    status_t st = drv_port_create(&l->port);
    if (st == OK)
        st = drv_port_bind(l->port, l->serve, KEY_SERVE, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_log("can't set up the port (%s)", status_str(st));
        return st;
    }
    l->irq = drv_handle(ds, DR_IRQ(0));
    if (l->irq != HANDLE_INVALID &&
        drv_port_bind(l->port, l->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT) != OK)
        l->irq = HANDLE_INVALID;
    if (l->irq == HANDLE_INVALID)
        drv_log("no interrupt: the stream is polled once a period, the jacks every %u ms",
                (unsigned)(JACK_POLL_NS / NS_PER_MS));
    bool unsol = l->h->rings && l->irq != HANDLE_INVALID;
    if (unsol)
        hda_unsol_enable(l->h, true);
    hda_jack_io(&l->io, l->h);
    hda_jacks_start(l->js, &l->io, unsol, drv_clock_ns());
    l->serve_pending = true;   /* something may be queued already */
    return OK;
}

status_t hda_loop(struct hda *h, const struct driver_start *ds, const struct hda_ops *ops,
                  void *ctx, struct output *out, struct jacks *js)
{
    struct loop *l = drv_malloc(sizeof(*l));
    if (!l)
        return ERR_NO_MEMORY;
    *l = (struct loop){ .h = h, .ops = ops, .ctx = ctx, .serve = drv_handle(ds, DR_SERVE),
                        .js = js };
    for (unsigned i = 0; i < QUERY_MAX; i++)
        l->query[i].ch = HANDLE_INVALID;
    stream_init(h, &l->st, drv_handle(ds, DR_PCIDEV), out);
    status_t st = setup(l, ds);
    while (st == OK) {
        if (l->serve_pending)
            serve_some(l, CH_SERVE, NULL);
        if (l->serve_closed)
            break;
        if (l->stream_pending)
            serve_some(l, CH_STREAM, NULL);
        bool queries = serve_queries(l);
        stream_update(h, &l->st);
        wait_check(l);
        hda_jacks_run(js, &l->io, drv_clock_ns());
        if (l->serve_pending || l->stream_pending || queries)
            continue;
        uint64_t deadline = l->st.running ? drv_clock_ns() + PERIOD_NS : DEADLINE_NEVER;
        uint64_t jd = hda_jacks_deadline(js);
        if (jd < deadline)
            deadline = jd;
        struct port_packet p;
        st = drv_port_wait(l->port, deadline, &p);
        if (st == OK)
            packet(l, &p);
        else if (st == ERR_TIMED_OUT)
            st = OK;
    }
    stream_ch_close(l, "the driver is stopping");
    for (unsigned i = 0; i < QUERY_MAX; i++)
        query_close(&l->query[i]);   /* their holders see ERR_PEER_CLOSED */
    if (l->io.set)
        hda_jacks_stop(js, &l->io);
    hda_unsol_enable(h, false);
    if (l->port)
        drv_handle_close(l->port);
    drv_log("%lu interrupt(s): %lu the stream's, %lu the RIRB's, %lu neither; %u unsolicited "
            "response(s), %u late answer(s)", (unsigned long)l->irqs,
            (unsigned long)l->stream_irqs, (unsigned long)l->rirb_irqs,
            (unsigned long)l->spurious, h->unsol, h->late);
    drv_free(l);
    return st;
}
