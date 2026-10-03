/* netstack: programs' TCP sockets and listeners (tcpsock.h has the
 * model; tcp.c moves their bytes). */
#include <idl/net.h>
#include "listen.h"
#include "progs.h"
#include "sockmem.h"
#include "stack.h"
#include "tcp.h"
#include "tcpsock.h"

#define TK_SOCK   1u   /* a socket's channel */
#define TK_RING   2u   /* a socket's to_stack event */
#define TK_LISTEN 3u   /* a listener's channel */

_Static_assert(NET_TCP_MAX == STACK_TCP_CONNS, "a socket for each of tcp.c's connections");
_Static_assert(NET_LISTENERS_MAX == STACK_TCP_LISTENERS, "one listener table");
_Static_assert(NET_BACKLOG_MAX == STACK_TCP_BACKLOG, "one backlog limit");
_Static_assert(NET_BACKLOG_TOTAL <= 128, "half-open pcbs fit lwIP's 128 beyond the connections");

struct tlisten;

/* A program's TCP socket. */
struct tsock {
    handle_t          ch;          /* our end of its channel; 0: the slot is free */
    handle_t          theirs;      /* the program's end, until it is handed out */
    uint32_t          gen;         /* the slot's generation */
    bool              pending;     /* requests may be queued */
    bool              charged;     /* counted against its opener and class */
    uint8_t           cls;         /* its opener's class */
    unsigned          opener;      /* the opener charged, and its generation then */
    uint32_t          opener_gen;  /* (0: the opener went: only the class is charged) */
    struct tlisten   *on;          /* the listener it waits on until accepted (else NULL) */
    struct ntcp_conn *c;           /* its connection (tcp.c) */
    struct sockmem    m;           /* its rings' memory */
    uint32_t          peer;        /* the peer's address and port, ... */
    uint16_t          peer_port;
    uint16_t          port;        /* ... and ours */
};

/* A program's listener. */
struct tlisten {
    handle_t              ch;          /* our end of its channel; 0: the slot is free */
    uint32_t              gen;         /* the slot's generation */
    bool                  pending;     /* requests may be queued */
    uint8_t               cls;         /* its opener's class */
    unsigned              opener;      /* its opener and that opener's generation */
    uint32_t              opener_gen;
    struct ntcp_listener *l;           /* tcp.c's listener */
    uint32_t              backlog;     /* counted against the shares */
    uint32_t              tx, rx;      /* its connections' ring sizes */
    bool                  waiting;     /* an accept waits for a connection: */
    struct idl_txn        txn;         /* ... its request ... */
    uint64_t              deadline;    /* ... and when it times out (ns) */
};

/* An accepted connection, as accept answers it. */
struct taken {
    handle_t socket, ring, to_stack, to_prog;
    uint32_t peer;
    uint16_t peer_port;
    uint32_t tx, rx;
};

static struct tsock   ts[NET_TCP_MAX];
static struct tlisten tl[NET_LISTENERS_MAX];
static uint32_t       refused_gone;   /* connections refused by listeners closed since */

static uint64_t key(unsigned kind, unsigned slot, uint32_t gen)
{
    return KEY_TCP | (uint64_t)kind << 48 | (uint64_t)gen << 16 | slot;
}

static struct opener *opener_of(unsigned slot, uint32_t gen)
{
    return gen ? progs_opener(slot, gen) : NULL;
}

/* ---- the shares ------------------------------------------------------------------- */

/* May opener o (class cls; NULL: none) have one more connection with rings
 * of `bytes`? */
static bool conn_ok(const struct opener *o, uint8_t cls, uint64_t bytes)
{
    if ((o && o->tcp >= NET_TCP_PER_OPENER) ||
        pg.held[CLASS_PROG].tcp + pg.held[CLASS_SYS].tcp >= NET_TCP_MAX)
        return false;
    if (cls == CLASS_PROG && pg.held[CLASS_PROG].tcp >= NET_PROG_TCP) {
        pg.c.refused_shares++;
        return false;
    }
    return sockmem_budget_ok(o, cls, bytes);
}

/* May opener o have one more listener with this backlog? */
static bool listener_ok(const struct opener *o, uint32_t backlog)
{
    const struct share *p = &pg.held[CLASS_PROG], *s = &pg.held[CLASS_SYS];
    if (o->listeners >= NET_LISTENERS_PER_OPENER ||
        p->listeners + s->listeners >= NET_LISTENERS_MAX ||
        p->backlog + s->backlog + backlog > NET_BACKLOG_TOTAL)
        return false;
    if (o->cls == CLASS_PROG &&
        (p->listeners >= NET_PROG_LISTENERS || p->backlog + backlog > NET_PROG_BACKLOG)) {
        pg.c.refused_shares++;
        return false;
    }
    return true;
}

static void charge(struct tsock *t, struct opener *o)
{
    if (o) {
        o->tcp++;
        o->ring_bytes += t->m.bytes;
    }
    pg.held[t->cls].tcp++;
    pg.held[t->cls].ring_bytes += t->m.bytes;
    t->charged = true;
}

static void uncharge(struct tsock *t)
{
    if (!t->charged)
        return;
    struct opener *o = opener_of(t->opener, t->opener_gen);
    if (o) {
        o->tcp--;
        o->ring_bytes -= t->m.bytes;
    }
    pg.held[t->cls].tcp--;
    pg.held[t->cls].ring_bytes -= t->m.bytes;
    t->charged = false;
}

/* ---- sockets ------------------------------------------------------------------------- */

/* Free socket t: its connection too unless tcp.c frees it itself
 * (conn_too false), its rings, its channel. */
static void ts_close(struct tsock *t, bool conn_too)
{
    unsigned i = (unsigned)(t - ts);
    if (conn_too && t->c)
        ntcp_conn_free(t->c);   /* while its rings are still mapped: it looks at them */
    uncharge(t);
    sockmem_drop(&t->m, pg.port, key(TK_RING, i, t->gen));
    if (t->ch) {
        (void)jam_port_unbind(pg.port, t->ch, key(TK_SOCK, i, t->gen));   /* may not be bound */
        jam_handle_close(t->ch);
    }
    if (t->theirs)
        jam_handle_close(t->theirs);
    *t = (struct tsock){ .gen = t->gen };
}

/* Its channel, rings, bindings and connection (c: tcp.c's, accepted;
 * NULL: a new one), charged to opener o (class cls). */
static status_t ts_new(struct opener *o, uint8_t cls, uint32_t tx, uint32_t rx,
                       struct ntcp_conn *c, struct tsock **out)
{
    unsigned i = 0;
    while (i < NET_TCP_MAX && ts[i].ch)
        i++;
    if (i == NET_TCP_MAX || !conn_ok(o, cls, sockring_bytes(tx, rx)))
        return ERR_NO_RESOURCES;
    struct tsock *t = &ts[i];
    *t = (struct tsock){ .gen = t->gen + 1, .cls = cls, .opener = o ? (unsigned)(o - pg.o) : 0,
                         .opener_gen = o ? o->gen : 0, .c = c };
    struct sockring r;
    status_t st = jam_channel_create(&t->ch, &t->theirs);
    if (st == OK)
        st = sockmem_make(&t->m, &r, SOCKRING_STREAM, tx, rx);
    if (st == OK && !c)
        st = ntcp_conn_new(t, &t->c);
    if (st == OK)
        st = jam_port_bind(pg.port, t->ch, key(TK_SOCK, i, t->gen), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(pg.port, t->m.to_stack, key(TK_RING, i, t->gen),
                           SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM, PORT_BIND_PERSISTENT);
    if (st != OK) {
        ts_close(t, !c);   /* an accepted connection is tcp.c's to free */
        return st;
    }
    t->c->owner = t;
    t->c->to_prog = t->m.to_prog;
    ntcp_conn_rings(t->c, &r);
    charge(t, o);
    *out = t;
    return OK;
}

/* Hand socket t to the program: its channel's other end and its rings'
 * handles. Failing, nothing is handed and t is untouched. */
static status_t hand_out(struct tsock *t, struct taken *k)
{
    handle_t hs[3];
    status_t st = sockmem_handles(&t->m, hs);
    if (st != OK)
        return st;
    *k = (struct taken){ .socket = t->theirs, .ring = hs[0], .to_stack = hs[1], .to_prog = hs[2],
                         .peer = t->peer, .peer_port = t->peer_port, .tx = t->c->r.tx.size,
                         .rx = t->c->r.rx.size };
    t->theirs = HANDLE_INVALID;
    return OK;
}

status_t tcpsock_op_tcp(void *ctx, uint32_t address, uint16_t port, uint32_t tx_bytes,
                        uint32_t rx_bytes, handle_t *out_socket, handle_t *out_ring,
                        handle_t *out_to_stack, handle_t *out_to_prog, uint16_t *out_port,
                        uint32_t *out_tx_bytes, uint32_t *out_rx_bytes)
{
    struct opener *o = ctx;
    uint32_t tx = tx_bytes ? tx_bytes : NET_TCP_TX, rx = rx_bytes ? rx_bytes : NET_TCP_RX;
    if (!port || !sock_sendable(address) || !sockring_size_ok(tx) || !sockring_size_ok(rx))
        return ERR_INVALID_ARGS;
    struct tsock *t;
    struct taken k;
    status_t st = ts_new(o, o->cls, tx, rx, NULL, &t);
    if (st != OK)
        return st;
    st = ntcp_connect(t->c, address, port);
    if (st == OK) {
        stack_tcp_ends(t->c->t, &t->peer, &t->peer_port, &t->port);
        st = hand_out(t, &k);
    }
    if (st != OK) {
        ts_close(t, true);
        return st;
    }
    *out_socket = k.socket;
    *out_ring = k.ring;
    *out_to_stack = k.to_stack;
    *out_to_prog = k.to_prog;
    *out_port = t->port;
    *out_tx_bytes = k.tx;
    *out_rx_bytes = k.rx;
    return OK;
}

static status_t ts_state(void *ctx, uint16_t *out_port, uint32_t *out_peer,
                         uint16_t *out_peer_port, uint32_t *out_queued, uint32_t *out_dropped)
{
    struct tsock *t = ctx;
    *out_port = t->port;
    *out_peer = t->peer;
    *out_peer_port = t->peer_port;
    *out_queued = t->c ? t->c->r.rx.size - sockring_room(&t->c->r.rx) : 0;
    *out_dropped = 0;   /* a stream drops nothing: its window stops the sender */
    return OK;
}

static const struct net_ops sock_ops = { .sock_state = ts_state };

static void ts_serve(struct tsock *t)
{
    t->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = net_serve_one(t->ch, &sock_ops, t);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            ts_close(t, true);
        else if (st != ERR_SHOULD_WAIT)
            printf("netstack: reading TCP socket %u's channel: %s\n", t->port, status_str(st));
        return;
    }
    t->pending = true;   /* its budget is spent: more may be queued */
}

/* ---- listeners ------------------------------------------------------------------------ */

/* tcp.c's ntcp_rings_fn: a listener's connection finished its handshake. */
static status_t l_rings(struct ntcp_listener *nl, struct ntcp_conn *c)
{
    struct tlisten *l = nl->req.owner;
    struct opener *o = opener_of(l->opener, l->opener_gen);
    struct tsock *t;
    if (!o)
        return ERR_BAD_STATE;   /* its opener is going: the listener goes next */
    /* A big tx ring (a bulk sender's, as serve's) that doesn't fit the
     * opener's bytes or the shares: the default one, so the client is
     * served (slower) rather than reset. Only tx: the window the SYN-ACK
     * announced is the rx ring's. */
    uint32_t tx = l->tx;
    if (tx > NET_TCP_TX && !sockmem_budget_fits(o, l->cls, sockring_bytes(tx, l->rx)))
        tx = NET_TCP_TX;
    status_t st = ts_new(o, l->cls, tx, l->rx, c, &t);
    if (st != OK)
        return st;
    stack_tcp_ends(c->t, &t->peer, &t->peer_port, &t->port);
    t->on = l;
    return OK;
}

/* tcp.c's ntcp_drop_fn: a waiting connection went before it was taken. */
static void l_drop(struct ntcp_listener *nl, struct ntcp_conn *c)
{
    (void)nl;
    struct tsock *t = c->owner;
    t->c = NULL;   /* tcp.c frees it */
    ts_close(t, false);
}

/* The oldest connection waiting on l, handed out. */
static status_t l_take(struct tlisten *l, struct taken *k)
{
    struct ntcp_conn *c;
    status_t st = ntcp_accept(l->l, &c);
    if (st != OK)
        return st;
    struct tsock *t = c->owner;
    t->on = NULL;
    st = hand_out(t, k);
    if (st != OK)
        ts_close(t, true);   /* nothing to give: the peer gets a reset */
    return st;
}

static status_t l_accept(void *ctx, struct idl_txn txn, uint32_t timeout_ms,
                         handle_t *out_socket, handle_t *out_ring, handle_t *out_to_stack,
                         handle_t *out_to_prog, uint32_t *out_peer, uint16_t *out_peer_port,
                         uint32_t *out_tx_bytes, uint32_t *out_rx_bytes)
{
    struct tlisten *l = ctx;
    struct taken k;
    if (l->waiting)
        return ERR_BAD_STATE;
    if (!l->l->n) {
        if (!timeout_ms)
            return ERR_SHOULD_WAIT;
        l->waiting = true;
        l->txn = txn;
        l->deadline = timeout_ms == NET_WAIT_FOREVER ? DEADLINE_NEVER
                                                     : now() + (uint64_t)timeout_ms * NS_PER_MS;
        return IDL_LATER;
    }
    status_t st = l_take(l, &k);
    if (st != OK)
        return st;
    *out_socket = k.socket;
    *out_ring = k.ring;
    *out_to_stack = k.to_stack;
    *out_to_prog = k.to_prog;
    *out_peer = k.peer;
    *out_peer_port = k.peer_port;
    *out_tx_bytes = k.tx;
    *out_rx_bytes = k.rx;
    return OK;
}

static const struct net_ops listener_ops = { .accept = l_accept };

/* Answer l's waiting accept: with a connection (st OK), or st. */
static void l_answer(struct tlisten *l, status_t st)
{
    struct taken k = { 0 };
    if (st == OK)
        st = l_take(l, &k);
    l->waiting = false;
    (void)net_reply_accept(l->txn, st, k.socket, k.ring, k.to_stack, k.to_prog, k.peer,
                           k.peer_port, k.tx, k.rx);   /* failing: its handles closed, the socket
                                                        * with them, and netstack sees that */
}

static void l_close(struct tlisten *l)
{
    unsigned i = (unsigned)(l - tl);
    if (l->waiting)
        l_answer(l, ERR_CANCELED);
    refused_gone += l->l->refused;
    ntcp_unlisten(l->l);   /* the connections waiting: reset, dropped (l_drop) */
    struct opener *o = opener_of(l->opener, l->opener_gen);
    if (o)
        o->listeners--;
    pg.held[l->cls].listeners--;
    pg.held[l->cls].backlog -= l->backlog;
    (void)jam_port_unbind(pg.port, l->ch, key(TK_LISTEN, i, l->gen));   /* bound when made */
    jam_handle_close(l->ch);
    *l = (struct tlisten){ .gen = l->gen };
}

/* A listener's slot, channel and binding, and tcp.c's listener. */
static status_t l_new(struct opener *o, const struct ntcp_listen_req *req, struct tlisten **out,
                      handle_t *theirs)
{
    unsigned i = 0;
    while (i < NET_LISTENERS_MAX && tl[i].ch)
        i++;
    if (i == NET_LISTENERS_MAX)
        return ERR_NO_RESOURCES;
    struct tlisten *l = &tl[i];
    *l = (struct tlisten){ .gen = l->gen + 1, .cls = o->cls, .opener = (unsigned)(o - pg.o),
                           .opener_gen = o->gen, .backlog = req->backlog };
    struct ntcp_listen_req r = *req;
    r.owner = l;
    status_t st = jam_channel_create(&l->ch, theirs);
    if (st != OK) {
        *l = (struct tlisten){ .gen = l->gen };
        return st;
    }
    st = ntcp_listen(&r, &l->l);
    if (st == OK)
        st = jam_port_bind(pg.port, l->ch, key(TK_LISTEN, i, l->gen),
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        if (l->l)
            ntcp_unlisten(l->l);
        jam_handle_close(l->ch);
        jam_handle_close(*theirs);
        *l = (struct tlisten){ .gen = l->gen };
        return st;
    }
    *out = l;
    return OK;
}

status_t tcpsock_op_listener(void *ctx, uint16_t port, uint32_t backlog, uint32_t tx_bytes,
                             uint32_t rx_bytes, handle_t *out_listener, uint16_t *out_port)
{
    struct opener *o = ctx;
    uint32_t tx = tx_bytes ? tx_bytes : NET_TCP_TX, rx = rx_bytes ? rx_bytes : NET_TCP_RX;
    if (!listen_may_accept(o, port))
        return ERR_ACCESS_DENIED;
    if (!backlog || backlog > NET_BACKLOG_MAX || !sockring_size_ok(tx) || !sockring_size_ok(rx))
        return ERR_INVALID_ARGS;
    if (!listener_ok(o, backlog))
        return ERR_NO_RESOURCES;
    struct ntcp_listen_req req = { .port = port, .backlog = backlog, .rx_size = rx,
                                   .rings = l_rings, .drop = l_drop };
    struct tlisten *l;
    status_t st = l_new(o, &req, &l, out_listener);
    if (st != OK)
        return st;
    l->tx = tx;
    l->rx = rx;
    o->listeners++;
    pg.held[l->cls].listeners++;
    pg.held[l->cls].backlog += backlog;
    *out_port = l->l->port;
    return OK;
}

static void l_serve(struct tlisten *l)
{
    l->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = net_serve_one(l->ch, &listener_ops, l);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            l_close(l);
        else if (st != ERR_SHOULD_WAIT)
            printf("netstack: reading TCP listener %u's channel: %s\n", l->l->port,
                   status_str(st));
        return;
    }
    l->pending = true;
}

/* ---- the loop's side ---------------------------------------------------------------------- */

void tcpsock_init(void)
{
    ntcp_init();
}

/* The program signalled to_stack. Its bits are cleared first: the binding
 * fires on a rise only, and tcp.c looks at the rings after this (the next
 * ntcp_work), so a signal made after the clear is a new rise and nothing
 * the program published is missed. */
static void ring_kick(struct tsock *t)
{
    (void)jam_event_signal(t->m.to_stack, SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM, 0);   /* ours */
    ntcp_kick(t->c);
}

bool tcpsock_packet(const struct port_packet *p)
{
    if (!(p->key & KEY_TCP))
        return false;
    unsigned kind = (unsigned)(p->key >> 48) & 0xff, i = (unsigned)(p->key & 0xffff);
    uint32_t gen = (uint32_t)(p->key >> 16);
    if (kind == TK_LISTEN && i < NET_LISTENERS_MAX && tl[i].ch && tl[i].gen == gen) {
        tl[i].pending = true;
    } else if (i < NET_TCP_MAX && ts[i].ch && ts[i].gen == gen) {
        if (kind == TK_SOCK)
            ts[i].pending = true;
        else if (kind == TK_RING && ts[i].c)
            ring_kick(&ts[i]);
    }
    return true;
}

void tcpsock_serve(void)
{
    for (unsigned i = 0; i < NET_LISTENERS_MAX; i++) {
        if (tl[i].ch && tl[i].pending)
            l_serve(&tl[i]);
        if (tl[i].ch && tl[i].waiting && tl[i].l->n)
            l_answer(&tl[i], OK);
    }
    for (unsigned i = 0; i < NET_TCP_MAX; i++)
        if (ts[i].ch && ts[i].pending)
            ts_serve(&ts[i]);
}

uint64_t tcpsock_tick(uint64_t t)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < NET_LISTENERS_MAX; i++) {
        struct tlisten *l = &tl[i];
        if (!l->ch || !l->waiting)
            continue;
        if (l->deadline <= t)
            l_answer(l, ERR_TIMED_OUT);
        else if (l->deadline < next)
            next = l->deadline;
    }
    return next;
}

bool tcpsock_pending(void)
{
    for (unsigned i = 0; i < NET_LISTENERS_MAX; i++)
        if (tl[i].ch && (tl[i].pending || (tl[i].waiting && tl[i].l->n)))
            return true;
    for (unsigned i = 0; i < NET_TCP_MAX; i++)
        if (ts[i].ch && ts[i].pending)
            return true;
    return false;
}

void tcpsock_close_opener(unsigned slot)
{
    uint32_t gen = pg.o[slot].gen;
    for (unsigned i = 0; i < NET_LISTENERS_MAX; i++)
        if (tl[i].ch && tl[i].opener == slot && tl[i].opener_gen == gen)
            l_close(&tl[i]);
    for (unsigned i = 0; i < NET_TCP_MAX; i++) {
        struct tsock *t = &ts[i];
        if (!t->ch || t->opener != slot || t->opener_gen != gen)
            continue;
        if (t->charged) {   /* the opener's count goes with it; the class's stays */
            pg.o[slot].tcp--;
            pg.o[slot].ring_bytes -= t->m.bytes;
        }
        t->opener_gen = 0;
        if (t->c)
            ntcp_conn_abort(t->c, ERR_PEER_CLOSED);
    }
}

void tcpsock_counts(struct net_counters *c)
{
    struct ntcp_counts n;
    struct stack_tcp_counts s;
    ntcp_get_counts(&n);
    stack_tcp_get_counts(&s);
    ntcp_census(&c->tcp_conns, &c->tcp_listeners);
    c->tcp_bytes_in = n.bytes_in;
    c->tcp_bytes_out = n.bytes_out;
    c->tcp_refused = refused_gone;
    for (unsigned i = 0; i < NET_LISTENERS_MAX; i++)
        if (tl[i].ch)
            c->tcp_refused += tl[i].l->refused;
    c->tcp_dropped = s.dropped + s.bad_acks + s.no_acks;
}
