/* nettest rx: what reaches netstack (nettest.h). The host's peer answers
 * "nettest-go 1" with the census (RX_*) and "nettest-go 2" with the flood;
 * tools/net-test.sh builds the same frames. */
#define CHECK_PROG "nettest"
#define CHECK_CUR  cur
#include <check.h>
#include "nettest.h"

#define RX_WAIT     (5 * NS_PER_S)
#define FLOOD_WAIT  (2 * NS_PER_S)

/* The census's VLAN 21 frames: frame seq is this long once untagged. */
static const uint32_t good_len[RX_GOOD] = { 60, 64, 100, 200, 512, 1000, 1400, 1500, 1513, 1514 };

/* The number after "<tag> " at f[14..len), or -1 if the text isn't there. */
static int64_t seq_of(const uint8_t *f, uint32_t len, const char *tag)
{
    uint32_t i = 14;
    for (; *tag; tag++, i++)
        if (i >= len || f[i] != (uint8_t)*tag)
            return -1;
    if (i >= len || f[i++] != ' ')
        return -1;
    int64_t v = 0;
    uint32_t digits = 0;
    for (; i < len && f[i] >= '0' && f[i] <= '9' && digits < 9; i++, digits++)
        v = v * 10 + (f[i] - '0');
    return digits && i < len && f[i] == 0 ? v : -1;
}

/* Ask the peer for a burst: "nettest-go <n>", tagged by the driver. */
static void go(struct sess *s, uint32_t n)
{
    uint8_t f[64];
    frame_make(f, sizeof(f), "nettest-go", n);
    netdev_put(&s->txe, f, sizeof(f));
    tx_kick(s);
}

/* The next received frame into f (*len), waiting on to_stack as netstack
 * would; false at the deadline. */
static bool next_frame(struct sess *s, uint8_t *f, uint32_t *len, uint64_t deadline)
{
    while (netdev_ready(&s->rxe) == 0) {
        if (now() > deadline)
            return false;
        (void)jam_event_signal(s->to_stack, NETDEV_SIG_RX, 0);   /* before looking */
        signals_t seen = 0;
        if (netdev_sleep(&s->rxe))
            (void)jam_object_wait_one(s->to_stack, NETDEV_SIG_RX, deadline, &seen);
        netdev_awake(&s->rxe);
    }
    status_t st = netdev_take(&s->rxe, f, NETDEV_FRAME_MAX, len);
    (void)netdev_publish(&s->rxe);
    return st == OK;
}

/* One census frame: ours, untagged, its sequence number and length right. */
static bool census_frame(const uint8_t *f, uint32_t len, uint32_t *seen)
{
    int64_t seq = seq_of(f, len, "nettest-rx");
    CHECK(f[12] == (uint8_t)(NT_ETHERTYPE >> 8) && f[13] == (uint8_t)NT_ETHERTYPE);
    CHECK(seq >= 0 && seq < (int64_t)RX_GOOD);
    CHECK(!(*seen & (1u << seq)));
    CHECK_EQ(len, good_len[seq]);
    *seen |= 1u << seq;
    return true;
}

bool t_rx_census(void)
{
    struct sess s;
    struct netdev_stats a, b;
    CHECK(wait_link());
    CHECK_ST(sess_open(&s), OK);
    CHECK(get_stats(nic, &a));
    go(&s, 1);
    uint8_t f[NETDEV_FRAME_MAX];
    uint32_t len = 0, seen = 0, n = 0;
    uint64_t end = now() + RX_WAIT;
    bool ok = true;
    while (ok && n < RX_GOOD && next_frame(&s, f, &len, end)) {
        ok = census_frame(f, len, &seen);
        n++;
    }
    (void)jam_nanosleep(now() + 200 * NS_PER_MS);   /* anything more would have come by now */
    uint32_t extra = netdev_ready(&s.rxe);
    sess_close(&s);
    CHECK(ok);
    CHECK(get_stats(nic, &b));
    printf("nettest: rx: %u of %u VLAN 21 frames; dropped: %lu untagged, %lu vlan 0, %lu other "
           "vlans or tags, %lu bad\n", n, RX_GOOD, (unsigned long)(b.rx_untagged - a.rx_untagged),
           (unsigned long)(b.rx_priority - a.rx_priority),
           (unsigned long)(b.rx_other_vlan - a.rx_other_vlan),
           (unsigned long)(b.rx_bad - a.rx_bad));
    CHECK_EQ(n, RX_GOOD);
    CHECK_EQ(extra, 0);
    CHECK_EQ(b.rx_frames - a.rx_frames, RX_GOOD);
    CHECK_EQ(b.rx_untagged - a.rx_untagged, RX_UNTAGGED);
    CHECK_EQ(b.rx_priority - a.rx_priority, RX_PRIORITY);
    CHECK_EQ(b.rx_other_vlan - a.rx_other_vlan, RX_OTHER);
    CHECK_EQ(b.rx_bad - a.rx_bad, 0);   /* the RX_LONG frame: the chip's to drop */
    CHECK_EQ(b.rx_ring_full - a.rx_ring_full, 0);
    return true;
}

bool t_rx_flood(void)
{
    struct sess s;
    struct netdev_stats a, b;
    CHECK_ST(sess_open(&s), OK);
    CHECK(get_stats(nic, &a));
    go(&s, 2);
    /* netstack stalls: the ring fills, the driver drops and counts, and
     * still answers. */
    (void)jam_nanosleep(now() + FLOOD_WAIT);
    CHECK(get_stats(s.ch, &b));
    printf("nettest: flood: %lu given, %lu dropped for a full ring, %lu missed by the chip\n",
           (unsigned long)(b.rx_frames - a.rx_frames),
           (unsigned long)(b.rx_ring_full - a.rx_ring_full),
           (unsigned long)(b.chip_rx_missed - a.chip_rx_missed));
    CHECK_EQ(b.rx_frames - a.rx_frames, NETDEV_SLOTS);
    CHECK_EQ(b.rx_ring_full - a.rx_ring_full, RX_FLOOD - NETDEV_SLOTS);
    uint8_t f[NETDEV_FRAME_MAX];
    uint32_t len = 0;
    for (uint32_t i = 0; i < NETDEV_SLOTS; i++) {
        CHECK(next_frame(&s, f, &len, now() + NS_PER_S));
        CHECK_EQ(seq_of(f, len, "nettest-flood"), i);
    }
    CHECK_EQ(netdev_ready(&s.rxe), 0);
    sess_close(&s);
    return true;
}
