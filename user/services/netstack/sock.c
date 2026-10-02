/* netstack: programs' UDP sockets (progs.h has the model). A socket is a
 * slot here, an lwIP socket (stack.h's stack_udp), a channel of its own
 * and its rings (<sockring.h>: one VMO netstack makes and maps, two
 * events); closing the channel, or its opener going, closes them all.
 *
 * The rings are the program's to write, so everything in them is read as
 * <sockring.h> says: our own counts and copies, the program's counts
 * clamped, a record's header read once and its bytes copied out before
 * anything looks at them. What a program may send is checked on that
 * copy, before lwIP sees it: unicast destinations only, a port, at most
 * NET_DGRAM_MAX bytes. The DHCP socket (netctl's dhcp_open) is the one
 * exception: port 68, sends to port 67 of 255.255.255.255 or a unicast
 * address, out of the interface even with no address. A refused record
 * is counted in the socket's status line (tx_refused, and why in `error`).
 *
 * Receiving never waits for the program: a datagram goes into the rx ring
 * (published once a turn, progs_flush), or is dropped and counted when it
 * doesn't fit. Sending never waits for the card: a tx ring is read only
 * while the card's tx ring has room for a datagram, at most SOCK_TX_BUDGET
 * records a socket a turn, the sockets in turn; with no room the rest
 * stays in the rings until the driver says it has some (dev_tx_room_came).
 * TCP's segments are lwIP's to send: one the card refused stays in lwIP,
 * and goes when the driver says it has room (sock_tcp_card).
 *
 * A ring the program broke (a count out of range, a bad record) is looked
 * at again only when the program signals: garbage costs netstack one look
 * a signal, not one a turn.
 *
 * Closing: only once the program's end of the socket's channel is closed
 * (a wait set may read the rings until it sees SIG_PEER_CLOSED) is the VMO
 * unmapped and shrunk to nothing, before its handles close, so a program
 * that keeps its handle holds no page charged to netstack (the program
 * unmaps first: libos's net_close). A socket whose opener goes is ended
 * but not closed: lwIP's socket and its port go at once, its status says
 * CLOSED (ERR_PEER_CLOSED), and its rings and channel stay, still counted
 * against the shares, until the program closes it. */
#include <idl/net.h>
#include "ctl.h"
#include "progs.h"
#include "sockmem.h"

#define BROADCAST   0xffffffffu

static uint8_t txbuf[NET_DGRAM_MAX];   /* the record being sent: our copy */

static uint64_t key_of(unsigned i, uint32_t base)
{
    return (base + i) | (uint64_t)pg.s[i].gen << 8;
}

bool sock_sendable(uint32_t a)
{
    if (!ctl_unicast(a))
        return false;
    struct stack_state st;
    stack_get(&st);
    uint32_t m = st.ip.mask;
    return !m || (a & m) != (st.ip.address & m) || (a & ~m) != ~m;
}

status_t sock_open(unsigned slot, uint16_t port, bool dhcp, handle_t *out, uint16_t *out_port,
                   unsigned *out_i)
{
    unsigned i = dhcp ? NET_SOCKETS_MAX : 0;   /* programs' are the first NET_SOCKETS_MAX */
    uint8_t cls = dhcp ? CLASS_SYS : pg.o[slot].cls;
    if (dhcp && pg.s[i].ch)
        return ERR_ALREADY_BOUND;
    while (!dhcp && i < NET_SOCKETS_MAX && pg.s[i].ch)
        i++;
    if (!dhcp && (i == NET_SOCKETS_MAX || !progs_share_ok(cls, 1, 0, 0)))
        return ERR_NO_RESOURCES;
    struct sock *s = &pg.s[i];
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    s->gen++;
    st = stack_udp_open(port, dhcp, s, &s->u, &s->port);
    if (st == OK)
        st = jam_port_bind(pg.port, mine, key_of(i, KEY_SOCK), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        stack_udp_close(s->u);
        s->u = NULL;
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    *s = (struct sock){ .ch = mine, .gen = s->gen, .dhcp = dhcp, .cls = cls, .opener = slot,
                        .opener_gen = dhcp ? 0 : pg.o[slot].gen, .u = s->u, .port = s->port,
                        .pending = true };   /* a request may come before the first packet */
    pg.held[cls].socks++;
    if (!dhcp)
        pg.o[slot].socks++;
    *out = theirs;
    *out_port = s->port;
    *out_i = i;
    return OK;
}

/* ---- the rings ---------------------------------------------------------------------- */

/* The opener whose share socket i's rings count against (NULL: the DHCP
 * socket's, or its opener gone). */
static struct opener *owner(const struct sock *s)
{
    return s->dhcp ? NULL : progs_opener(s->opener, s->opener_gen);
}

status_t sock_rings_make(unsigned i, uint32_t *tx, uint32_t *rx, handle_t hs[3])
{
    struct sock *s = &pg.s[i];
    uint32_t t = *tx ? *tx : SOCKRING_UDP_TX, r = *rx ? *rx : SOCKRING_UDP_RX;
    if (s->m.vmo)
        return ERR_BAD_STATE;
    if (!sockring_size_ok(t) || !sockring_size_ok(r))
        return ERR_INVALID_ARGS;
    uint64_t bytes = sockring_bytes(t, r);
    if (!sockmem_budget_ok(owner(s), s->cls, bytes))
        return ERR_NO_RESOURCES;
    hs[0] = hs[1] = hs[2] = HANDLE_INVALID;
    status_t st = sockmem_make(&s->m, &s->r, SOCKRING_DGRAM, t, r);
    if (st == OK)
        st = jam_port_bind(pg.port, s->m.to_stack, key_of(i, KEY_RING),
                           SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = sockmem_handles(&s->m, hs);
    if (st != OK) {
        sockmem_drop(&s->m, pg.port, key_of(i, KEY_RING));
        return st;
    }
    s->st = (struct sockring_status){ .state = SOCKRING_STATE_OPEN };
    s->changes_put = 0;
    s->tx_ready = true;   /* the first look finds it empty and raises our flag */
    struct opener *o = owner(s);
    if (o)
        o->ring_bytes += bytes;
    pg.held[s->cls].ring_bytes += bytes;
    *tx = t;
    *rx = r;
    return OK;
}

void sock_close(unsigned i)
{
    struct sock *s = &pg.s[i];
    struct opener *o = owner(s);
    stack_udp_close(s->u);   /* no more datagrams for it */
    if (s->m.vmo) {
        if (o)
            o->ring_bytes -= s->m.bytes;
        pg.held[s->cls].ring_bytes -= s->m.bytes;
        sockmem_drop(&s->m, pg.port, key_of(i, KEY_RING));
    }
    (void)jam_port_unbind(pg.port, s->ch, key_of(i, KEY_SOCK));   /* bound since sock_open */
    jam_handle_close(s->ch);
    if (o && o->socks)
        o->socks--;
    pg.held[s->cls].socks--;
    s->ch = HANDLE_INVALID;
    s->u = NULL;
    s->pending = s->tx_ready = s->rx_dirty = s->st_dirty = false;
}

/* Socket s's opener went: end it (above, "Closing"). */
static void sock_end(struct sock *s)
{
    stack_udp_close(s->u);   /* its port is free again; no more datagrams */
    s->u = NULL;
    s->tx_ready = false;
    s->st.state = SOCKRING_STATE_CLOSED;
    s->st.error = ERR_PEER_CLOSED;
    s->st.changes++;
    s->st_dirty = s->m.vmo != 0;
}

void sock_close_opener(unsigned slot)
{
    for (unsigned i = 0; i < NET_SOCKETS_MAX; i++) {
        struct sock *s = &pg.s[i];
        if (!s->ch || !s->u || s->opener != slot || s->opener_gen != pg.o[slot].gen)
            continue;
        if (!s->m.vmo) {
            sock_close(i);   /* no rings: nothing of the program's to keep */
            continue;
        }
        if (pg.o[slot].ring_bytes >= s->m.bytes)
            pg.o[slot].ring_bytes -= s->m.bytes;   /* the opener's count goes with it */
        if (pg.o[slot].socks)
            pg.o[slot].socks--;
        s->opener_gen = 0;   /* owner() finds none from now on */
        sock_end(s);
    }
}

void sock_census(uint32_t *queued, uint32_t *open)
{
    *queued = *open = 0;
    for (unsigned i = 0; i < SOCK_SLOTS; i++) {
        struct sock *s = &pg.s[i];
        if (!s->ch)
            continue;
        (*open)++;
        if (s->m.vmo)
            *queued += s->r.rx.size - sockring_room(&s->r.rx);
    }
}

/* stack.h's stack_udp_input: a datagram for socket ctx, into its rx ring. */
static void dgram_in(void *ctx, uint32_t from, uint16_t port, const uint8_t *data, size_t len)
{
    struct sock *s = ctx;
    if (!s->ch || len > NET_DGRAM_MAX)
        return;
    if (s->peer && (from != s->peer || port != s->peer_port))
        return;   /* not from its peer: not for it */
    struct sockring_dgram h = { .addr = from, .port = port, .len = (uint16_t)len };
    if (s->m.vmo && sockring_dgram_put(&s->r.rx, &h, data) == OK) {
        pg.c.dgrams_in++;
        s->rx_dirty = true;
        return;
    }
    s->st.rx_dropped++;   /* full (or no rings yet) */
    s->st_dirty = s->m.vmo != 0;
    pg.c.dgrams_dropped++;
}

/* ---- sending ------------------------------------------------------------------------ */

/* Why a record from s may not go (OK: it may), on our copy *h. */
static status_t refusal(const struct sock *s, struct sockring_dgram *h)
{
    if (!h->addr && !h->port) {
        if (!s->peer)
            return ERR_BAD_STATE;
        h->addr = s->peer;
        h->port = s->peer_port;
    }
    if (!h->port)
        return ERR_INVALID_ARGS;
    if (s->dhcp ? h->port != NET_PORT_DHCP_SERVER ||
                      (h->addr != BROADCAST && !sock_sendable(h->addr))
                : !sock_sendable(h->addr))
        return ERR_INVALID_ARGS;
    return OK;
}

static void refused(struct sock *s, status_t why)
{
    s->st.tx_refused++;
    if (s->st.error != why)
        s->st.changes++;
    s->st.error = why;
    s->st_dirty = true;
    pg.c.dgrams_refused++;
}

/* Up to SOCK_TX_BUDGET records off socket s's tx ring while the card has
 * room. true: the card ran out of room first. */
static bool tx_one(struct sock *s)
{
    struct sockring_end *e = &s->r.tx;
    (void)jam_event_signal(s->m.to_stack, SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM, 0);   /* ours */
    sockring_awake(e);
    s->tx_ready = false;
    uint64_t errors = e->errors, before = e->count;
    unsigned k = 0;
    for (; k < SOCK_TX_BUDGET; k++) {
        if (!dev_tx_room(pg.dev, SOCK_CARD_ROOM))
            break;
        struct sockring_dgram h;
        status_t st = sockring_dgram_take(e, &h, txbuf);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st == ERR_OUT_OF_RANGE) {   /* a broken record: counted in ring_errors below */
            pg.c.dgrams_refused++;
            continue;
        }
        st = refusal(s, &h);
        if (st == OK)
            st = stack_udp_send(s->u, h.addr, h.port, txbuf, h.len, s->dhcp);
        if (st == OK)
            pg.c.dgrams_out++;
        else
            refused(s, st);
    }
    bool broken = e->errors != errors;
    if (broken) {
        s->st.ring_errors = e->errors + s->r.rx.errors;
        s->st_dirty = true;
    }
    if (s->st_dirty)   /* before the count: a program that sees it taken sees why it was refused */
        sockring_status_put(&s->r, &s->st);
    if (e->count != before && sockring_publish(e))
        (void)jam_event_signal(s->m.to_prog, 0, SOCKRING_SIG_TX_ROOM);   /* gone: the channel says */
    bool full = !broken && k < SOCK_TX_BUDGET && !dev_tx_room(pg.dev, SOCK_CARD_ROOM);
    if (broken)   /* our flag up, whatever the ring holds: the next look waits for a signal */
        __atomic_store_n(&e->cons->waits, 1, __ATOMIC_RELAXED);
    else if (full || k == SOCK_TX_BUDGET)
        s->tx_ready = true;          /* more may wait: the next turn, or when the card has room */
    else if (!sockring_sleep(e, 1))
        s->tx_ready = true;          /* came while we looked */
    return full;
}

void sock_tx_all(void)
{
    if (pg.card_full)
        return;
    for (unsigned n = 0; n < SOCK_SLOTS; n++) {
        unsigned i = (pg.next_tx + n) % SOCK_SLOTS;
        struct sock *s = &pg.s[i];
        if (!s->ch || !s->m.vmo || !s->u || !s->tx_ready || !tx_one(s))
            continue;
        /* The card is full: ask to be told, and start here next time. */
        pg.next_tx = i;
        pg.card_full = dev_tx_wait(pg.dev, SOCK_CARD_ROOM);
        if (pg.card_full)
            return;
    }
    pg.next_tx = (pg.next_tx + 1) % SOCK_SLOTS;
}

bool sock_tx_pending(void)
{
    if (pg.card_full)
        return false;
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (pg.s[i].ch && pg.s[i].m.vmo && pg.s[i].u && pg.s[i].tx_ready)
            return true;
    return false;
}

void sock_ring_event(unsigned i, uint32_t gen)
{
    struct sock *s = i < SOCK_SLOTS ? &pg.s[i] : NULL;
    if (s && s->ch && s->m.vmo && s->u && s->gen == gen)
        s->tx_ready = true;   /* SIG_RX_ROOM needs nothing: a full rx ring drops */
}

/* dev.h's dev_tx_room_came: the card's ring has room (or a new session). */
static void room_came(void)
{
    pg.card_full = false;
    if (pg.tcp_card_wait) {
        pg.tcp_card_wait = false;
        stack_tx_resume();
    }
}

bool sock_tcp_card(void)
{
    if (pg.tcp_card_wait || !stack_tx_blocked())
        return false;
    if (dev_tx_wait(pg.dev, 1)) {
        pg.tcp_card_wait = true;
        return false;
    }
    stack_tx_resume();   /* room came while we looked */
    return true;
}

void sock_flush(void)
{
    for (unsigned i = 0; i < SOCK_SLOTS; i++) {
        struct sock *s = &pg.s[i];
        if (!s->ch || !s->m.vmo || !(s->rx_dirty || s->st_dirty))
            continue;
        signals_t bits = 0;
        if (s->st_dirty) {
            s->st.ring_errors = s->r.tx.errors + s->r.rx.errors;
            sockring_status_put(&s->r, &s->st);
            bits |= s->st.changes != s->changes_put ? SOCKRING_SIG_STATE : 0;
            s->changes_put = s->st.changes;
        }
        if (s->rx_dirty && sockring_publish(&s->r.rx))
            bits |= SOCKRING_SIG_RX;
        s->rx_dirty = s->st_dirty = false;
        if (bits)
            (void)jam_event_signal(s->m.to_prog, 0, bits);   /* gone: its channel says */
    }
}

/* ---- the socket's methods ------------------------------------------------------- */

static status_t op_connect(void *ctx, uint32_t address, uint16_t port)
{
    struct sock *s = ctx;
    if (s->dhcp)
        return ERR_NOT_SUPPORTED;
    if ((address || port) && (!port || !sock_sendable(address)))
        return ERR_INVALID_ARGS;
    s->peer = address;
    s->peer_port = address ? port : 0;
    return OK;
}

static status_t op_state(void *ctx, uint16_t *out_port, uint32_t *out_peer,
                         uint16_t *out_peer_port, uint32_t *out_queued, uint32_t *out_dropped)
{
    struct sock *s = ctx;
    *out_port = s->port;
    *out_peer = s->peer;
    *out_peer_port = s->peer_port;
    *out_queued = s->m.vmo ? s->r.rx.size - sockring_room(&s->r.rx) : 0;
    *out_dropped = s->st.rx_dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)s->st.rx_dropped;
    return OK;
}

static status_t op_rings(void *ctx, uint32_t tx_bytes, uint32_t rx_bytes, handle_t *out_ring,
                         handle_t *out_to_stack, handle_t *out_to_prog, uint32_t *out_tx_bytes,
                         uint32_t *out_rx_bytes)
{
    struct sock *s = ctx;
    handle_t hs[3];
    status_t st = sock_rings_make((unsigned)(s - pg.s), &tx_bytes, &rx_bytes, hs);
    if (st != OK)
        return st;
    *out_ring = hs[0];
    *out_to_stack = hs[1];
    *out_to_prog = hs[2];
    *out_tx_bytes = tx_bytes;
    *out_rx_bytes = rx_bytes;
    return OK;
}

static const struct net_ops sock_ops = {
    .sock_connect = op_connect,
    .sock_state = op_state,
    .sock_rings = op_rings,
};

void sock_serve(unsigned i)
{
    struct sock *s = &pg.s[i];
    s->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = net_serve_one(s->ch, &sock_ops, s);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            sock_close(i);
        else if (st != ERR_SHOULD_WAIT)
            printf("netstack: reading socket %u's channel: %s\n", s->port, status_str(st));
        return;
    }
    s->pending = true;   /* its budget is spent: more may be queued */
}

/* ctl.h's ctl_dhcp_open: netctl's DHCP socket. */
static status_t dhcp_open(handle_t *out)
{
    uint16_t port;
    unsigned i;
    return sock_open(0, NET_PORT_DHCP_CLIENT, true, out, &port, &i);
}

void sock_hooks(void)
{
    stack_udp_input = dgram_in;
    ctl_dhcp_open = dhcp_open;
    dev_tx_room_came = room_came;
}
