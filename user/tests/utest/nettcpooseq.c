/* utest: netstack's TCP keeping the segments that come past a hole
 * (lwIP's out-of-order queue, lwipopts.h TCP_QUEUE_OOSEQ) and saying so
 * with SACK blocks (RFC 2018), in-process on nettcp.h's fixture (the test
 * is the peer, on the wire, and the program, on the rings):
 *
 *   nettcp_ooseq_sack    segments past a hole are kept, none sent twice:
 *                        each one's ACK holds the hole's sequence number
 *                        and SACK blocks for the kept ranges, newest first;
 *                        with a hole filled the ACK jumps past all that was
 *                        kept and the ring has every byte in order; past a
 *                        hole, a segment without an ACK flag or acking bytes
 *                        never sent is still dropped before lwIP, not kept;
 *                        a peer that didn't offer SACK gets the same queue
 *                        and no blocks; a listener's SYN-ACK offers SACK
 *                        only to a SYN that did
 *   nettcp_ooseq_bounds  hostile peers whose hole never fills: a connection
 *                        keeps at most its window in segments, tiny ones
 *                        too; a bulk one at most its share, then the others
 *                        their first STACK_OOSEQ_FIRST each until the queues
 *                        hold STACK_OOSEQ_MAX together and the last
 *                        STACK_RX_KEEP buffers stay free; an ordinary
 *                        connection still receives in order; filled, the bulk
 *                        one's kept bytes come out exact; a connection let
 *                        go of gives its buffers back at once and keeps no
 *                        more */
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

#define SEG        STACK_TCP_MSS
#define PEER_SHIFT 7u               /* the peers' window scale */
#define BULK       (2u << 20)       /* a bulk rx ring: a whole scaled window */
#define BULK_CONNS 8u               /* more than the shares hold */

/* The peer p sends segment i of stream id (its bytes i * SEG on) at
 * sequence number base + i * SEG: in order or not, p is not moved on. */
static void seg_at(const struct tp *p, uint32_t base, uint32_t id, unsigned i, uint8_t flags)
{
    static uint8_t buf[SEG];
    struct tp q = *p;
    q.snd = base + i * SEG;
    tp_fill(id, (uint64_t)i * SEG, buf, SEG);
    tp_send(&q, flags, buf, SEG);
}

/* netstack's turn, and the last segment it sent x's peer: a bare ACK. One
 * without SACK blocks lwIP may delay: its TCP timer sends it then (within
 * 250 ms; another of lwIP's timers may come first). */
static bool next_ack(struct cx *x, struct tp_seg *a)
{
    static struct tp_seg s[TP_CAP];
    unsigned n;
    CHECK(fix_turn(x, s, TP_CAP, &n, false));
    for (unsigned k = 0; !n && k < 4; k++) {
        fix_tick();
        CHECK(fix_turn(x, s, TP_CAP, &n, false));
    }
    CHECK(n >= 1);
    *a = s[n - 1];
    CHECK_EQ(a->flags, TP_ACK);
    CHECK_EQ(a->len, 0);
    return true;
}

/* a acks segments before `upto` (from base) and carries exactly n SACK
 * blocks, block i the segments [blocks[i][0], blocks[i][1]). */
static bool acks(const struct tp_seg *a, uint32_t base, unsigned upto,
                 const unsigned (*blocks)[2], unsigned n)
{
    CHECK_EQ(a->ack, base + upto * SEG);
    CHECK_EQ(a->nsack, n);
    for (unsigned i = 0; i < n; i++) {
        CHECK_EQ(a->sack[i][0], base + blocks[i][0] * SEG);
        CHECK_EQ(a->sack[i][1], base + blocks[i][1] * SEG);
    }
    return true;
}

/* lwIP's receive buffers in use now. */
static uint32_t held(void)
{
    struct stack_counts c;
    stack_get_counts(&c);
    return c.rx_buffers_used;
}

/* The program reads segs segments of stream id from x's rx ring, from
 * segment `from` on, every byte checked, and gives them back. */
static bool read_segs(struct cx *x, uint32_t id, unsigned from, unsigned segs)
{
    static uint8_t buf[SEG];
    CHECK_EQ(sockring_ready(&x->prog.rx), segs * SEG);
    for (unsigned i = from; i < from + segs; i++) {
        CHECK_EQ(sockring_stream_read(&x->prog.rx, buf, SEG), SEG);
        if (!tp_same(id, (uint64_t)i * SEG, buf, SEG))
            FAIL("segment %u's bytes are not the stream's", i);
    }
    fix_publish(x, &x->prog.rx);
    return true;
}

/* ---- kept, and said so ------------------------------------------------------------ */

/* Past the hole, the checks netstack makes before lwIP still come first:
 * a segment with no ACK flag, and one acking bytes never sent, are
 * dropped and counted, not kept. */
static bool checks_first(struct cx *x, uint32_t base, unsigned at)
{
    struct stack_tcp_counts t = fix_counts();
    uint32_t was = held();
    seg_at(&x->p, base, 40, at, TP_PSH);
    struct tp lie = x->p;
    lie.rcv += 100000;
    seg_at(&lie, base, 40, at, TP_ACK | TP_PSH);
    CHECK_EQ(fix_counts().no_acks, t.no_acks + 1);
    CHECK_EQ(fix_counts().bad_acks, t.bad_acks + 1);
    CHECK_EQ(held(), was);
    tp_forget();
    return true;
}

static bool sack_kept(void)
{
    struct cx x;
    struct fix_big b = { 0 };
    struct tp_seg a;
    CHECK(fix_open_big(&x, &b, &(struct tp){ .port = 8300, .wscale = PEER_SHIFT + 1, .sack = true },
                       4096, 256 * 1024));
    uint32_t base = x.p.snd;
    seg_at(&x.p, base, 40, 0, TP_ACK | TP_PSH);
    for (unsigned i = 2; i < 10; i++) {   /* a hole at 1 */
        seg_at(&x.p, base, 40, i, TP_ACK | TP_PSH);
        CHECK(next_ack(&x, &a));
        CHECK(acks(&a, base, 1, (const unsigned[][2]){ { 2, i + 1 } }, 1));
    }
    CHECK_EQ(sockring_ready(&x.prog.rx), SEG);   /* only what came in order */
    CHECK_EQ(held(), 8);
    CHECK_EQ(fix_counts().ooseq_held, 8);
    CHECK(checks_first(&x, base, 10));
    seg_at(&x.p, base, 40, 12, TP_ACK | TP_PSH);   /* a second hole, at 10 and 11 */
    seg_at(&x.p, base, 40, 13, TP_ACK | TP_PSH);
    CHECK(next_ack(&x, &a));
    CHECK(acks(&a, base, 1, (const unsigned[][2]){ { 12, 14 }, { 2, 10 } }, 2));
    seg_at(&x.p, base, 40, 1, TP_ACK | TP_PSH);    /* the first filled: */
    CHECK(next_ack(&x, &a));                       /* past all it kept */
    CHECK(acks(&a, base, 10, (const unsigned[][2]){ { 12, 14 } }, 1));
    CHECK_EQ(held(), 2);
    CHECK(read_segs(&x, 40, 0, 10));
    seg_at(&x.p, base, 40, 10, TP_ACK | TP_PSH);
    CHECK(next_ack(&x, &a));
    CHECK(acks(&a, base, 11, (const unsigned[][2]){ { 12, 14 } }, 1));
    seg_at(&x.p, base, 40, 11, TP_ACK | TP_PSH);   /* the second filled: nothing left to SACK */
    CHECK(next_ack(&x, &a));
    CHECK(acks(&a, base, 14, NULL, 0));
    CHECK_EQ(held(), 0);
    CHECK(read_segs(&x, 40, 10, 4));
    x.p.snd = base + 14 * SEG;
    fix_reset_by_peer(&x);
    fix_free_big(&x, &b);
    return true;
}

/* A peer that didn't offer SACK: the same queue, ACKs without blocks. */
static bool kept_without_sack(void)
{
    struct cx x;
    struct tp_seg a;
    CHECK(fix_connect(&x, 8301, 8192, 16384));
    uint32_t base = x.p.snd;
    for (unsigned i = 1; i < 4; i++) {   /* a hole at 0 */
        seg_at(&x.p, base, 41, i, TP_ACK | TP_PSH);
        CHECK(next_ack(&x, &a));
        CHECK(acks(&a, base, 0, NULL, 0));
    }
    CHECK_EQ(held(), 3);
    seg_at(&x.p, base, 41, 0, TP_ACK | TP_PSH);
    CHECK(next_ack(&x, &a));
    CHECK(acks(&a, base, 4, NULL, 0));
    CHECK_EQ(held(), 0);
    CHECK(read_segs(&x, 41, 0, 4));
    fix_free(&x);
    tp_forget();
    return true;
}

/* A listener's SYN-ACK carries SACK-permitted only for a SYN that did. */
static bool listener_offers(void)
{
    struct ntcp_listener *l;
    struct ntcp_listen_req req = { .port = 7400, .backlog = 4, .rx_size = 16384,
                                   .rings = fix_lst_rings, .drop = fix_lst_drop };
    CHECK_ST(ntcp_listen(&req, &l), OK);
    for (unsigned sack = 0; sack < 2; sack++) {
        struct tp_seg s[4];
        unsigned n;
        struct cx x = { .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = (uint16_t)(24000 + sack),
                               .our = 7400, .snd = PEER_ISN, .win = 0xffff, .sack = sack } };
        tp_send(&x.p, TP_SYN, NULL, 0);
        CHECK(fix_turn(&x, s, 4, &n, false));
        CHECK_EQ(n, 1);
        CHECK_EQ(s[0].flags, TP_SYN | TP_ACK);
        CHECK_EQ(s[0].sack_ok, sack);
    }
    ntcp_unlisten(l);   /* the two half-open ones go with it */
    tp_forget();
    return true;
}

bool t_nettcp_ooseq_sack(void)
{
    CHECK(fix_up());
    bool ok = sack_kept() && kept_without_sack() && listener_offers();
    CHECK(ok);
    CHECK(fix_down());
    return true;
}

/* ---- bounded ------------------------------------------------------------------------ */

/* x's peer sends segments [from, to) of stream id past a hole at segment 0
 * that it never fills (x->p.snd stays where the hole is); netstack's ACKs
 * are forgotten. */
static void flood(struct cx *x, uint32_t id, unsigned from, unsigned to)
{
    for (unsigned i = from; i < to; i++) {
        seg_at(&x->p, x->p.snd, id, i, TP_ACK | TP_PSH);
        if (tp_caught() >= TP_CAP / 2)
            tp_forget();
    }
    tp_forget();
}

/* One byte at every other sequence number past a one-byte hole: 200
 * buffers' worth of segments in a 64240-byte window, which holds 44 full
 * ones. Then let go of: its buffers back at once, and no more kept. */
static bool tiny_segments(void)
{
    static struct tp_seg s[TP_CAP];
    struct cx x;
    unsigned n;
    CHECK(fix_connect(&x, 8310, 8192, 65536));   /* a peer that doesn't scale: 64240 */
    uint32_t base = x.p.snd, cut = fix_counts().ooseq_cut;
    uint32_t cap = (STACK_TCP_WND_PLAIN + SEG - 1) / SEG;   /* its window in full segments */
    for (unsigned i = 0; i < 200; i++) {
        struct tp q = x.p;
        q.snd = base + 1 + 2 * i;
        tp_send(&q, TP_ACK | TP_PSH, "x", 1);
        if (tp_caught() >= TP_CAP / 2)
            tp_forget();
    }
    tp_forget();
    CHECK_EQ(held(), cap);
    CHECK(fix_counts().ooseq_cut > cut);
    struct tp q = x.p;
    tp_send(&q, TP_ACK | TP_PSH, "x", 1);   /* the hole: it and the next one come in order */
    CHECK(fix_turn(&x, s, TP_CAP, &n, false));
    CHECK_EQ(sockring_ready(&x.prog.rx), 2);
    CHECK_EQ(held(), cap - 1);
    uint8_t two[2];
    CHECK_EQ(sockring_stream_read(&x.prog.rx, two, 2), 2);
    fix_publish(&x, &x.prog.rx);
    fix_free(&x);   /* nothing unread: let go of, not reset */
    CHECK_EQ(held(), 0);
    q.snd = base + 3;
    for (unsigned i = 0; i < 4; i++, q.snd += 2)   /* still past the hole: kept no more */
        tp_send(&q, TP_ACK | TP_PSH, "y", 1);
    CHECK_EQ(held(), 0);
    tp_forget();
    return true;
}

/* The queues together: the first bulk connection takes its share, the
 * others their first STACK_OOSEQ_FIRST each, until STACK_OOSEQ_MAX. */
static bool fill_shares(struct cx *bx)
{
    flood(&bx[0], 50, 1, 1000);
    uint32_t want = STACK_OOSEQ_MAX - STACK_OOSEQ_KEEP_BULK;
    CHECK_EQ(held(), want);
    for (unsigned k = 1; k < BULK_CONNS; k++) {
        flood(&bx[k], 50 + k, 1, 100);
        want = want + STACK_OOSEQ_FIRST < STACK_OOSEQ_MAX ? want + STACK_OOSEQ_FIRST
                                                          : STACK_OOSEQ_MAX;
        CHECK_EQ(held(), want);
    }
    CHECK_EQ(want, STACK_OOSEQ_MAX);
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK_EQ(c.rx_buffers_none, 0);   /* never a frame without a buffer */
    CHECK(STACK_RX_BUFS - c.rx_buffers_used >= STACK_RX_KEEP);
    CHECK_EQ(fix_counts().ooseq_held, STACK_OOSEQ_MAX);
    return true;
}

/* The first one's hole filled: what it kept, in order and exact, and the
 * ACK says where its queue was cut. */
static bool bulk_filled(struct cx *x)
{
    struct tp_seg a;
    uint32_t kept = STACK_OOSEQ_MAX - STACK_OOSEQ_KEEP_BULK, base = x->p.snd;
    seg_at(&x->p, base, 50, 0, TP_ACK | TP_PSH);
    CHECK(next_ack(x, &a));
    CHECK_EQ(a.ack, base + (1 + kept) * SEG);
    CHECK(read_segs(x, 50, 0, 1 + kept));
    x->p.snd = base + (1 + kept) * SEG;
    CHECK_EQ(held(), STACK_OOSEQ_MAX - kept);
    return true;
}

static bool bulk_shares(void)
{
    static struct cx bx[BULK_CONNS];
    static struct fix_big bb[BULK_CONNS];
    struct cx small;
    for (unsigned k = 0; k < BULK_CONNS; k++) {
        struct tp peer = { .port = (uint16_t)(8320 + k), .wscale = PEER_SHIFT + 1, .sack = true };
        CHECK(fix_open_big(&bx[k], &bb[k], &peer, 4096, BULK));
    }
    CHECK(fix_connect(&small, 8330, 8192, 16384));
    CHECK(fill_shares(bx));
    CHECK(fix_receive(&small, 42, 60000, 4096));   /* an ordinary connection, in order */
    CHECK(bulk_filled(&bx[0]));
    for (unsigned k = 0; k < BULK_CONNS; k++)   /* nothing unread: let go of */
        fix_free_big(&bx[k], &bb[k]);
    CHECK_EQ(held(), 0);
    flood(&bx[1], 51, 1, 20);
    CHECK_EQ(held(), 0);
    fix_free(&small);
    tp_forget();
    return true;
}

bool t_nettcp_ooseq_bounds(void)
{
    CHECK(fix_up());
    bool ok = tiny_segments() && bulk_shares();
    CHECK(ok);
    CHECK(fix_down());
    return true;
}
