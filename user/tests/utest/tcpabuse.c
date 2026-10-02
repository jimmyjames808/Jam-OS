/* utest: netstack's TCP against hostile peers and programs (the fixture
 * is nettcp.h's): a SYN flood held to the listener's backlog while a
 * live connection and another listener go on; lwIP's pcb pool full of
 * half-open connections, where a program's connect still gets one and a
 * SYN does not; malformed segments dropped without an answer beyond an
 * ACK; a fuzz of thousands of mutated segments that leaves nothing behind;
 * and a program writing counts out of range into its rings, whose
 * connection is reset. */
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

/* A peer that only sends: port `from` to our port `to`. */
static struct tp peer_at(uint16_t from, uint16_t to, uint32_t isn)
{
    return (struct tp){ .ip = PEER_IP, .mac = pkt_peer_mac, .port = from, .our = to, .snd = isn,
                        .win = 0xffff };
}

/* ---- floods ---------------------------------------------------------------------- */

bool t_nettcp_syn_flood(void)
{
    static uint8_t buf[20000];
    struct ntcp_listener *a, *b;
    struct cx live, x;
    CHECK(fix_up());
    struct ntcp_listen_req req = { .port = 7100, .backlog = 4, .rx_size = 8192,
                                   .rings = fix_lst_rings, .drop = fix_lst_drop };
    CHECK_ST(ntcp_listen(&req, &a), OK);
    req.port = 7101;
    CHECK_ST(ntcp_listen(&req, &b), OK);
    CHECK(fix_connect(&live, 8090, 8192, 8192));
    uint32_t half = fix_counts().half_open, none = fix_counts().pcbs_none;
    for (unsigned i = 0; i < 500; i++) {
        struct tp f = peer_at((uint16_t)(20000 + i), 7100, PEER_ISN + i * 7919);
        tp_send(&f, TP_SYN, NULL, 0);
    }
    CHECK_EQ(tp_caught(), 4);   /* a SYN-ACK for each the backlog holds, nothing else */
    tp_forget();
    CHECK_EQ(fix_counts().half_open, half + 4);
    CHECK_EQ(fix_counts().pcbs_none, none);   /* the backlog, not the pool, said no */
    /* The live connection, both ways, and the other listener. */
    CHECK(fix_send(&live, 7, sizeof(buf), buf));
    CHECK(fix_receive(&live, 8, sizeof(buf), 4096));
    CHECK(fix_handshake(&x, 7101, 40000, 8192));
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(fix_accept(b, &x));
    CHECK(fix_send(&x, 9, 3000, buf));
    fix_free(&x);
    fix_free(&live);
    ntcp_unlisten(a);
    ntcp_unlisten(b);
    tp_forget();
    CHECK(fix_down());   /* the flood's half-open ones end with the address */
    return true;
}

/* Ring memory every connection of the pool test shares: they never carry
 * a byte. */
static uint8_t pool_mem[SOCKRING_HDR + 2 * SOCKRING_MIN] __attribute__((aligned(4096)));

static bool pool_connect(struct ntcp_conn **out)
{
    struct sockring r;
    CHECK_ST(sockring_make(&r, pool_mem, SOCKRING_STREAM, SOCKRING_MIN, SOCKRING_MIN), OK);
    CHECK_ST(ntcp_conn_new(NULL, out), OK);
    ntcp_conn_rings(*out, &r);
    CHECK_ST(ntcp_connect(*out, PEER_IP, 9000), OK);
    tp_forget();   /* its SYN */
    return true;
}

static status_t no_rings(struct ntcp_listener *l, struct ntcp_conn *c)
{
    (void)l;
    (void)c;
    return ERR_NO_RESOURCES;   /* nothing here completes a handshake */
}

static void no_drop(struct ntcp_listener *l, struct ntcp_conn *c)
{
    (void)l;
    (void)c;
}

/* Every listener 15 half-open connections deep (240), then programs'
 * connects until lwIP's pool is full: a SYN gets no pcb (and no answer),
 * a program's connect still does (lwIP recycles a half-open one). */
bool t_nettcp_pool_full(void)
{
    static struct ntcp_listener *l[STACK_TCP_LISTENERS];
    static struct ntcp_conn *c[STACK_TCP_CONNS];
    unsigned nc = 0;
    CHECK(fix_up());
    for (unsigned i = 0; i < STACK_TCP_LISTENERS; i++) {
        struct ntcp_listen_req req = { .port = (uint16_t)(7200 + i),
                                       .backlog = STACK_TCP_BACKLOG, .rx_size = SOCKRING_MIN,
                                       .rings = no_rings, .drop = no_drop };
        CHECK_ST(ntcp_listen(&req, &l[i]), OK);
        for (unsigned k = 0; k < STACK_TCP_BACKLOG - 1; k++) {
            struct tp f = peer_at((uint16_t)(21000 + i * 32 + k), (uint16_t)(7200 + i), PEER_ISN);
            tp_send(&f, TP_SYN, NULL, 0);
            tp_forget();
        }
    }
    struct stack_tcp_listen *spare;
    uint16_t port;
    CHECK_ST(stack_tcp_listen(0, 1, SOCKRING_MIN, NULL, &spare, &port), ERR_NO_RESOURCES);
    uint32_t half = fix_counts().half_open;
    CHECK_EQ(half, STACK_TCP_LISTENERS * (STACK_TCP_BACKLOG - 1));
    /* Until full: no TIME_WAIT left to recycle, every pcb in use. */
    for (;;) {
        struct stack_tcp_counts t = fix_counts();
        if (!t.time_wait && t.live + t.half_open == STACK_TCP_CONNS + 128)
            break;
        CHECK(nc < STACK_TCP_CONNS - 1);
        CHECK(pool_connect(&c[nc++]));
    }
    CHECK_EQ(fix_counts().half_open, half);   /* nothing recycled yet */
    uint32_t none = fix_counts().pcbs_none;
    struct tp f = peer_at(22000, 7200, PEER_ISN);   /* listener 0 has a backlog slot left */
    tp_send(&f, TP_SYN, NULL, 0);
    CHECK_EQ(tp_caught(), 0);
    CHECK(fix_counts().pcbs_none > none);
    CHECK(pool_connect(&c[nc++]));
    CHECK_EQ(fix_counts().half_open, half - 1);
    printf("utest: nettcp: lwIP's pool full: %u half-open, %u connecting; a SYN then had no pcb, "
           "a connect did\n", half, nc - 1);
    for (unsigned i = 0; i < nc; i++)
        ntcp_conn_free(c[i]);
    for (unsigned i = 0; i < STACK_TCP_LISTENERS; i++)
        ntcp_unlisten(l[i]);
    tp_forget();
    CHECK(fix_down());
    return true;
}

/* ---- malformed segments -------------------------------------------------------------- */

enum spoil {
    SP_CHECKSUM, SP_HDR_SHORT, SP_HDR_LONG, SP_TRUNCATED, SP_SYN_FIN, SP_ACK_FUTURE,
    SP_ACK_OLD, SP_SEQ_OLD, SP_SEQ_FUTURE, SP_NO_ACK, SP_COUNT
};

static const char *const spoil_names[SP_COUNT] = {
    "bad TCP checksum", "data offset 4", "data offset past the segment",
    "TCP header cut short", "SYN and FIN", "ACK of bytes never sent", "ACK from long ago",
    "an old sequence number", "a sequence number past the window", "no ACK flag",
};

/* A data segment of 10 bytes from x's peer, spoiled one way; its length. */
static size_t spoiled(uint8_t *f, const struct cx *x, enum spoil how)
{
    struct tp p = x->p;
    uint8_t flags = TP_ACK | TP_PSH;
    if (how == SP_ACK_FUTURE)
        p.rcv += 100000;
    if (how == SP_ACK_OLD)
        p.rcv -= 200000;
    if (how == SP_SEQ_OLD)
        p.snd -= 50000;
    if (how == SP_SEQ_FUTURE)
        p.snd += 100000;
    if (how == SP_SYN_FIN)
        flags = TP_SYN | TP_FIN | TP_ACK;
    if (how == SP_NO_ACK)
        flags = TP_PSH;   /* RFC 9293 3.10.7.4: "if the ACK bit is off, drop the segment" */
    size_t n = tp_frame(f, &p, flags, "malformed!", 10);
    uint8_t *t = f + 34;
    switch (how) {
    case SP_CHECKSUM:  t[25] ^= 0x40; break;
    case SP_HDR_SHORT: t[12] = 4 << 4; tp_refix(f, n); break;
    case SP_HDR_LONG:  t[12] = 15 << 4; tp_refix(f, n); break;
    case SP_TRUNCATED:
        pkt_ipv4(f, 6, PEER_IP, OUR_IP, 10);
        return 34 + 10;
    default:           break;
    }
    return n;
}

static bool only_acks(struct cx *x, enum spoil how)
{
    static struct tp_seg s[8];
    unsigned n;
    CHECK(fix_turn(x, s, 8, &n, false));
    for (unsigned i = 0; i < n; i++)
        if (s[i].len || s[i].flags != TP_ACK)
            FAIL("a %s segment was answered with flags %x, %zu bytes", spoil_names[how],
                 s[i].flags, s[i].len);
    if (sockring_ready(&x->prog.rx))
        FAIL("a %s segment's bytes reached the ring", spoil_names[how]);
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_OPEN);
    return true;
}

bool t_nettcp_malformed(void)
{
    static uint8_t f[PKT_FRAME_MAX], got[16];
    struct cx x;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8091, 8192, 8192));
    struct stack_tcp_counts before = fix_counts();
    for (unsigned i = 0; i < SP_COUNT; i++) {
        tp_input(f, spoiled(f, &x, i));
        CHECK(only_acks(&x, i));
    }
    struct stack_tcp_counts after = fix_counts();
    CHECK(after.bad_checksums > before.bad_checksums);
    CHECK(after.dropped >= before.dropped + 3);
    CHECK_EQ(after.bad_acks, before.bad_acks + 2);   /* never sent, long ago: before lwIP */
    CHECK_EQ(after.no_acks, before.no_acks + 1);
    /* SYNs with broken options to a listener: answered or not, nothing
     * breaks. */
    struct ntcp_listener *l;
    struct ntcp_listen_req req = { .port = 7300, .backlog = 4, .rx_size = 8192,
                                   .rings = fix_lst_rings, .drop = fix_lst_drop };
    CHECK_ST(ntcp_listen(&req, &l), OK);
    static const uint8_t opts[][4] = { { 2, 0, 0, 0 }, { 2, 255, 5, 180 }, { 8, 1, 1, 1 } };
    for (unsigned i = 0; i < 3; i++) {
        struct tp p = peer_at((uint16_t)(23000 + i), 7300, PEER_ISN);
        size_t n = tp_frame(f, &p, TP_SYN, NULL, 0);
        memcpy(f + 34 + 20, opts[i], 4);
        tp_refix(f, n);
        tp_input(f, n);
    }
    ntcp_unlisten(l);
    tp_forget();
    /* Still a working connection. */
    tp_send(&x.p, TP_ACK | TP_PSH, "fine", 4);
    CHECK_EQ(sockring_stream_read(&x.prog.rx, got, sizeof(got)), 4);
    CHECK(!memcmp(got, "fine", 4));
    fix_publish(&x, &x.prog.rx);
    fix_free(&x);
    tp_forget();
    CHECK(fix_down());
    return true;
}

static uint32_t xorshift(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* One random segment from x's peer, near what the connection expects,
 * with a few bytes changed (its checksum fixed again half the time). */
static void fuzz_one(struct cx *x, uint32_t *seed, uint8_t *f)
{
    x->p.snd = x->acked;   /* where netstack's receive stands, from its last ACK */
    static const uint8_t flag_sets[] = { TP_ACK, TP_ACK | TP_PSH, TP_ACK | TP_FIN, TP_RST,
                                         TP_ACK | TP_RST, TP_SYN, TP_SYN | TP_ACK, 0 };
    uint32_t r = xorshift(seed);
    struct tp p = x->p;
    if (r & 3)   /* a quarter right on the expected number: a RST there ends it */
        p.snd += (r & 0xffff) - 0x8000;
    if (r & 4)   /* half acking just what netstack sent */
        p.rcv += (r >> 16 & 0xff) - 0x80;
    uint8_t data[200];
    size_t len = xorshift(seed) % sizeof(data);
    tp_fill(r, 0, data, len);
    size_t n = tp_frame(f, &p, flag_sets[r >> 24 & 7], data, len);
    for (unsigned k = 0, flips = xorshift(seed) % 4; k < flips; k++)
        f[34 + xorshift(seed) % (n - 34)] ^= (uint8_t)(1u << (xorshift(seed) & 7));
    if (r & (1u << 31))
        tp_refix(f, n);
    tp_input(f, n);
}

/* Thousands of mutated segments at live connections (a new one each time
 * one dies). Whatever lwIP makes of them: no crash, and once the address
 * is gone nothing is left in lwIP's pools, heap or receive buffers. */
bool t_nettcp_fuzz(void)
{
    static uint8_t f[PKT_FRAME_MAX], sink[8192];
    struct cx x;
    uint32_t seed = 0x74637021;
    unsigned conns = 1;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8092, 8192, 8192));
    for (unsigned i = 0; i < 4000; i++) {
        fuzz_one(&x, &seed, f);
        if (i % 16)
            continue;
        static struct tp_seg s[TP_CAP];
        unsigned n;
        CHECK(fix_turn(&x, s, TP_CAP, &n, true));
        tp_forget();
        if (sockring_stream_read(&x.prog.rx, sink, sizeof(sink)))
            fix_publish(&x, &x.prog.rx);
        if (fix_status(&x).state != SOCKRING_STATE_CLOSED)
            continue;
        fix_free(&x);
        CHECK(fix_connect(&x, (uint16_t)(8093 + conns++ % 100), 8192, 8192));
    }
    ntcp_conn_abort(x.c, ERR_CANCELED);
    fix_free(&x);
    tp_forget();
    CHECK(conns > 1);   /* some mutations did end connections */
    struct stack_tcp_counts t = fix_counts();
    printf("utest: nettcp: fuzz: 4000 segments, %u connections, lwIP dropped %u in all, "
           "%u bad checksums, %u bad ACKs\n", conns, t.dropped, t.bad_checksums, t.bad_acks);
    CHECK(fix_down());
    return true;
}

/* ---- hostile rings ------------------------------------------------------------------- */

/* x's connection is reset for its ring: a RST on the wire, CLOSED with
 * ERR_OUT_OF_RANGE, the ring error counted. */
static bool reset_for_ring(struct cx *x)
{
    static struct tp_seg s[8];
    unsigned n;
    ntcp_kick(x->c);
    CHECK(fix_turn(x, s, 8, &n, true));
    CHECK(n >= 1);
    CHECK(s[n - 1].flags & TP_RST);
    struct sockring_status st = fix_status(x);
    CHECK_EQ(st.state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(st.error, ERR_OUT_OF_RANGE);
    CHECK(st.ring_errors >= 1);
    fix_free(x);
    return true;
}

bool t_nettcp_hostile_ring(void)
{
    struct cx x;
    CHECK(fix_up());
    /* A tx count far past the ring. */
    CHECK(fix_connect(&x, 8200, 8192, 8192));
    __atomic_store_n(&x.prog.page->tx_prod.count, 1ull << 40, __ATOMIC_RELEASE);
    CHECK(reset_for_ring(&x));
    /* An rx count past what was put there. */
    CHECK(fix_connect(&x, 8201, 8192, 8192));
    tp_send(&x.p, TP_ACK | TP_PSH, "some bytes", 10);
    __atomic_store_n(&x.prog.page->rx_cons.count, 1ull << 30, __ATOMIC_RELEASE);
    CHECK(reset_for_ring(&x));
    /* Flags nobody defined on the tx line. */
    CHECK(fix_connect(&x, 8202, 8192, 8192));
    __atomic_store_n(&x.prog.page->tx_prod.flags, 0x80000000u, __ATOMIC_RELEASE);
    CHECK(reset_for_ring(&x));
    /* Bytes after its own END: ignored (its FIN went, nothing follows). */
    static struct tp_seg s[8];
    unsigned n;
    CHECK(fix_connect(&x, 8203, 8192, 8192));
    __atomic_store_n(&x.prog.page->tx_prod.flags, SOCKRING_END, __ATOMIC_RELEASE);
    ntcp_kick(x.c);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_FIN | TP_ACK);
    __atomic_store_n(&x.prog.page->tx_prod.count, 100, __ATOMIC_RELEASE);
    ntcp_kick(x.c);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(n, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_OPEN);
    ntcp_conn_abort(x.c, ERR_CANCELED);
    fix_free(&x);
    tp_forget();
    CHECK(fix_down());
    return true;
}
