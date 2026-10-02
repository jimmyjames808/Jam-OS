/* netstack: the openers of /svc/net and /svc/net-sys (progs.h has the
 * model). Each shared channel hands each opener a channel of its own
 * (svc.connect), of that channel's class; on it the opener asks about the
 * interface, waits for it to change, opens sockets (sock.c) and pings.
 * What waits is kept in a table of requests in flight (struct later), at
 * most NET_LATER_PER_OPENER an opener and LATER_MAX in all, answered when
 * the answer comes (a change, an echo reply, the driver's counts) or at
 * its deadline (progs_tick). Every opener, socket, request and ring byte
 * is counted against its class (pg.held): ordinary openers stop at their
 * share (progs_share_ok).
 *
 * An echo request carries the opener's echo id: its slot in the low 5
 * bits (unique among the openers there are) and random bits above, so a
 * reply goes to the opener that sent the request and to no other. */
#include <idl/svc.h>
#include "ctl.h"
#include "listen.h"
#include "progs.h"
#include "tcpsock.h"

/* The shared channels' keys (main.c's are below 0x10; listen.c's /svc/net-listen is 3) and
 * the kind of opener each makes. */
static const uint64_t shared_key[CLASSES] = { [CLASS_PROG] = 2, [CLASS_SYS] = 4 };
static const struct opener_kind kinds[CLASSES] = {
    [CLASS_PROG] = { .cls = CLASS_PROG }, [CLASS_SYS] = { .cls = CLASS_SYS },
};

_Static_assert(NET_OPENERS <= 32, "an opener's slot fits the echo id's low 5 bits");

struct progs pg;

static uint64_t key_of(unsigned i)
{
    return (KEY_OPENER + i) | (uint64_t)pg.o[i].gen << 8;
}

struct opener *progs_opener(unsigned slot, uint32_t gen)
{
    return slot < NET_OPENERS && pg.o[slot].ch && pg.o[slot].gen == gen ? &pg.o[slot] : NULL;
}

bool progs_share_ok(uint8_t cls, unsigned socks, unsigned later, uint64_t ring_bytes)
{
    const struct share *h = &pg.held[CLASS_PROG];
    if (cls != CLASS_PROG)
        return true;
    if (h->socks + socks <= NET_PROG_SOCKETS && h->later + later <= NET_PROG_LATER &&
        h->ring_bytes + ring_bytes <= NET_PROG_RING_BYTES)
        return true;
    pg.c.refused_shares++;
    return false;
}

/* ---- requests in flight ---------------------------------------------------------- */

static struct later *later_new(struct opener *o, uint8_t kind, struct idl_txn txn,
                               uint64_t deadline)
{
    if (o->later >= NET_LATER_PER_OPENER || !progs_share_ok(o->cls, 0, 1, 0))
        return NULL;
    for (unsigned i = 0; i < LATER_MAX; i++) {
        struct later *l = &pg.l[i];
        if (l->kind != LATER_FREE)
            continue;
        *l = (struct later){ .kind = kind, .opener = (unsigned)(o - pg.o), .txn = txn,
                             .deadline = deadline };
        o->later++;
        pg.held[o->cls].later++;
        return l;
    }
    return NULL;
}

static void later_done(struct later *l)
{
    struct opener *o = &pg.o[l->opener];
    if (o->later) {
        o->later--;
        pg.held[o->cls].later--;
    }
    l->kind = LATER_FREE;
}

/* Answer l with st (and, for OK, what its kind carries); it is done. */
static void later_answer(struct later *l, status_t st, uint32_t rtt_us, uint8_t ttl,
                         uint16_t size, const uint8_t *counts)
{
    static uint8_t zero[NETDEV_STATS_SIZE];
    if (l->kind == LATER_WAIT)
        (void)net_reply_wait_change(l->txn, st, pg.version);
    else if (l->kind == LATER_ECHO)
        (void)net_reply_echo(l->txn, st, rtt_us, ttl, size);
    else if (l->kind == LATER_CHIP)
        (void)net_reply_chip_counts(l->txn, st, counts ? counts : zero);
    later_done(l);   /* a reply that can't be written: its opener is going */
}

/* ctl.h's ctl_changed: the address or the DNS servers changed. */
static void changed(void)
{
    pg.version++;
    for (unsigned i = 0; i < LATER_MAX; i++)
        if (pg.l[i].kind == LATER_WAIT)
            later_answer(&pg.l[i], OK, 0, 0, 0, NULL);
}

/* stack.h's stack_echo_input: the reply (or the unreachable) to the echo
 * in flight with this id and seq to that peer, if there is one. */
static void echo_in(uint32_t peer, uint16_t id, uint16_t seq, uint8_t ttl, size_t len,
                    bool unreachable)
{
    for (unsigned i = 0; i < LATER_MAX; i++) {
        struct later *l = &pg.l[i];
        if (l->kind != LATER_ECHO || l->addr != peer || l->seq != seq ||
            pg.o[l->opener].echo_id != id)
            continue;
        uint64_t us = (now() - l->sent) / 1000;
        if (us > UINT32_MAX)
            us = UINT32_MAX;
        if (!unreachable)
            pg.c.echoes_answered++;
        later_answer(l, unreachable ? ERR_NOT_FOUND : OK, (uint32_t)us, ttl, (uint16_t)len, NULL);
        return;
    }
}

/* dev.h's dev_stats_done: the driver's counts, for every chip_counts waiting. */
static void chip_done(status_t st, const uint8_t *counts)
{
    for (unsigned i = 0; i < LATER_MAX; i++)
        if (pg.l[i].kind == LATER_CHIP)
            later_answer(&pg.l[i], st, 0, 0, 0, counts);
}

/* Forget opener i's requests in flight (its channel is going: no answers). */
static void later_drop(unsigned i)
{
    for (unsigned k = 0; k < LATER_MAX; k++)
        if (pg.l[k].kind != LATER_FREE && pg.l[k].opener == i)
            later_done(&pg.l[k]);
}

uint64_t progs_tick(void)
{
    uint64_t t = now(), next = DEADLINE_NEVER;
    for (unsigned i = 0; i < LATER_MAX; i++) {
        struct later *l = &pg.l[i];
        if (l->kind == LATER_FREE)
            continue;
        if (l->deadline <= t)
            later_answer(l, ERR_TIMED_OUT, 0, 0, 0, NULL);
        else if (l->deadline < next)
            next = l->deadline;
    }
    return next;
}

/* ---- the methods ------------------------------------------------------------------ */

static status_t op_iface(void *ctx, uint32_t *out_address, uint32_t *out_mask,
                         uint32_t *out_gateway, uint32_t *out_dns1, uint32_t *out_dns2,
                         uint8_t out_mac[6], uint8_t *out_device, uint8_t *out_link,
                         uint16_t *out_vlan, uint32_t *out_speed, uint32_t *out_version)
{
    (void)ctx;
    struct stack_state s;
    struct dev_report r = { 0 };
    uint32_t dns[2];
    stack_get(&s);
    ctl_dns(dns);
    if (pg.dev)
        dev_get_report(pg.dev, &r);
    *out_address = s.ip.address;
    *out_mask = s.ip.mask;
    *out_gateway = s.ip.gateway;
    *out_dns1 = dns[0];
    *out_dns2 = dns[1];
    memcpy(out_mac, s.mac, 6);
    *out_device = r.session;
    *out_link = s.link;
    *out_vlan = r.session ? r.vlan : 0;
    *out_speed = s.link ? r.speed : 0;
    *out_version = pg.version;
    return OK;
}

static status_t op_counts(void *ctx, uint8_t out_counts[256])
{
    (void)ctx;
    struct stack_counts s;
    struct dev_report r = { 0 };
    stack_get_counts(&s);
    if (pg.dev)
        dev_get_report(pg.dev, &r);
    struct net_counters c = pg.c;
    c.rx_frames = s.rx_frames;
    c.rx_refused = s.rx_refused;
    c.tx_frames = s.tx_frames;
    c.tx_dropped = s.tx_dropped;
    c.echo_replies = s.echo_replies;
    c.icmp_errors = s.icmp_errors;
    c.icmp_limited = s.icmp_limited;
    c.link_dropped = s.link_dropped;
    c.arp_dropped = s.arp_dropped;
    c.ip_dropped = s.ip_dropped;
    c.icmp_dropped = s.icmp_dropped;
    c.udp_dropped = s.udp_dropped;
    c.bad_checksums = s.bad_checksums;
    c.rx_buffers_used = s.rx_buffers_used;
    c.heap_used = s.heap_used;
    c.sessions = r.sessions;
    c.ring_errors = r.ring_errors;
    c.rx_bad = r.rx_bad;
    c.tx_full = r.tx_full;
    c.openers = 0;
    for (unsigned i = 0; i < NET_OPENERS; i++)
        c.openers += pg.o[i].ch != 0;
    c.later = 0;
    for (unsigned i = 0; i < LATER_MAX; i++)
        c.later += pg.l[i].kind != LATER_FREE;
    sock_census(&c.queued, &c.sockets);
    uint64_t rb = pg.held[CLASS_PROG].ring_bytes + pg.held[CLASS_SYS].ring_bytes;
    c.ring_bytes = rb > UINT32_MAX ? UINT32_MAX : (uint32_t)rb;
    tcpsock_counts(&c);
    memcpy(out_counts, &c, sizeof(c));
    return OK;
}

static status_t op_wait_change(void *ctx, struct idl_txn txn, uint32_t version,
                               uint32_t timeout_ms, uint32_t *out_version)
{
    if (version != pg.version) {
        *out_version = pg.version;
        return OK;
    }
    uint64_t deadline = timeout_ms == NET_WAIT_FOREVER
                            ? DEADLINE_NEVER : now() + (uint64_t)timeout_ms * NS_PER_MS;
    struct later *l = later_new(ctx, LATER_WAIT, txn, deadline);
    if (!l)
        return ERR_NO_RESOURCES;
    l->version = version;
    return IDL_LATER;
}

static status_t op_chip_counts(void *ctx, struct idl_txn txn, uint8_t out_counts[256])
{
    (void)out_counts;
    if (!pg.dev)
        return ERR_NOT_FOUND;
    struct later *l = later_new(ctx, LATER_CHIP, txn, now() + CHIP_WAIT);
    if (!l)
        return ERR_NO_RESOURCES;
    status_t st = dev_ask_stats(pg.dev);
    if (st != OK) {
        later_done(l);
        return st;
    }
    return IDL_LATER;
}

static status_t op_udp(void *ctx, uint16_t port, handle_t *out_socket, uint16_t *out_port)
{
    struct opener *o = ctx;
    unsigned i;
    if (!listen_may_bind(o, port))
        return ERR_ACCESS_DENIED;
    if (o->socks >= NET_SOCKETS_PER_OPENER)
        return ERR_NO_RESOURCES;
    return sock_open((unsigned)(o - pg.o), port, false, out_socket, out_port, &i);
}

static status_t op_udp_rings(void *ctx, uint16_t port, uint32_t tx_bytes, uint32_t rx_bytes,
                             handle_t *out_socket, handle_t *out_ring, handle_t *out_to_stack,
                             handle_t *out_to_prog, uint16_t *out_port, uint32_t *out_tx_bytes,
                             uint32_t *out_rx_bytes)
{
    struct opener *o = ctx;
    unsigned i;
    handle_t hs[3];
    if (!listen_may_bind(o, port))
        return ERR_ACCESS_DENIED;
    if (o->socks >= NET_SOCKETS_PER_OPENER)
        return ERR_NO_RESOURCES;
    status_t st = sock_open((unsigned)(o - pg.o), port, false, out_socket, out_port, &i);
    if (st != OK)
        return st;
    st = sock_rings_make(i, &tx_bytes, &rx_bytes, hs);
    if (st != OK) {
        jam_handle_close(*out_socket);   /* its client end: never sent */
        sock_close(i);
        return st;
    }
    *out_ring = hs[0];
    *out_to_stack = hs[1];
    *out_to_prog = hs[2];
    *out_tx_bytes = tx_bytes;
    *out_rx_bytes = rx_bytes;
    return OK;
}

static status_t op_echo(void *ctx, struct idl_txn txn, uint32_t address, uint16_t seq,
                        uint16_t size, uint32_t timeout_ms, uint32_t *out_rtt_us,
                        uint8_t *out_ttl, uint16_t *out_size)
{
    (void)out_rtt_us;
    (void)out_ttl;
    (void)out_size;
    struct opener *o = ctx;
    if (!ctl_unicast(address) || size > NET_DGRAM_MAX || !timeout_ms ||
        timeout_ms > NET_ECHO_TIMEOUT_MAX)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < LATER_MAX; i++)
        if (pg.l[i].kind == LATER_ECHO && pg.l[i].opener == (unsigned)(o - pg.o) &&
            pg.l[i].addr == address && pg.l[i].seq == seq)
            return ERR_BAD_STATE;   /* its reply couldn't be told from the other's */
    struct later *l = later_new(o, LATER_ECHO, txn, now() + (uint64_t)timeout_ms * NS_PER_MS);
    if (!l)
        return ERR_NO_RESOURCES;
    l->addr = address;
    l->seq = seq;
    l->sent = now();
    status_t st = stack_echo_send(address, o->echo_id, seq, size);
    if (st != OK) {
        later_done(l);
        return st;
    }
    pg.c.echoes_sent++;
    return IDL_LATER;
}

static const struct net_ops opener_ops = {
    .iface = op_iface,
    .wait_change = op_wait_change,
    .counts = op_counts,
    .chip_counts = op_chip_counts,
    .udp = op_udp,
    .echo = op_echo,
    .udp_rings = op_udp_rings,
    .tcp = tcpsock_op_tcp,
    .tcp_listener = tcpsock_op_listener,
};

/* The shared channel answers only what is answered at once (a reply that
 * came later could go to any of its holders). */
static const struct net_ops shared_ops = { .iface = op_iface, .counts = op_counts };

/* ---- the channels ------------------------------------------------------------------ */

status_t progs_connect(void *ctx, handle_t *out)
{
    static const struct opener_kind plain = { .cls = CLASS_PROG };
    const struct opener_kind *k = ctx ? ctx : &plain;
    if (k->cls == CLASS_PROG && pg.held[CLASS_PROG].openers >= NET_PROG_OPENERS) {
        pg.c.refused_shares++;
        return ERR_NO_RESOURCES;
    }
    for (unsigned i = 0; i < NET_OPENERS; i++) {
        struct opener *o = &pg.o[i];
        if (o->ch)
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        o->gen++;
        st = jam_port_bind(pg.port, mine, key_of(i), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
        if (st != OK) {
            jam_handle_close(mine);
            jam_handle_close(theirs);
            return st;
        }
        *o = (struct opener){ .ch = mine, .gen = o->gen, .pending = true, .cls = k->cls,
                              .echo_id = (uint16_t)((os_random_u32() & ~0x1fu) | i),
                              .listen = k->listen };
        pg.held[k->cls].openers++;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

uint32_t progs_shared_dispatch(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                               uint32_t *rhn)
{
    return net_dispatch(&shared_ops, ctx, req, n, rep, rhs, rhn);
}

static void serve_shared(uint8_t cls)
{
    pg.shared_pending[cls] = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = svc_serve_request(pg.shared[cls], progs_shared_dispatch, progs_connect,
                                        (void *)&kinds[cls]);
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT) {
            /* Every holder is gone, init's duplicate too: no new openers. */
            nstack_log("%s's channel is closed (%s): no new openers",
                       cls == CLASS_SYS ? "/svc/net-sys" : "/svc/net", status_str(st));
            (void)jam_port_unbind(pg.port, pg.shared[cls], shared_key[cls]);   /* was bound */
            jam_handle_close(pg.shared[cls]);
            pg.shared[cls] = HANDLE_INVALID;
        }
        return;
    }
    pg.shared_pending[cls] = true;
}

static void opener_close(unsigned i)
{
    struct opener *o = &pg.o[i];
    later_drop(i);
    sock_close_opener(i);
    tcpsock_close_opener(i);
    (void)jam_port_unbind(pg.port, o->ch, key_of(i));   /* bound since progs_connect */
    jam_handle_close(o->ch);
    pg.held[o->cls].openers--;
    o->ch = HANDLE_INVALID;
    o->pending = false;
}

static void serve_opener(unsigned i)
{
    struct opener *o = &pg.o[i];
    o->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = net_serve_one(o->ch, &opener_ops, o);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            opener_close(i);
        else if (st != ERR_SHOULD_WAIT)
            printf("netstack: reading an opener's channel: %s\n", status_str(st));
        return;
    }
    o->pending = true;   /* its budget is spent: more may be queued */
}

void progs_serve(void)
{
    for (uint8_t c = 0; c < CLASSES; c++)
        if (pg.shared[c] && pg.shared_pending[c])
            serve_shared(c);
    for (unsigned i = 0; i < NET_OPENERS; i++)
        if (pg.o[i].ch && pg.o[i].pending)
            serve_opener(i);
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (pg.s[i].ch && pg.s[i].pending)
            sock_serve(i);
    sock_tx_all();
}

bool progs_pending(void)
{
    for (uint8_t c = 0; c < CLASSES; c++)
        if (pg.shared[c] && pg.shared_pending[c])
            return true;
    for (unsigned i = 0; i < NET_OPENERS; i++)
        if (pg.o[i].ch && pg.o[i].pending)
            return true;
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (pg.s[i].ch && pg.s[i].pending)
            return true;
    return sock_tx_pending();
}

void progs_flush(void)
{
    sock_flush();
}

bool progs_packet(const struct port_packet *p)
{
    uint32_t low = (uint32_t)(p->key & 0xff), gen = (uint32_t)(p->key >> 8);
    if (p->key == shared_key[CLASS_PROG] || p->key == shared_key[CLASS_SYS]) {
        uint8_t c = p->key == shared_key[CLASS_SYS] ? CLASS_SYS : CLASS_PROG;
        pg.shared_pending[c] = pg.shared[c] != 0;
    } else if (low >= KEY_RING && low < KEY_RING + SOCK_SLOTS) {
        sock_ring_event(low - KEY_RING, gen);
    } else if (low >= KEY_OPENER && low < KEY_OPENER + NET_OPENERS) {
        struct opener *o = progs_opener(low - KEY_OPENER, gen);
        if (o)
            o->pending = true;
    } else if (low >= KEY_SOCK && low < KEY_SOCK + SOCK_SLOTS) {
        struct sock *s = &pg.s[low - KEY_SOCK];
        if (s->ch && s->gen == gen)
            s->pending = true;
    } else {
        return false;
    }
    return true;
}

/* Watch shared channel h of class cls (0: none). */
static status_t take_shared(uint8_t cls, handle_t h)
{
    if (!h)
        return OK;
    pg.shared[cls] = h;
    pg.shared_pending[cls] = true;   /* connects may be queued from before a restart */
    return jam_port_bind(pg.port, h, shared_key[cls], SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_PERSISTENT);
}

status_t progs_init(handle_t port, handle_t shared, handle_t shared_sys, struct dev *d)
{
    pg.port = port;
    pg.dev = d;
    pg.version = 1;
    ctl_changed = changed;
    stack_echo_input = echo_in;
    dev_stats_done = chip_done;
    sock_hooks();
    if (!shared)
        nstack_log("started without /svc/net's channel (SR_USER + 1): no program can use it");
    if (!shared_sys)
        nstack_log("started without /svc/net-sys's channel (SR_USER + 2): no reserve for "
                   "the network's services");
    status_t st = take_shared(CLASS_PROG, shared);
    return st == OK ? take_shared(CLASS_SYS, shared_sys) : st;
}
