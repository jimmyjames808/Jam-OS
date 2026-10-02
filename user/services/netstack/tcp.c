/* netstack's TCP connections: tcp.h has the model (the window is the rx
 * ring's free room; bytes go into lwIP only as the peer's window allows;
 * END is FIN both ways).
 *
 * Connections and listeners live in fixed tables (TCP_CONNS,
 * TCP_LISTENERS). Work is noted on a list by the hooks and by ntcp_kick,
 * and done by ntcp_work from the loop: each listed connection once a turn,
 * with what its tx ring held when it was looked at. Two rarer waits are
 * found by a scan of the table instead of the list, so that they never
 * keep the loop awake: a connection whose FIN is out but not yet acked
 * after the peer's came (lwIP says nothing when an ACK only moves it from
 * CLOSING to TIME_WAIT), looked at after every turn; and one whose bytes
 * found lwIP's heap or segments full, looked at again after the next ack
 * anywhere freed some.
 *
 * Every count the program writes is read through sockring.c's clamps; one
 * out of range (sockring_end's `errors`) means the stream's bytes can't be
 * trusted any more, so the connection is reset (ERR_OUT_OF_RANGE). */
#include <os.h>
#include <sockring.h>
#include "stack.h"
#include "tcp.h"

_Static_assert(SOCKRING_MIN / 2 > STACK_TCP_MSS, "the smallest window, halved, holds a segment");

static struct ntcp_conn     conns[TCP_CONNS];
static struct ntcp_listener listeners[TCP_LISTENERS];
static struct ntcp_conn    *work_head, *work_tail;   /* noted work, oldest first */
static unsigned            nclosing;   /* connections with `closing` (their FIN's ack awaited) */
static unsigned            nretry;     /* connections with `retry` */
static bool                freed;      /* an ack or an end freed lwIP memory since the last turn */
static uint8_t             chunk[TCP_CHUNK];   /* bytes on their way from a tx ring into lwIP */
static struct ntcp_counts  counts;     /* since netstack started */

/* ---- small helpers ---------------------------------------------------------- */

static uint32_t min32(uint64_t a, uint64_t b)
{
    return (uint32_t)(a < b ? a : b);
}

static void note(struct ntcp_conn *c)
{
    if (c->listed)
        return;
    c->listed = true;
    c->next = NULL;
    if (work_tail)
        work_tail->next = c;
    else
        work_head = c;
    work_tail = c;
}

static void unlist(struct ntcp_conn *c)
{
    if (!c->listed)
        return;
    struct ntcp_conn **pp = &work_head, *prev = NULL;
    while (*pp && *pp != c) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    if (*pp)
        *pp = c->next;
    if (work_tail == c)
        work_tail = prev;
    c->listed = false;
    c->next = NULL;
}

static void closing_set(struct ntcp_conn *c, bool on)
{
    if (c->closing != on)
        nclosing = on ? nclosing + 1 : nclosing - 1;
    c->closing = on;
}

static void retry_set(struct ntcp_conn *c, bool on)
{
    if (c->retry != on)
        nretry = on ? nretry + 1 : nretry - 1;
    c->retry = on;
}

static void signal_prog(struct ntcp_conn *c, uint32_t bits)
{
    c->signalled |= bits;
    if (c->to_prog)   /* a program that is gone has nobody to tell */
        (void)jam_event_signal(c->to_prog, 0, bits);
}

static void state_set(struct ntcp_conn *c, uint32_t state, status_t error)
{
    if (c->st.state == state && c->st.error == error)
        return;
    c->st.state = state;
    c->st.error = error;
    c->st.changes++;
    if (c->r.page)
        sockring_status_put(&c->r, &c->st);
    signal_prog(c, SOCKRING_SIG_STATE);
}

/* Did the program's counts go wrong (sockring_end's errors)? The status
 * line says how often. */
static bool ring_bad(struct ntcp_conn *c)
{
    uint64_t e = c->r.tx.errors + c->r.rx.errors;
    if (e == c->st.ring_errors)
        return e != 0;
    c->st.ring_errors = e;
    sockring_status_put(&c->r, &c->st);
    return true;
}

/* ---- in: the rx ring and the window ------------------------------------------ */

/* Give the window back what the program took from the rx ring since the
 * last look. */
static void give_back(struct ntcp_conn *c)
{
    if (!c->t)
        return;
    uint64_t unread = c->r.rx.size - sockring_room(&c->r.rx);   /* a bad count: all unread */
    uint64_t taken = c->r.rx.count > unread ? c->r.rx.count - unread : 0;
    if (taken <= c->given_back)
        return;
    stack_tcp_recved(c->t, taken - c->given_back);
    c->given_back = taken;
}

/* Keep the rx producer's `waits` up while the window left is under half
 * a window plus a segment: the program's reads signal SOCKRING_SIG_RX_ROOM
 * then, and each is given back at once. lwIP tells the peer as soon as
 * the window opened by a segment (lwipopts.h), so with the flag down the
 * peer knows of half a window at least and keeps sending, and its bytes
 * bring netstack back here. Twice at most: a read between the two looks
 * is given back and looked at again. */
static void watch_rx(struct ntcp_conn *c)
{
    uint32_t need = c->r.rx.size - (c->window / 2 - STACK_TCP_MSS);   /* room under this: watch */
    for (int i = 0; i < 2; i++) {
        if (sockring_room(&c->r.rx) >= need) {
            sockring_awake(&c->r.rx);
            return;
        }
        if (sockring_sleep(&c->r.rx, need))
            return;
        give_back(c);
    }
}

static size_t on_rx(void *ctx, const uint8_t *data, size_t n)
{
    struct ntcp_conn *c = ctx;
    if (!c->r.page || c->rx_ended)
        return 0;
    give_back(c);
    if (ring_bad(c)) {
        note(c);   /* reset from the loop, not from inside lwIP */
        return 0;
    }
    uint32_t k = sockring_stream_write(&c->r.rx, data, min32(n, UINT32_MAX));
    counts.bytes_in += k;
    if (k && sockring_publish(&c->r.rx))
        signal_prog(c, SOCKRING_SIG_RX);
    watch_rx(c);
    return k;
}

static void on_rx_end(void *ctx)
{
    struct ntcp_conn *c = ctx;
    c->rx_ended = true;
    if (c->r.page && sockring_finish(&c->r.rx))
        signal_prog(c, SOCKRING_SIG_RX);
    note(c);
}

/* ---- out: the tx ring into lwIP ------------------------------------------------ */

/* Bytes that were ready when it looked, as far as lwIP takes them; then
 * the FIN once the program's END is all that is left. */
static void pump_tx(struct ntcp_conn *c)
{
    if (!c->t || !c->connected || c->fin_sent)
        return;
    uint32_t budget = sockring_ready(&c->r.tx);
    if (ring_bad(c)) {   /* before a byte of it is sent */
        ntcp_conn_abort(c, ERR_OUT_OF_RANGE);
        return;
    }
    bool moved = false;
    while (budget) {
        uint32_t n = min32(min32(budget, stack_tcp_room(c->t)), sizeof(chunk));
        if (!n)
            break;
        n = sockring_stream_read(&c->r.tx, chunk, n);
        status_t st = stack_tcp_send(c->t, chunk, n);
        if (st != OK) {
            c->r.tx.count -= n;   /* not taken after all: nothing was published yet */
            retry_set(c, st == ERR_NO_MEMORY);
            break;
        }
        budget -= n;
        counts.bytes_out += n;
        moved = true;
    }
    if (moved && sockring_publish(&c->r.tx))
        signal_prog(c, SOCKRING_SIG_TX_ROOM);
    if (sockring_at_end(&c->r.tx)) {
        status_t st = stack_tcp_shutdown(c->t);
        c->fin_sent = st == OK;
        retry_set(c, st == ERR_NO_MEMORY);
    } else if (sockring_ready(&c->r.tx)) {
        /* Bytes left: published after it looked (the next turn takes
         * them), or waiting for the peer's window or lwIP's memory (an ack
         * comes back here). */
        sockring_awake(&c->r.tx);
        if (!c->retry && stack_tcp_room(c->t))
            note(c);
    } else if (!sockring_sleep(&c->r.tx, 1)) {
        note(c);   /* empty, so the program's next publish signals; it just did */
    }
    if (moved || c->fin_sent)
        stack_tcp_push(c->t);
}

/* ---- the connection's life -------------------------------------------------------- */

/* Both FINs went and ours was acked: CLOSED, and lwIP's side let go of
 * (TIME_WAIT is lwIP's). Until the ack, looked at again every turn. */
static void closed_check(struct ntcp_conn *c)
{
    if (!c->t || !c->rx_ended || !c->fin_sent)
        return;
    if (!stack_tcp_fin_acked(c->t)) {
        closing_set(c, true);
        return;
    }
    closing_set(c, false);
    stack_tcp_release(c->t);
    c->t = NULL;
    state_set(c, SOCKRING_STATE_CLOSED, OK);
}

static void conn_work(struct ntcp_conn *c)
{
    if (!c->r.page || !c->t)
        return;
    give_back(c);
    if (ring_bad(c)) {
        ntcp_conn_abort(c, ERR_OUT_OF_RANGE);
        return;
    }
    watch_rx(c);
    pump_tx(c);
    closed_check(c);
}

static void on_connected(void *ctx)
{
    struct ntcp_conn *c = ctx;
    c->connected = true;
    state_set(c, SOCKRING_STATE_OPEN, OK);
    note(c);
}

static void on_sent(void *ctx)
{
    freed = true;
    note(ctx);
}

static void queue_drop(struct ntcp_listener *l, struct ntcp_conn *c);
static void slot_clear(struct ntcp_conn *c);

static void on_gone(void *ctx, status_t why)
{
    struct ntcp_conn *c = ctx;
    c->t = NULL;
    freed = true;
    closing_set(c, false);
    retry_set(c, false);
    if (c->queued_on) {   /* the program never saw it */
        struct ntcp_listener *l = c->queued_on;
        queue_drop(l, c);
        l->req.drop(l, c);
        slot_clear(c);
        return;
    }
    if (why == ERR_PEER_CLOSED && !c->connected)
        why = ERR_NOT_FOUND;   /* a reset for our SYN: nobody listens there */
    state_set(c, SOCKRING_STATE_CLOSED, why);
}

/* ---- listeners ------------------------------------------------------------------- */

static void queue_drop(struct ntcp_listener *l, struct ntcp_conn *c)
{
    unsigned k = 0;
    for (unsigned i = 0; i < l->n; i++) {
        struct ntcp_conn *x = l->q[(l->head + i) % STACK_TCP_BACKLOG];
        if (x != c)
            l->q[(l->head + k++) % STACK_TCP_BACKLOG] = x;
    }
    l->n = k;
    c->queued_on = NULL;
}

/* lwIP finished a handshake on listener lctx: a slot and rings for it,
 * and into the listener's queue. NULL refuses it (lwIP resets it). */
static void *on_accepted(void *lctx, struct stack_tcp *t)
{
    struct ntcp_listener *l = lctx;
    struct ntcp_conn *c;
    if (l->n >= STACK_TCP_BACKLOG || ntcp_conn_new(l->req.owner, &c) != OK) {
        l->refused++;
        return NULL;
    }
    c->t = t;   /* the rings function may ask stack_tcp_ends */
    if (l->req.rings(l, c) != OK) {
        slot_clear(c);
        l->refused++;
        return NULL;
    }
    if (!c->r.page || c->r.rx.size < l->req.rx_size) {   /* the window was announced already */
        l->req.drop(l, c);
        slot_clear(c);
        l->refused++;
        return NULL;
    }
    c->connected = true;
    c->window = min32(l->req.rx_size, STACK_TCP_WND);
    state_set(c, SOCKRING_STATE_OPEN, OK);
    c->queued_on = l;
    l->q[(l->head + l->n++) % STACK_TCP_BACKLOG] = c;
    l->arrived++;
    return c;
}

status_t ntcp_listen(const struct ntcp_listen_req *req, struct ntcp_listener **out)
{
    if (!req->rings || !req->drop || !sockring_size_ok(req->rx_size))
        return ERR_INVALID_ARGS;
    struct ntcp_listener *l = NULL;
    for (unsigned i = 0; i < TCP_LISTENERS && !l; i++)
        if (!listeners[i].used)
            l = &listeners[i];
    if (!l)
        return ERR_NO_RESOURCES;
    *l = (struct ntcp_listener){ .used = true, .req = *req };
    status_t st = stack_tcp_listen(req->port, req->backlog, req->rx_size, l, &l->l, &l->port);
    if (st != OK) {
        *l = (struct ntcp_listener){ 0 };
        return st;
    }
    *out = l;
    return OK;
}

status_t ntcp_accept(struct ntcp_listener *l, struct ntcp_conn **out)
{
    if (!l->n)
        return ERR_SHOULD_WAIT;
    struct ntcp_conn *c = l->q[l->head];
    l->head = (l->head + 1) % STACK_TCP_BACKLOG;
    l->n--;
    c->queued_on = NULL;
    if (c->t)
        stack_tcp_taken(c->t);
    note(c);   /* bytes may have come before the program had it */
    *out = c;
    return OK;
}

void ntcp_unlisten(struct ntcp_listener *l)
{
    while (l->n) {
        struct ntcp_conn *c = l->q[l->head];
        l->head = (l->head + 1) % STACK_TCP_BACKLOG;
        l->n--;
        if (c->t)
            stack_tcp_abort(c->t);
        l->req.drop(l, c);
        slot_clear(c);
    }
    stack_tcp_unlisten(l->l);
    *l = (struct ntcp_listener){ 0 };
}

/* ---- connections ------------------------------------------------------------------ */

static void slot_clear(struct ntcp_conn *c)
{
    unlist(c);
    closing_set(c, false);
    retry_set(c, false);
    *c = (struct ntcp_conn){ 0 };
}

status_t ntcp_conn_new(void *owner, struct ntcp_conn **out)
{
    for (unsigned i = 0; i < TCP_CONNS; i++) {
        struct ntcp_conn *c = &conns[i];
        if (c->used)
            continue;
        *c = (struct ntcp_conn){ .used = true, .owner = owner, .window = STACK_TCP_WND };
        c->st.state = SOCKRING_STATE_OPEN;   /* as sockring_make leaves the line */
        *out = c;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

void ntcp_conn_rings(struct ntcp_conn *c, const struct sockring *r)
{
    c->r = *r;
    c->window = min32(r->rx.size, STACK_TCP_WND);
    sockring_status_put(&c->r, &c->st);
}

status_t ntcp_connect(struct ntcp_conn *c, uint32_t to, uint16_t port)
{
    if (!c->r.page || c->t || c->st.state == SOCKRING_STATE_CLOSED)
        return ERR_BAD_STATE;
    status_t st = stack_tcp_connect(to, port, c->window, c, &c->t);
    if (st == OK)
        state_set(c, SOCKRING_STATE_CONNECTING, OK);
    return st;
}

void ntcp_kick(struct ntcp_conn *c)
{
    note(c);
}

void ntcp_conn_abort(struct ntcp_conn *c, status_t why)
{
    if (c->t)
        stack_tcp_abort(c->t);
    c->t = NULL;
    closing_set(c, false);
    retry_set(c, false);
    state_set(c, SOCKRING_STATE_CLOSED, why);
}

void ntcp_conn_free(struct ntcp_conn *c)
{
    if (c->queued_on)
        queue_drop(c->queued_on, c);
    if (c->t) {
        /* Bytes it never read, or a ring it broke: the peer is told with a
         * reset, as a closed socket with unread data does elsewhere. */
        bool unread = c->r.page && sockring_room(&c->r.rx) < c->r.rx.size;
        if (unread || ring_bad(c))
            stack_tcp_abort(c->t);
        else
            stack_tcp_release(c->t);
    }
    slot_clear(c);
}

/* ---- the loop's side ---------------------------------------------------------------- */

static const struct stack_tcp_hooks hooks = {
    .rx = on_rx,
    .rx_end = on_rx_end,
    .connected = on_connected,
    .sent = on_sent,
    .gone = on_gone,
    .accepted = on_accepted,
};

void ntcp_init(void)
{
    stack_tcp_set_hooks(&hooks);
}

void ntcp_work(void)
{
    if (freed && nretry) {
        for (unsigned i = 0; i < TCP_CONNS; i++) {
            if (conns[i].retry) {
                retry_set(&conns[i], false);
                note(&conns[i]);
            }
        }
    }
    freed = false;
    /* The list as it is now: work noted while it runs waits for the next
     * turn, so one busy connection can't hold the loop. */
    struct ntcp_conn *c = work_head;
    work_head = work_tail = NULL;
    while (c) {
        struct ntcp_conn *next = c->next;
        c->listed = false;
        c->next = NULL;
        conn_work(c);
        c = next;
    }
    for (unsigned i = 0; nclosing && i < TCP_CONNS; i++)
        if (conns[i].closing)
            closed_check(&conns[i]);
}

bool ntcp_pending(void)
{
    return work_head != NULL;
}

void ntcp_get_counts(struct ntcp_counts *out)
{
    *out = counts;
}

void ntcp_census(uint32_t *nconns, uint32_t *nlisteners)
{
    *nconns = *nlisteners = 0;
    for (unsigned i = 0; i < TCP_CONNS; i++)
        *nconns += conns[i].used;
    for (unsigned i = 0; i < TCP_LISTENERS; i++)
        *nlisteners += listeners[i].used;
}
