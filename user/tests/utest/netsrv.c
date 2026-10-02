/* utest: the network drivers' netdev server (drivers/lib/netserver.c,
 * <jam/netserver.h>), linked in as it is, over a fake card and the test
 * as netstack: info and stats; one session at a time (a second open refused, open refused
 * on the session channel, a closed or orphaned session replaced); the
 * rights of what open hands out; netstack's frames to the card's send
 * function exactly (bad lengths, flags and tags counted, never sent; a
 * card out of descriptors holds the ring until srv_tx_room); received
 * frames into the rx ring with NETDEV_SIG_RX, a full ring or no session
 * dropped and counted; NETDEV_SIG_LINK; hostile ring counts. The server
 * runs in this thread: the test calls its loop's steps (pump). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/driver.h>
#include <jam/netdev.h>
#include <jam/netserver.h>
#include <os.h>
#include "utest.h"

/* ---- the fake card ------------------------------------------------------------------ */

struct card {
    uint32_t room;                /* free descriptors */
    uint32_t sent;                /* frames send took */
    uint8_t  last[NETDEV_FRAME_MAX];
    size_t   last_len;
    uint64_t sum;                 /* of every byte sent, to compare streams */
};

/* As tx_send: a frame whose EtherType is a tag is refused. */
static status_t card_send(void *ctx, const uint8_t *f, size_t len)
{
    struct card *c = ctx;
    if (len >= 14 && f[12] == 0x81 && f[13] == 0x00)
        return ERR_INVALID_ARGS;
    if (!c->room)
        return ERR_NO_RESOURCES;
    c->room--;
    c->sent++;
    memcpy(c->last, f, len);
    c->last_len = len;
    for (size_t i = 0; i < len; i++)
        c->sum += f[i];
    return OK;
}

static uint32_t card_room(void *ctx)
{
    return ((struct card *)ctx)->room;
}

static void card_info(void *ctx, struct srv_info *out)
{
    (void)ctx;
    *out = (struct srv_info){ .mac = { 2, 0, 0, 0, 0, 1 }, .vlan = 21, .link = NETDEV_LINK_UP,
                              .speed = 1000, .changes = 3, .chip = "FAKE" };
}

static void card_stats(void *ctx, struct netdev_stats *s)
{
    s->tx_done += ((struct card *)ctx)->sent;
    s->rx_untagged += 7;
}

static const struct srv_dev card_dev = {
    .send = card_send, .room = card_room, .info = card_info, .stats = card_stats,
};

/* ---- the test's side ------------------------------------------------------------------- */

struct bench {
    handle_t port, cli, serve;    /* the server's port; DR_SERVE's two ends */
    struct card c;
    struct srv v;
    uint32_t txid;
};

/* netstack's view of one session. */
struct side {
    handle_t session, tx, rx, to_driver, to_stack;
    void *txm, *rxm;
    struct netdev_end txe, rxe;
};

/* The server's loop, as the driver's runs it, until nothing is left. */
static void pump(struct bench *b)
{
    for (unsigned i = 0; i < 256; i++) {
        bool busy = srv_work(&b->v);
        struct port_packet p;
        if (jam_port_wait(b->port, 0, &p) == OK) {
            (void)srv_packet(&b->v, &p);
            continue;
        }
        if (!busy)
            return;
    }
}

/* One turn of the driver's loop: the packets noted, one srv_work. */
static void turn(struct bench *b)
{
    struct port_packet p;
    while (jam_port_wait(b->port, 0, &p) == OK)
        (void)srv_packet(&b->v, &p);
    (void)srv_work(&b->v);
}

static bool bench_up(struct bench *b)
{
    *b = (struct bench){ .c.room = 255 };
    CHECK_ST(jam_port_create(&b->port), OK);
    CHECK_ST(jam_channel_create(&b->cli, &b->serve), OK);
    CHECK_ST(srv_init(&b->v, b->port, b->serve, &card_dev, &b->c), OK);
    return true;
}

static void side_close(struct side *s)
{
    if (s->txm)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)s->txm,
                             NETDEV_RING_BYTES);   /* nothing to do if it fails */
    if (s->rxm)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)s->rxm,
                             NETDEV_RING_BYTES);
    handle_t hs[] = { s->session, s->tx, s->rx, s->to_driver, s->to_stack };
    for (unsigned i = 0; i < 5; i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    *s = (struct side){ 0 };
}

static void bench_down(struct bench *b)
{
    srv_end(&b->v);
    jam_handle_close(b->cli);
    jam_handle_close(b->serve);
    jam_handle_close(b->port);
}

/* A reply to the call just sent on ch, after the server's turn. */
static status_t reply(struct bench *b, handle_t ch, void *rep, struct idl_msg *m)
{
    pump(b);
    return idl_reply_read(ch, rep, NETDEV_REP_MAX, m);
}

static status_t do_open(struct bench *b, handle_t ch, struct side *s)
{
    _Alignas(8) uint8_t rep[NETDEV_REP_MAX];
    struct idl_msg m;
    status_t st = netdev_open_send(ch, idl_txid_next(&b->txid));
    if (st == OK)
        st = reply(b, ch, rep, &m);
    if (st == OK)
        st = netdev_open_result(rep, &m, &s->session, &s->tx, &s->rx, &s->to_driver,
                                &s->to_stack);
    return st;
}

static status_t map(handle_t vmo, void **at)
{
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, NETDEV_RING_BYTES,
                               VMAR_READ | VMAR_WRITE, &va);
    *at = (void *)(uintptr_t)va;
    return st;
}

/* open, the rights, the rings mapped and attached as netstack does. */
static bool open_side(struct bench *b, struct side *s)
{
    *s = (struct side){ 0 };
    CHECK_ST(do_open(b, b->cli, s), OK);
    CHECK_ST(jam_vmo_set_size(s->tx, 4096), ERR_ACCESS_DENIED);   /* no resize */
    handle_t dup = 0;
    CHECK_ST(jam_handle_duplicate(s->rx, RIGHT_SAME, &dup), ERR_ACCESS_DENIED);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(s->to_driver, NETDEV_SIG_TX, 0, &seen), ERR_ACCESS_DENIED);
    CHECK_ST(map(s->tx, &s->txm), OK);
    CHECK_ST(map(s->rx, &s->rxm), OK);
    CHECK(netdev_end_attach(&s->txe, s->txm, NETDEV_RING_TX, true));
    CHECK(netdev_end_attach(&s->rxe, s->rxm, NETDEV_RING_RX, false));
    return true;
}

static bool get_stats(struct bench *b, struct netdev_stats *out)
{
    _Alignas(8) uint8_t rep[NETDEV_REP_MAX];
    struct idl_msg m;
    uint8_t counts[NETDEV_STATS_SIZE];
    CHECK_ST(netdev_stats_send(b->cli, idl_txid_next(&b->txid)), OK);
    CHECK_ST(reply(b, b->cli, rep, &m), OK);
    CHECK_ST(netdev_stats_result(rep, &m, counts), OK);
    memcpy(out, counts, sizeof(*out));
    return true;
}

/* A frame of len bytes with EtherType `type`, numbered n. */
static void frame(uint8_t *f, size_t len, uint16_t type, uint32_t n)
{
    for (size_t i = 0; i < len; i++)
        f[i] = (uint8_t)(n * 13 + i);
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
}

/* ---- the tests ------------------------------------------------------------------------------ */

bool t_netserver_session(void)
{
    struct bench b;
    struct side s, s2;
    CHECK(bench_up(&b));
    /* info */
    _Alignas(8) uint8_t rep[NETDEV_REP_MAX];
    struct idl_msg m;
    uint8_t mac[6], chip[16];
    uint16_t vlan = 0, mtu = 0;
    uint32_t link = 0, speed = 0, changes = 0;
    CHECK_ST(netdev_info_send(b.cli, idl_txid_next(&b.txid)), OK);
    CHECK_ST(reply(&b, b.cli, rep, &m), OK);
    CHECK_ST(netdev_info_result(rep, &m, mac, &vlan, &mtu, &link, &speed, &changes, chip), OK);
    CHECK(vlan == 21 && mtu == 1500 && link == NETDEV_LINK_UP && speed == 1000 && changes == 3);
    CHECK(mac[5] == 1 && !memcmp(chip, "FAKE\0", 5));
    /* one session; a second open is refused while it has a client */
    CHECK(open_side(&b, &s));
    CHECK_ST(do_open(&b, b.cli, &s2), ERR_BAD_STATE);
    CHECK_ST(do_open(&b, s.session, &s2), ERR_NOT_SUPPORTED);   /* not on the session */
    /* info works on the session channel too */
    CHECK_ST(netdev_info_send(s.session, idl_txid_next(&b.txid)), OK);
    CHECK_ST(reply(&b, s.session, rep, &m), OK);
    CHECK_ST(netdev_info_result(rep, &m, mac, &vlan, &mtu, &link, &speed, &changes, chip), OK);
    /* the opener goes away: the session ends, and a new open works */
    side_close(&s);
    pump(&b);
    CHECK(!b.v.open);
    CHECK(open_side(&b, &s));
    /* an opener gone without the server noticing yet (its close packet
     * still queued): open finds the session orphaned and replaces it */
    jam_handle_close(s.session);
    s.session = 0;
    CHECK_ST(netdev_open_send(b.cli, idl_txid_next(&b.txid)), OK);
    b.v.serve_ready = true;
    (void)srv_work(&b.v);   /* DR_SERVE first, before any packet is read */
    CHECK(b.v.open && b.v.gen == 3);
    CHECK_ST(idl_reply_read(b.cli, rep, sizeof(rep), &m), OK);
    s2 = (struct side){ 0 };
    CHECK_ST(netdev_open_result(rep, &m, &s2.session, &s2.tx, &s2.rx, &s2.to_driver,
                                &s2.to_stack), OK);
    pump(&b);               /* the old session's close packet: stale, ignored */
    CHECK(b.v.open);
    struct netdev_stats st;
    CHECK(get_stats(&b, &st));
    CHECK_EQ(st.sessions, 3);
    CHECK(st.rx_untagged == 7 && st.reserved[0] == 0);   /* the card's own counts added */
    side_close(&s);
    side_close(&s2);
    bench_down(&b);
    return true;
}

/* netstack's frames: exactly what was written reaches the card, bad
 * slots never do; a card out of descriptors holds the ring. */
bool t_netserver_tx(void)
{
    struct bench b;
    struct side s;
    CHECK(bench_up(&b));
    CHECK(open_side(&b, &s));
    uint8_t f[NETDEV_FRAME_MAX + 16];   /* room for the too-long one */
    /* good, too short, too long, flags set, tagged, good */
    frame(f, 60, 0x0806, 1);
    netdev_put(&s.txe, f, 60);
    frame(f, 60, 0x0800, 2);
    netdev_put(&s.txe, f, 13);
    netdev_put(&s.txe, f, NETDEV_FRAME_MAX + 1);
    netdev_put(&s.txe, f, 60);
    netdev_slot_of(&s.txe, s.txe.count - 1)->flags = 1;
    frame(f, 60, 0x8100, 3);
    netdev_put(&s.txe, f, 60);
    frame(f, NETDEV_FRAME_MAX, 0x0800, 4);
    netdev_put(&s.txe, f, NETDEV_FRAME_MAX);
    (void)netdev_publish(&s.txe);
    CHECK_ST(jam_event_signal(s.to_driver, 0, NETDEV_SIG_TX), OK);
    pump(&b);
    CHECK_EQ(b.c.sent, 2);
    CHECK(b.c.last_len == NETDEV_FRAME_MAX && !memcmp(b.c.last, f, NETDEV_FRAME_MAX));
    struct netdev_stats st;
    CHECK(get_stats(&b, &st));
    CHECK(st.tx_frames == 2 && st.tx_bad_len == 2 && st.tx_bad_flags == 1 && st.tx_bad_tag == 1);
    CHECK_EQ(st.tx_bytes, 64 + NETDEV_FRAME_MAX + 4);   /* the tag counted */
    CHECK_EQ(st.tx_done, 2);
    /* out of descriptors: one goes, the rest wait in the ring */
    b.c.room = 1;
    for (uint32_t i = 0; i < 5; i++) {
        frame(f, 100, 0x0800, 10 + i);
        netdev_put(&s.txe, f, 100);
    }
    (void)netdev_publish(&s.txe);
    CHECK_ST(jam_event_signal(s.to_driver, 0, NETDEV_SIG_TX), OK);
    pump(&b);
    CHECK_EQ(b.c.sent, 3);
    CHECK(b.v.tx_blocked);
    b.c.room = 100;
    srv_tx_room(&b.v);
    pump(&b);
    CHECK_EQ(b.c.sent, 7);
    CHECK(!b.v.tx_blocked);
    /* hostile counts: backwards, then far ahead: never more than a ring */
    __atomic_store_n(&s.txe.hdr->produced, 2, __ATOMIC_RELEASE);
    CHECK_ST(jam_event_signal(s.to_driver, 0, NETDEV_SIG_TX), OK);
    pump(&b);
    CHECK_EQ(b.c.sent, 7);
    /* far ahead: at most one ring's worth per turn of the loop (netstack
     * may keep the driver busy, as heavy traffic would, but never past a
     * bound in one step), and the error counted */
    b.c.room = 100000;
    __atomic_store_n(&s.txe.hdr->produced, UINT64_MAX, __ATOMIC_RELEASE);
    CHECK_ST(jam_event_signal(s.to_driver, 0, NETDEV_SIG_TX), OK);
    turn(&b);
    CHECK(b.c.sent > 7 && b.c.sent <= 7 + NETDEV_SLOTS);
    CHECK(s.txe.hdr->consumed <= 7 + 4 + NETDEV_SLOTS);   /* 4 bad slots taken before */
    CHECK(b.v.s.tx.errors >= 2);   /* backwards, and ahead */
    side_close(&s);
    bench_down(&b);
    return true;
}

/* Received frames: to the ring with a wake, dropped and counted when the
 * ring is full or nobody has a session; the link signal. */
bool t_netserver_rx(void)
{
    struct bench b;
    struct side s;
    CHECK(bench_up(&b));
    uint8_t f[NETDEV_FRAME_MAX], g[NETDEV_FRAME_MAX];
    frame(f, 64, 0x0800, 1);
    srv_rx(&b.v, f, 64);   /* no session yet */
    CHECK(open_side(&b, &s));
    CHECK(netdev_sleep(&s.rxe));   /* netstack waits for frames */
    for (uint32_t i = 0; i < 3; i++) {
        frame(f, 64 + i, 0x0800, i);
        srv_rx(&b.v, f, 64 + i);
    }
    srv_rx(&b.v, f, 13);   /* a length no frame has */
    srv_rx_done(&b.v);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(s.to_stack, NETDEV_SIG_RX, 0, &seen), OK);
    netdev_awake(&s.rxe);
    CHECK_EQ(netdev_ready(&s.rxe), 3);
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t len = 0;
        frame(f, 64 + i, 0x0800, i);
        CHECK_ST(netdev_take(&s.rxe, g, sizeof(g), &len), OK);
        CHECK(len == 64 + i && !memcmp(f, g, len));
    }
    (void)netdev_publish(&s.rxe);
    /* netstack stops reading: one ring's worth fits, the rest is dropped */
    for (uint32_t i = 0; i < NETDEV_SLOTS + 5; i++)
        srv_rx(&b.v, f, 64);
    srv_rx_done(&b.v);
    CHECK_ST(jam_object_signal(s.to_stack, NETDEV_SIG_LINK, 0), OK);
    srv_link(&b.v);
    CHECK_ST(jam_object_wait_one(s.to_stack, NETDEV_SIG_LINK, 0, &seen), OK);
    struct netdev_stats st;
    CHECK(get_stats(&b, &st));
    CHECK(st.rx_frames == 3 + NETDEV_SLOTS && st.rx_ring_full == 5 && st.rx_no_session == 1);
    CHECK(st.rx_bad == 1 && st.link_changes == 1);
    CHECK_EQ(st.rx_bytes, 64 + 65 + 66 + 64ull * NETDEV_SLOTS);
    /* a hostile consumer count: no room, counted, nothing written */
    __atomic_store_n(&s.rxe.hdr->consumed, UINT64_MAX, __ATOMIC_RELEASE);
    srv_rx(&b.v, f, 64);
    CHECK(get_stats(&b, &st));
    CHECK(st.rx_ring_full == 6 && st.ring_errors >= 1);
    side_close(&s);
    bench_down(&b);
    return true;
}
