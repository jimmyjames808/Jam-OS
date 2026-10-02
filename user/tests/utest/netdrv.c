/* utest: bin/netstack over a fake network driver. The test plays both
 * devmgr (netstack's device channel: GET_SERVICE) and the driver (the
 * netdev channel: info and open; then the session's rings and events,
 * real VMOs and events with the rights netdev.open hands out), and calls
 * netstack's control channel as init does. Frames go into the rx ring and
 * come out of the tx ring exactly as a NIC driver would see them, and are
 * checked byte by byte (netpkt.h).
 *
 * Covered: netstack finds the card through devmgr, opens a session and
 * answers ARP and a ping through the rings (every frame untagged, 60 bytes
 * at least); the link going down and up (NETDEV_SIG_LINK, netdev.info on
 * the session); a driver restart (the session closed: netstack asks
 * devmgr again, opens a new session, keeps its address); a hostile driver:
 * slots with bad lengths and flags refused and counted, an rx or tx count
 * out of range ends the session (counted) and netstack opens a new one;
 * killed, its job is empty. netsock.c runs its socket tests on this
 * harness (netdrv.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <devmgr.h>
#include <idl/netctl.h>
#include <idl/netdev.h>
#include <jam/netdev.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
#include "utest.h"

#define WAIT     (5 * NS_PER_S)     /* for netstack to do what it must */
#define QUIET    (300 * NS_PER_MS)  /* for netstack to do nothing */

/* The fake devmgr and driver, and netstack under test. */
struct fake {
    handle_t job, proc, ctl;            /* netstack, its job, netctl's client end */
    handle_t net;                       /* /svc/net's shared channel, client end */
    handle_t dev;                       /* our end of netstack's device channel */
    handle_t svc;                       /* our end of the driver channel handed out */
    handle_t session, tx_vmo, rx_vmo, to_driver, to_stack;   /* the session's, ours */
    void    *tx_map, *rx_map;
    struct netdev_end tx, rx;           /* we consume tx and produce rx */
    uint32_t link, changes;             /* what info answers */
};

static struct fake fk;

/* ---- the driver's side ---------------------------------------------------------- */

static status_t f_info(void *ctx, uint8_t out_mac[6], uint16_t *out_vlan, uint16_t *out_mtu,
                       uint32_t *out_link, uint32_t *out_speed, uint32_t *out_changes,
                       uint8_t out_chip[16])
{
    struct fake *f = ctx;
    memcpy(out_mac, pkt_our_mac, 6);
    *out_vlan = 21;
    *out_mtu = NETDEV_MTU;
    *out_link = f->link;
    *out_speed = f->link ? 1000 : 0;
    *out_changes = f->changes;
    memset(out_chip, 0, 16);
    memcpy(out_chip, "FAKE", 4);
    return OK;
}

static status_t f_stats(void *ctx, uint8_t out_counts[256])
{
    (void)ctx;
    memset(out_counts, 0, 256);
    out_counts[0] = 42;   /* rx_frames: netsock.c looks for it */
    return OK;
}

static status_t ring(handle_t *vmo, void **map)
{
    uint64_t va = 0;
    status_t st = jam_vmo_create(NETDEV_RING_BYTES, 0, HANDLE_INVALID, vmo);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), *vmo, 0, NETDEV_RING_BYTES,
                          VMAR_READ | VMAR_WRITE, &va);
    *map = (void *)(uintptr_t)va;
    return st;
}

static status_t f_open(void *ctx, handle_t *out_session, handle_t *out_tx, handle_t *out_rx,
                       handle_t *out_to_driver, handle_t *out_to_stack)
{
    struct fake *f = ctx;
    status_t st = ring(&f->tx_vmo, &f->tx_map);
    if (st == OK)
        st = ring(&f->rx_vmo, &f->rx_map);
    if (st == OK)
        st = jam_event_create(&f->to_driver);
    if (st == OK)
        st = jam_event_create(&f->to_stack);
    if (st == OK)
        st = jam_channel_create(&f->session, out_session);
    if (st == OK)
        st = jam_handle_duplicate(f->tx_vmo, NETDEV_RING_RIGHTS, out_tx);
    if (st == OK)
        st = jam_handle_duplicate(f->rx_vmo, NETDEV_RING_RIGHTS, out_rx);
    if (st == OK)
        st = jam_handle_duplicate(f->to_driver, NETDEV_TO_DRIVER_RIGHTS, out_to_driver);
    if (st == OK)
        st = jam_handle_duplicate(f->to_stack, NETDEV_TO_STACK_RIGHTS, out_to_stack);
    if (st != OK)
        return st;   /* the test fails on it; what was made goes with utest */
    netdev_end_make(&f->tx, f->tx_map, NETDEV_RING_TX, false);
    netdev_end_make(&f->rx, f->rx_map, NETDEV_RING_RX, true);
    return OK;
}

static const struct netdev_ops ops = { .info = f_info, .stats = f_stats, .open = f_open };

/* One netdev request off ch, answered. */
static bool serve(handle_t ch)
{
    signals_t seen;
    CHECK_ST(jam_object_wait_one(ch, SIG_READABLE, now() + WAIT, &seen), OK);
    CHECK_ST(netdev_serve_one(ch, &ops, &fk), OK);
    return true;
}

/* As devmgr: answer netstack's GET_SERVICE with a new driver channel. */
static bool serve_devmgr(void)
{
    signals_t seen;
    struct devmgr_req q;
    uint32_t n = 0, nh = 0;
    CHECK_ST(jam_object_wait_one(fk.dev, SIG_READABLE, now() + WAIT, &seen), OK);
    CHECK_ST(drv_channel_read(fk.dev, &q, sizeof(q), &n, NULL, 0, &nh), OK);
    CHECK_EQ(q.ordinal, DEVMGR_GET_SERVICE);
    handle_t theirs;
    CHECK_ST(jam_channel_create(&fk.svc, &theirs), OK);
    struct devmgr_rep r = { .txid = q.txid, .status = OK };
    CHECK_ST(jam_channel_write(fk.dev, &r, sizeof(r), &theirs, 1), OK);
    return true;
}

/* netstack connects: devmgr, then info and open on the driver channel. */
static bool connected(void)
{
    CHECK(serve_devmgr());
    CHECK(serve(fk.svc));   /* info */
    CHECK(serve(fk.svc));   /* open */
    jam_handle_close(fk.svc);
    fk.svc = HANDLE_INVALID;
    return true;
}

/* The driver ends its session (as one that died): everything closed. */
static void drop_session(void)
{
    handle_t hs[] = { fk.session, fk.tx_vmo, fk.rx_vmo, fk.to_driver, fk.to_stack };
    for (unsigned k = 0; k < 5; k++)
        if (hs[k])
            jam_handle_close(hs[k]);
    for (unsigned k = 0; k < 2; k++)
        if (k ? fk.rx_map : fk.tx_map)
            (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR),
                                 (uint64_t)(uintptr_t)(k ? fk.rx_map : fk.tx_map),
                                 NETDEV_RING_BYTES);   /* the test's own: can't fail */
    fk.session = fk.tx_vmo = fk.rx_vmo = fk.to_driver = fk.to_stack = HANDLE_INVALID;
    fk.tx_map = fk.rx_map = NULL;
}

/* A frame into the rx ring, netstack woken if it sleeps. */
static bool drv_send(const uint8_t *frame, size_t len)
{
    CHECK(netdev_room(&fk.rx) > 0);
    netdev_put(&fk.rx, frame, (uint32_t)len);
    if (netdev_publish(&fk.rx))
        CHECK_ST(jam_event_signal(fk.to_stack, 0, NETDEV_SIG_RX), OK);
    return true;
}

/* The next frame netstack sent, into f (*n bytes), within `wait`. */
static status_t drv_recv(uint8_t *f, uint32_t *n, uint64_t wait)
{
    uint64_t end = now() + wait;
    for (;;) {
        netdev_awake(&fk.tx);
        if (netdev_ready(&fk.tx)) {
            status_t st = netdev_take(&fk.tx, f, NETDEV_FRAME_MAX, n);
            (void)netdev_publish(&fk.tx);   /* netstack never waits for room */
            return st;
        }
        if (!netdev_sleep(&fk.tx))
            continue;
        signals_t seen;
        status_t st = jam_object_wait_one(fk.to_driver, NETDEV_SIG_TX, end, &seen);
        if (st != OK)
            return st;
        (void)jam_event_signal(fk.to_driver, NETDEV_SIG_TX, 0);   /* ours: can't fail */
    }
}

/* The gratuitous ARP netstack sends when it has an address and its link
 * comes up (a new address, a new session, the link back). */
static bool announced(void)
{
    static uint8_t f[NETDEV_FRAME_MAX];
    uint32_t n;
    CHECK_ST(drv_recv(f, &n, WAIT), OK);
    CHECK(pkt_frame_ok(f, n));
    CHECK(!memcmp(f, pkt_bcast, 6));
    CHECK_EQ(pkt_get16(f + 12), ETH_ARP);
    CHECK_EQ(pkt_get32(f + 28), OUR_IP);
    CHECK_EQ(pkt_get32(f + 38), OUR_IP);
    return true;
}

/* netstack ended the session itself (it found the rings broken): its end
 * closes; ours go too, and it connects anew. */
static bool ended_by_netstack(void)
{
    signals_t seen;
    CHECK_ST(jam_object_wait_one(fk.session, SIG_PEER_CLOSED, now() + WAIT, &seen), OK);
    drop_session();
    CHECK(connected());
    CHECK(announced());
    return true;
}

/* ---- netstack under test ---------------------------------------------------------- */

static bool start(void)
{
    handle_t ctl_srv, dev_cli, net_srv;
    fk = (struct fake){ .link = NETDEV_LINK_UP | NETDEV_LINK_FULL, .changes = 1 };
    CHECK_ST(new_job(&fk.job), OK);
    CHECK_ST(jam_channel_create(&fk.ctl, &ctl_srv), OK);
    CHECK_ST(jam_channel_create(&fk.dev, &dev_cli), OK);
    CHECK_ST(jam_channel_create(&fk.net, &net_srv), OK);
    const char *argv[] = { "bin/netstack" };
    struct spawn_handle x[] = { { SR_USER + 0, ctl_srv }, { SR_DEVMGR_DEVICE, dev_cli },
                                { SR_USER + 1, net_srv } };
    struct spawn_args a = {
        .path = "bin/netstack", .argc = 1, .argv = argv, .job = fk.job, .extra = x, .nextra = 3,
    };
    CHECK_ST(spawn(&a, &fk.proc), OK);   /* consumes ctl_srv, dev_cli and net_srv */
    CHECK(connected());
    CHECK_ST(netctl_set_ipv4(fk.ctl, OUR_IP, MASK24, GW_IP), OK);
    CHECK(announced());
    return true;
}

static bool stop(void)
{
    struct process_info pi;
    CHECK_ST(jam_process_kill(fk.proc), OK);
    CHECK_ST(spawn_wait(fk.proc, 10 * NS_PER_S, &pi), OK);
    drop_session();
    jam_handle_close(fk.dev);
    jam_handle_close(fk.ctl);
    jam_handle_close(fk.net);
    jam_handle_close(fk.proc);
    struct job_info ji;
    for (uint64_t end = now() + WAIT;;) {   /* a killed process's pages: bounded */
        CHECK_ST(info_of(fk.job, &ji), OK);
        unsigned k = 1;
        while (k < JOB_LIMIT_COUNT && !ji.used[k])
            k++;
        if (k == JOB_LIMIT_COUNT)
            break;
        if (now() > end)
            FAIL("netstack's job still has %lu units of kind %u", (unsigned long)ji.used[k], k);
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    jam_handle_close(fk.job);
    return true;
}

/* A ping through the rings answered (after an ARP request for us, if
 * `arp`). */
static bool ping(uint32_t seq, bool arp)
{
    static uint8_t f[NETDEV_FRAME_MAX], req[NETDEV_FRAME_MAX];
    uint32_t n;
    if (arp) {
        CHECK(drv_send(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP)));
        CHECK_ST(drv_recv(f, &n, WAIT), OK);
        CHECK(pkt_is_arp_reply(f, n, pkt_peer_mac, PEER_IP));
    }
    size_t len = pkt_echo(req, pkt_peer_mac, PEER_IP, OUR_IP, seq, 56);
    CHECK(drv_send(req, len));
    CHECK_ST(drv_recv(f, &n, WAIT), OK);
    CHECK(pkt_is_echo_reply(f, n, req, pkt_peer_mac, PEER_IP, 56));
    return true;
}

/* netctl.device's sessions and counts. */
static bool device(uint32_t *sessions, uint64_t *ring_errors, uint64_t *rx_bad)
{
    uint8_t session, chip[16];
    uint16_t vlan;
    uint32_t speed;
    uint64_t tx_full;
    CHECK_ST(netctl_device(fk.ctl, &session, &vlan, &speed, sessions, ring_errors, rx_bad,
                           &tx_full, chip), OK);
    CHECK_EQ(vlan, 21);
    CHECK(!memcmp(chip, "FAKE", 5));
    return true;
}

bool t_netdrv_ping_and_link(void)
{
    static uint8_t f[NETDEV_FRAME_MAX];
    uint32_t n, sessions;
    uint64_t errs, bad;
    CHECK(start());
    CHECK(ping(1, true));
    CHECK(device(&sessions, &errs, &bad));
    CHECK_EQ(sessions, 1);
    /* The link goes down: netstack asks the driver (info on the session)
     * and sends nothing; up again, it answers again. */
    fk.link = 0;
    fk.changes++;
    CHECK_ST(jam_event_signal(fk.to_stack, 0, NETDEV_SIG_LINK), OK);
    CHECK(serve(fk.session));
    CHECK(drv_send(f, pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, 2, 8)));
    CHECK_ST(drv_recv(f, &n, QUIET), ERR_TIMED_OUT);
    fk.link = NETDEV_LINK_UP;
    fk.changes++;
    CHECK_ST(jam_event_signal(fk.to_stack, 0, NETDEV_SIG_LINK), OK);
    CHECK(serve(fk.session));
    CHECK(announced());
    CHECK(ping(3, false));
    CHECK(stop());
    return true;
}

bool t_netdrv_restart(void)
{
    uint32_t sessions;
    uint64_t errs, bad;
    CHECK(start());
    CHECK(ping(1, true));
    drop_session();          /* the driver died */
    CHECK(connected());      /* netstack asks devmgr again and opens anew */
    CHECK(announced());
    CHECK(ping(2, false));   /* the address and the peer's ARP entry kept */
    CHECK(device(&sessions, &errs, &bad));
    CHECK_EQ(sessions, 2);
    uint32_t addr, mask, gw, d1, d2;
    uint8_t mac[6], dev, link;
    CHECK_ST(netctl_info(fk.ctl, &addr, &mask, &gw, &d1, &d2, mac, &dev, &link), OK);
    CHECK_EQ(addr, OUR_IP);
    CHECK(dev && link);
    CHECK(stop());
    return true;
}

/* A slot the driver wrote badly: refused and counted, nothing answered. */
static bool bad_slot(uint32_t len, uint32_t flags)
{
    static uint8_t f[NETDEV_FRAME_MAX];
    struct netdev_slot *s = netdev_slot_of(&fk.rx, fk.rx.count);
    netdev_put(&fk.rx, f, (uint32_t)pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, 9, 8));
    s->len = len;
    s->flags = flags;
    if (netdev_publish(&fk.rx))
        CHECK_ST(jam_event_signal(fk.to_stack, 0, NETDEV_SIG_RX), OK);
    uint32_t n;
    CHECK_ST(drv_recv(f, &n, QUIET), ERR_TIMED_OUT);
    return true;
}

bool t_netdrv_hostile_driver(void)
{
    static uint8_t f[NETDEV_FRAME_MAX];
    uint32_t sessions;
    uint64_t errs, bad;
    CHECK(start());
    CHECK(ping(1, true));
    CHECK(bad_slot(0, 0));
    CHECK(bad_slot(NETDEV_FRAME_MAX + 1, 0));
    CHECK(bad_slot(13, 0));
    CHECK(bad_slot(60, 1));
    CHECK(device(&sessions, &errs, &bad));
    CHECK_EQ(bad, 4);
    CHECK(ping(2, false));   /* still answering */
    /* rx `produced` far ahead: the session is ended, a new one opened. */
    __atomic_store_n(&fk.rx.hdr->produced, fk.rx.count + 5 * NETDEV_SLOTS, __ATOMIC_RELEASE);
    CHECK_ST(jam_event_signal(fk.to_stack, 0, NETDEV_SIG_RX), OK);
    CHECK(ended_by_netstack());
    CHECK(ping(3, false));
    /* tx `consumed` ahead of what netstack produced: the same. */
    __atomic_store_n(&fk.tx.hdr->consumed, fk.tx.count + 7, __ATOMIC_RELEASE);
    CHECK(drv_send(f, pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, 4, 8)));
    CHECK(ended_by_netstack());
    CHECK(ping(5, false));
    CHECK(device(&sessions, &errs, &bad));
    CHECK_EQ(sessions, 3);
    CHECK(errs >= 2);
    CHECK(stop());
    return true;
}

/* ---- for netsock.c (netdrv.h) ------------------------------------------------------ */

bool netdrv_start(void)
{
    return start();
}

bool netdrv_stop(void)
{
    return stop();
}

bool netdrv_send(const uint8_t *frame, size_t len)
{
    return drv_send(frame, len);
}

status_t netdrv_recv(uint8_t *f, uint32_t *n, uint64_t wait)
{
    return drv_recv(f, n, wait);
}

bool netdrv_ping(uint32_t seq, bool arp)
{
    return ping(seq, arp);
}

bool netdrv_serve_session(void)
{
    return serve(fk.session);
}

handle_t netdrv_ctl(void)
{
    return fk.ctl;
}

handle_t netdrv_net(void)
{
    return fk.net;
}
