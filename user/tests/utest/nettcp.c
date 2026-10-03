/* utest: netstack's TCP (stack.c's TCP edge, tcp.c's connections) driven
 * in-process: the test is the peer, byte by byte on the wire (tcppeer.c),
 * and the program, on the other side of each connection's rings.
 *
 * Covered here: a connection netstack opens, 100 KB each way through
 * rings smaller than that, and a close where netstack's FIN goes first;
 * a listener (the SYN-ACK's window is the accepted rings', bytes that
 * come before the program accepts wait in the ring, the peer's FIN first,
 * the backlog full, the listener closed); a slow reader whose window
 * closes and opens with its reads and never holds a byte in lwIP; and
 * resets (refused, by the peer, by netstack, one out of the window
 * ignored); and the card's tx ring full (the segments wait in lwIP and go
 * when room comes, each byte once). nettcp.h has the fixture; tcpabuse.c
 * the hostile tests. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <sockring.h>
#include "netpkt.h"
#include "nettcp.h"
#include "stack.h"
#include "tcp.h"
#include "tcppeer.h"
#include "utest.h"

static uint8_t ring_mem[FIX_RINGS][FIX_BYTES] __attribute__((aligned(4096)));
static bool ring_used[FIX_RINGS];
static struct stack_counts base;   /* lwIP's memory with nothing going on */

/* ---- the fixture (nettcp.h) -------------------------------------------------------- */

bool fix_up(void)
{
    struct stack_edge e = { .tx = tp_edge_tx };
    memcpy(e.mac, pkt_our_mac, 6);
    stack_stop();
    CHECK_ST(stack_start(&e), OK);
    stack_set_link(true);
    struct stack_ipv4 ip = { OUR_IP, MASK24, GW_IP };
    stack_set_ipv4(&ip);
    ntcp_init();
    tp_room(TP_CAP);
    tp_arp(pkt_peer_mac, PEER_IP);
    tp_arp(pkt_other_mac, OTHER_IP);
    tp_forget();
    stack_get_counts(&base);
    return true;
}

bool fix_down(void)
{
    uint32_t conns, listeners;
    ntcp_census(&conns, &listeners);
    CHECK_EQ(conns, 0);
    CHECK_EQ(listeners, 0);
    for (unsigned i = 0; i < FIX_RINGS; i++)
        CHECK(!ring_used[i]);
    stack_clear();   /* what lwIP still runs on our address is reset */
    stack_stop();
    tp_forget();
    struct stack_tcp_counts t = fix_counts();
    CHECK_EQ(t.live + t.half_open, 0);
    CHECK_EQ(t.segs_used, 0);
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK_EQ(c.rx_buffers_used, 0);
    CHECK_EQ(c.heap_used, base.heap_used);
    return true;
}

bool fix_rings(uint32_t tx, uint32_t rx, struct sockring *stack_side, struct sockring *prog,
               unsigned *slot)
{
    uint64_t bytes = sockring_bytes(tx, rx);
    CHECK(bytes <= FIX_BYTES);
    unsigned i = 0;
    while (i < FIX_RINGS && ring_used[i])
        i++;
    CHECK(i < FIX_RINGS);
    memset(ring_mem[i], 0, bytes);
    CHECK_ST(sockring_make(stack_side, ring_mem[i], SOCKRING_STREAM, tx, rx), OK);
    CHECK_ST(sockring_attach(prog, ring_mem[i], bytes, SOCKRING_STREAM, tx, rx), OK);
    ring_used[i] = true;
    *slot = i;
    return true;
}

void fix_rings_free(unsigned slot)
{
    ring_used[slot] = false;
}

bool fix_turn(struct cx *x, struct tp_seg *s, unsigned max, unsigned *n, bool rst_ok)
{
    unsigned others;
    ntcp_work();
    CHECK(tp_read(&x->p, s, max, n, &others));
    for (unsigned i = 0; i < *n; i++) {
        if (!rst_ok && (s[i].flags & TP_RST))
            FAIL("segment %u of %u is a reset", i, *n);
        if (s[i].flags & TP_ACK) {
            x->acked = s[i].ack;
            x->window = s[i].win;
        }
    }
    return true;
}

bool fix_connect(struct cx *x, uint16_t port, uint32_t tx, uint32_t rx)
{
    struct sockring ss;
    struct tp_seg s[4];
    unsigned n;
    *x = (struct cx){ .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = port, .snd = PEER_ISN,
                             .win = 0xffff } };
    CHECK(fix_rings(tx, rx, &ss, &x->prog, &x->slot));
    CHECK_ST(ntcp_conn_new(x, &x->c), OK);
    ntcp_conn_rings(x->c, &ss);
    CHECK_ST(ntcp_connect(x->c, PEER_IP, port), OK);
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_CONNECTING);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_SYN);
    CHECK_EQ(s[0].mss, STACK_TCP_MSS);
    CHECK_EQ(s[0].len, 0);
    x->p.our = s[0].sport;
    x->p.rcv = s[0].seq + 1;
    tp_send(&x->p, TP_SYN | TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_ACK);
    CHECK_EQ(s[0].ack, x->p.snd);
    CHECK_EQ(s[0].win, rx < STACK_TCP_WND_PLAIN ? rx : STACK_TCP_WND_PLAIN);   /* the ring's */
    struct sockring_status st = fix_status(x);
    CHECK_EQ(st.state, SOCKRING_STATE_OPEN);
    CHECK_EQ(st.error, OK);
    CHECK(x->c->signalled & SOCKRING_SIG_STATE);
    return true;
}

void fix_free(struct cx *x)
{
    ntcp_conn_free(x->c);
    fix_rings_free(x->slot);
}

void fix_tick(void)
{
    uint64_t t = now(), d = stack_poll(), last = t + 300 * NS_PER_MS;
    (void)jam_nanosleep(d < last ? d : last);   /* a test's own sleep: nothing to do if it fails */
    stack_poll();
    ntcp_work();
}

void fix_publish(struct cx *x, struct sockring_end *e)
{
    if (sockring_publish(e))
        ntcp_kick(x->c);
}

struct sockring_status fix_status(const struct cx *x)
{
    struct sockring_status s;
    sockring_status_get(&x->prog, &s);
    return s;
}

struct stack_tcp_counts fix_counts(void)
{
    struct stack_tcp_counts t;
    stack_tcp_get_counts(&t);
    return t;
}

/* ---- moving bytes ------------------------------------------------------------------ */

/* The program writes `total` bytes of stream `id` as its tx ring takes
 * them; the peer takes what comes, in order, and acks it. */
bool fix_send(struct cx *x, uint32_t id, size_t total, uint8_t *got_buf)
{
    static uint8_t tmp[16 * 1024];
    static struct tp_seg s[TP_CAP];
    size_t put = 0, got = 0;
    unsigned ticks = 0;
    for (unsigned round = 0; got < total; round++) {
        CHECK(round < 3000);
        uint32_t n = sockring_room(&x->prog.tx);
        n = (uint32_t)(n < total - put ? n : total - put);
        n = n < sizeof(tmp) ? n : sizeof(tmp);
        tp_fill(id, put, tmp, n);
        CHECK_EQ(sockring_stream_write(&x->prog.tx, tmp, n), n);
        put += n;
        if (n)
            fix_publish(x, &x->prog.tx);
        unsigned k;
        CHECK(fix_turn(x, s, TP_CAP, &k, false));
        size_t before = got;
        tp_absorb(&x->p, s, k, got_buf, total, &got);
        if (got > before) {
            tp_send(&x->p, TP_ACK, NULL, 0);
        } else if (!n) {
            if (++ticks > 20)
                FAIL("stalled: %zu of %zu written, %zu arrived", put, total, got);
            fix_tick();
        }
    }
    CHECK_EQ(got, total);
    CHECK(tp_same(id, 0, got_buf, total));
    return true;
}

/* The peer sends what netstack's window (from its ACKs) allows. */
static size_t peer_fill(struct cx *x, uint32_t id, size_t sent, size_t total)
{
    static uint8_t tmp[STACK_TCP_MSS];
    int32_t room = (int32_t)(x->acked + x->window - x->p.snd);
    while (sent < total && room > 0) {
        size_t n = total - sent;
        n = n < STACK_TCP_MSS ? n : STACK_TCP_MSS;
        n = n < (size_t)room ? n : (size_t)room;
        tp_fill(id, sent, tmp, n);
        tp_send(&x->p, TP_ACK | TP_PSH, tmp, n);
        sent += n;
        room -= (int32_t)n;
    }
    return sent;
}

/* The peer sends `total` bytes of stream `id`, the program reading
 * `chunk` at a time; checked at every step: the bytes, nothing past the
 * ring, no receive buffer held in lwIP. */
bool fix_receive(struct cx *x, uint32_t id, size_t total, uint32_t chunk)
{
    static uint8_t tmp[64 * 1024];
    static struct tp_seg s[TP_CAP];
    size_t sent = 0, read = 0;
    unsigned ticks = 0;
    for (unsigned round = 0; read < total; round++) {
        CHECK(round < 6000);
        size_t was_sent = sent, was_read = read;
        sent = peer_fill(x, id, sent, total);
        unsigned k;
        CHECK(fix_turn(x, s, TP_CAP, &k, false));
        for (unsigned i = 0; i < k; i++)
            CHECK_EQ(s[i].len, 0);
        struct stack_counts c;
        stack_get_counts(&c);
        CHECK_EQ(c.rx_buffers_used, 0);
        CHECK_EQ(sent - read, sockring_ready(&x->prog.rx));   /* every byte sent is in the ring */
        uint32_t want = chunk < sizeof(tmp) ? chunk : sizeof(tmp);
        uint32_t n = sockring_stream_read(&x->prog.rx, tmp, want);
        CHECK(tp_same(id, read, tmp, n));
        read += n;
        if (n)
            fix_publish(x, &x->prog.rx);
        CHECK(fix_turn(x, s, TP_CAP, &k, false));
        if (sent != was_sent || read != was_read)
            continue;
        if (++ticks > 20)
            FAIL("stalled: %zu of %zu sent, %zu read, window %u at %u", sent, total, read,
                 x->window, x->acked);
        fix_tick();   /* an ACK lwIP delays */
    }
    return true;
}

/* The one FIN among n segments (the others pure ACKs): its index. */
static bool fin_of(const struct tp_seg *s, unsigned n, unsigned *at)
{
    unsigned fins = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i].flags & TP_FIN) {
            fins++;
            *at = i;
        } else {
            CHECK_EQ(s[i].flags, TP_ACK);
            CHECK_EQ(s[i].len, 0);
        }
    }
    CHECK_EQ(fins, 1);
    return true;
}

/* Netstack's FIN first (the program's END), then the peer's: CLOSED, OK. */
static bool close_ours_first(struct cx *x)
{
    struct tp_seg s[8];
    unsigned n, at = 0;
    if (sockring_finish(&x->prog.tx))
        ntcp_kick(x->c);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK(fin_of(s, n, &at));
    CHECK_EQ(s[at].seq, x->p.rcv);
    CHECK_EQ(s[at].len, 0);
    tp_absorb(&x->p, s, n, NULL, 0, &(size_t){ 0 });
    tp_send(&x->p, TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_OPEN);   /* the peer's half is still open */
    tp_send(&x->p, TP_FIN | TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].ack, x->p.snd);   /* its FIN acked */
    CHECK(sockring_at_end(&x->prog.rx));
    struct sockring_status st = fix_status(x);
    CHECK_EQ(st.state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(st.error, OK);
    return true;
}

/* ---- the tests ------------------------------------------------------------------------ */

bool t_nettcp_connect(void)
{
    static uint8_t buf[100000];
    struct cx x;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8080, 16384, 16384));
    CHECK(fix_send(&x, 1, sizeof(buf), buf));
    CHECK(fix_receive(&x, 2, sizeof(buf), 5000));
    CHECK(close_ours_first(&x));
    fix_free(&x);
    CHECK(fix_counts().time_wait >= 1);   /* ours: lwIP's now */
    CHECK(fix_down());
    return true;
}

/* The listener test's connections: their rings, and the program's side
 * of each (by ring slot; the connection's owner is slot + 1). */
static struct sockring lst_prog[FIX_RINGS];

status_t fix_lst_rings(struct ntcp_listener *l, struct ntcp_conn *c)
{
    struct sockring ss, prog;
    unsigned slot;
    if (!fix_rings(8192, l->req.rx_size, &ss, &prog, &slot))
        return ERR_NO_RESOURCES;
    lst_prog[slot] = prog;
    ntcp_conn_rings(c, &ss);
    c->owner = (void *)(uintptr_t)(slot + 1);
    return OK;
}

void fix_lst_drop(struct ntcp_listener *l, struct ntcp_conn *c)
{
    (void)l;
    fix_rings_free((unsigned)(uintptr_t)c->owner - 1);
}

/* A peer's handshake with listener port `port`: the SYN-ACK checked (its
 * window `win`), the ACK sent. */
bool fix_handshake(struct cx *x, uint16_t port, uint16_t from, uint32_t win)
{
    struct tp_seg s[4];
    unsigned n;
    *x = (struct cx){ .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = from, .our = port,
                             .snd = PEER_ISN, .win = 0xffff } };
    tp_send(&x->p, TP_SYN, NULL, 0);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_SYN | TP_ACK);
    CHECK_EQ(s[0].ack, x->p.snd);
    CHECK_EQ(s[0].mss, STACK_TCP_MSS);
    CHECK_EQ(s[0].win, win);
    x->p.rcv = s[0].seq + 1;
    return true;
}

bool fix_accept(struct ntcp_listener *l, struct cx *x)
{
    CHECK_ST(ntcp_accept(l, &x->c), OK);
    x->slot = (unsigned)(uintptr_t)x->c->owner - 1;
    x->prog = lst_prog[x->slot];
    return true;
}

/* The peer closes first: its FIN, then the program's END, then the last
 * ACK frees the connection in lwIP at once (no TIME_WAIT on our side). */
static bool close_peer_first(struct cx *x)
{
    struct tp_seg s[8];
    unsigned n;
    tp_send(&x->p, TP_FIN | TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK(sockring_at_end(&x->prog.rx));
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_OPEN);
    if (sockring_finish(&x->prog.tx))
        ntcp_kick(x->c);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK(n >= 1);
    CHECK(s[n - 1].flags & TP_FIN);
    tp_absorb(&x->p, s, n, NULL, 0, &(size_t){ 0 });
    uint32_t tw = fix_counts().time_wait;
    tp_send(&x->p, TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK_EQ(n, 0);
    struct sockring_status st = fix_status(x);
    CHECK_EQ(st.state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(st.error, OK);
    CHECK_EQ(fix_counts().time_wait, tw);
    return true;
}

static bool listen_one(struct ntcp_listener *l)
{
    static uint8_t buf[3000];
    struct cx x;
    CHECK(fix_handshake(&x, l->port, 30000, 8192));   /* the accepted rings' window, not lwIP's */
    tp_fill(3, 0, buf, 100);
    tp_send(&x.p, TP_ACK | TP_PSH, buf, 100);   /* bytes with the handshake's ACK */
    CHECK_EQ(l->n, 1);
    CHECK(fix_accept(l, &x));
    CHECK_EQ(l->n, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_OPEN);
    CHECK_EQ(sockring_stream_read(&x.prog.rx, buf, sizeof(buf)), 100);   /* waited in the ring */
    CHECK(tp_same(3, 0, buf, 100));
    fix_publish(&x, &x.prog.rx);
    CHECK(fix_send(&x, 4, sizeof(buf), buf));
    CHECK(close_peer_first(&x));
    fix_free(&x);
    return true;
}

bool t_nettcp_listen(void)
{
    struct ntcp_listener *l, *l2;
    struct tp_seg s[8];
    unsigned n;
    CHECK(fix_up());
    struct ntcp_listen_req req = { .port = 7000, .backlog = 4, .rx_size = 8192,
                                   .rings = fix_lst_rings, .drop = fix_lst_drop };
    CHECK_ST(ntcp_listen(&req, &l), OK);
    CHECK_EQ(l->port, 7000);
    CHECK_ST(ntcp_listen(&req, &l2), ERR_ALREADY_BOUND);
    CHECK(listen_one(l));
    /* A backlog of 2: a third handshake's SYN goes unanswered. */
    req.port = 7001;
    req.backlog = 2;
    CHECK_ST(ntcp_listen(&req, &l2), OK);
    struct cx a, b, c;
    CHECK(fix_handshake(&a, 7001, 31000, 8192));
    tp_send(&a.p, TP_ACK, NULL, 0);
    CHECK(fix_handshake(&b, 7001, 31001, 8192));
    tp_send(&b.p, TP_ACK, NULL, 0);
    CHECK_EQ(l2->n, 2);
    c = (struct cx){ .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = 31002, .our = 7001,
                            .snd = PEER_ISN, .win = 0xffff } };
    tp_send(&c.p, TP_SYN, NULL, 0);
    CHECK(fix_turn(&c, s, 8, &n, false));
    CHECK_EQ(n, 0);
    /* Closed: the two waiting are reset; a SYN then is refused. */
    ntcp_unlisten(l2);
    CHECK(fix_turn(&a, s, 8, &n, true));
    CHECK_EQ(n, 1);
    CHECK(s[0].flags & TP_RST);
    CHECK(fix_turn(&b, s, 8, &n, true));
    CHECK_EQ(n, 1);
    CHECK(s[0].flags & TP_RST);
    ntcp_unlisten(l);
    struct cx d = { .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = 31003, .our = 7000,
                           .snd = PEER_ISN, .win = 0xffff } };
    tp_send(&d.p, TP_SYN, NULL, 0);
    CHECK(fix_turn(&d, s, 8, &n, true));
    CHECK_EQ(n, 1);
    CHECK(s[0].flags & TP_RST);
    tp_forget();
    CHECK(fix_down());
    return true;
}

/* A reader slower than its sender: the window is the ring's room. A 4 KiB
 * ring announces 4096, closes to 0 when full (bytes past it are dropped
 * by lwIP, never kept), the program's read signals netstack (it sleeps on
 * the ring past half full) and the window opens; then 64 KiB through it,
 * 700 bytes a read. */
bool t_nettcp_slow_reader(void)
{
    static uint8_t buf[8192];
    struct cx x;
    struct tp_seg s[16];
    unsigned n;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8081, 4096, 4096));
    CHECK_EQ(x.window, 4096);
    size_t sent = peer_fill(&x, 5, 0, sizeof(buf));
    CHECK_EQ(sent, 4096);
    CHECK(fix_turn(&x, s, 16, &n, false));
    if (x.acked != x.p.snd) {   /* the last ACK may be delayed */
        fix_tick();
        CHECK(fix_turn(&x, s, 16, &n, false));
    }
    CHECK_EQ(x.acked, x.p.snd);
    CHECK_EQ(x.window, 0);
    CHECK_EQ(sockring_ready(&x.prog.rx), 4096);
    CHECK_EQ(__atomic_load_n(&x.prog.page->rx_prod.waits, __ATOMIC_RELAXED), 1);
    /* Past the window: dropped, and nothing kept. */
    tp_fill(5, 4096, buf, 1000);
    tp_send(&x.p, TP_ACK, buf, 1000);
    x.p.snd -= 1000;   /* the peer will send them again */
    CHECK(fix_turn(&x, s, 16, &n, false));
    CHECK_EQ(sockring_ready(&x.prog.rx), 4096);
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK_EQ(c.rx_buffers_used, 0);
    /* A read of 3000: the program's publish wakes netstack, which opens
     * the window by 3000 at once. */
    CHECK_EQ(sockring_stream_read(&x.prog.rx, buf, 3000), 3000);
    CHECK(tp_same(5, 0, buf, 3000));
    CHECK(sockring_publish(&x.prog.rx));
    ntcp_kick(x.c);
    CHECK(fix_turn(&x, s, 16, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(x.window, 3000);
    CHECK_EQ(sockring_stream_read(&x.prog.rx, buf, sizeof(buf)), 1096);
    fix_publish(&x, &x.prog.rx);
    CHECK(fix_turn(&x, s, 16, &n, false));
    /* The stream from where the ring left it, in 700-byte reads. */
    x.p.snd = x.acked;
    CHECK(fix_receive(&x, 6, 64 * 1024, 700));
    CHECK(close_ours_first(&x));
    fix_free(&x);
    CHECK(fix_down());
    return true;
}

/* n segments: pure ACKs, then a RST at sequence number seq. */
static bool rst_last(const struct tp_seg *s, unsigned n, uint32_t seq)
{
    CHECK(n >= 1);
    for (unsigned i = 0; i + 1 < n; i++)
        CHECK(s[i].flags == TP_ACK && !s[i].len);
    CHECK(s[n - 1].flags & TP_RST);
    CHECK_EQ(s[n - 1].seq, seq);
    return true;
}

bool t_nettcp_reset(void)
{
    struct cx x;
    struct tp_seg s[8];
    unsigned n;
    CHECK(fix_up());
    /* Refused: our SYN answered with a reset. */
    struct sockring ss;
    x = (struct cx){ .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = 9, .win = 0xffff } };
    CHECK(fix_rings(4096, 4096, &ss, &x.prog, &x.slot));
    CHECK_ST(ntcp_conn_new(&x, &x.c), OK);
    ntcp_conn_rings(x.c, &ss);
    CHECK_ST(ntcp_connect(x.c, PEER_IP, 9), OK);
    CHECK(fix_turn(&x, s, 8, &n, false));
    x.p.our = s[0].sport;
    x.p.rcv = s[0].seq + 1;
    tp_send(&x.p, TP_RST | TP_ACK, NULL, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(fix_status(&x).error, ERR_NOT_FOUND);
    fix_free(&x);
    /* Reset by the peer: what came before stays in the ring. A reset out
     * of the window is ignored first (answered with an ACK at most). */
    CHECK(fix_connect(&x, 8082, 4096, 4096));
    tp_send(&x.p, TP_ACK | TP_PSH, "early", 5);
    x.p.snd += 100000;
    tp_send(&x.p, TP_RST, NULL, 0);
    x.p.snd -= 100000;
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_OPEN);
    tp_send(&x.p, TP_RST, NULL, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(fix_status(&x).error, ERR_PEER_CLOSED);
    CHECK_EQ(sockring_ready(&x.prog.rx), 5);
    CHECK(!sockring_at_end(&x.prog.rx));   /* not a FIN */
    fix_free(&x);
    /* Reset by netstack: a RST on the wire, CLOSED with the reason given. */
    CHECK(fix_connect(&x, 8083, 4096, 4096));
    ntcp_conn_abort(x.c, ERR_CANCELED);
    CHECK(fix_turn(&x, s, 8, &n, true));
    CHECK(rst_last(s, n, x.p.rcv));
    CHECK_EQ(fix_status(&x).error, ERR_CANCELED);
    /* Freed with bytes unread: a reset too (lwIP sends the ACK it was
     * delaying first). */
    fix_free(&x);
    CHECK(fix_connect(&x, 8084, 4096, 4096));
    tp_send(&x.p, TP_ACK | TP_PSH, "unread", 6);
    CHECK(fix_turn(&x, s, 8, &n, false));
    fix_free(&x);
    CHECK(fix_turn(&x, s, 8, &n, true));
    CHECK(rst_last(s, n, x.p.rcv));
    CHECK(fix_down());
    return true;
}

/* The card's tx ring full: the segments wait in lwIP and go as soon as the
 * loop says there is room again (stack_tx_resume, which netstack calls on
 * the driver's NETDEV_SIG_TX_ROOM), not on lwIP's next timer; each byte
 * once, in order. */
bool t_nettcp_card_full(void)
{
    static struct tp_seg s[TP_CAP];
    static uint8_t buf[4000], got[4000];
    struct cx x;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8096, 16384, 16384));
    tp_forget();
    tp_room(0);
    tp_fill(7, 0, buf, sizeof(buf));   /* 3 segments: the initial congestion window's */
    CHECK_EQ(sockring_stream_write(&x.prog.tx, buf, sizeof(buf)), sizeof(buf));
    fix_publish(&x, &x.prog.tx);
    ntcp_work();
    CHECK_EQ(tp_caught(), 0);
    CHECK(stack_tx_blocked());
    ntcp_work();   /* a turn of its own sends nothing more: lwIP holds them */
    CHECK_EQ(tp_caught(), 0);
    tp_room(1);
    stack_tx_resume();   /* room for one: one goes, and the edge is full again */
    CHECK_EQ(tp_caught(), 1);
    CHECK(stack_tx_blocked());
    tp_room(TP_CAP);
    stack_tx_resume();
    CHECK(!stack_tx_blocked());
    unsigned n, others;
    size_t have = 0;
    CHECK(tp_read(&x.p, s, TP_CAP, &n, &others));
    CHECK_EQ(n, 3);
    CHECK_EQ(tp_absorb(&x.p, s, n, got, sizeof(got), &have), 3);
    CHECK_EQ(have, sizeof(buf));
    CHECK(tp_same(7, 0, got, sizeof(got)));
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(fix_turn(&x, s, TP_CAP, &n, false));
    CHECK_EQ(n, 0);   /* nothing sent twice */
    fix_free(&x);
    CHECK(fix_down());
    return true;
}
