/* utest: what a network driver may know of a frame (<jam/netframe.h>:
 * classify, tag and check on transmit, keep and untag on receive), over
 * hand-made and hostile frames; and the RTL8125 driver's pure parts: the
 * transmit-register guard and tx.c's gate (notx.h), its arguments
 * (args.h), the send test's ARP frames (arp.h), and the driver left alone
 * without a VLAN or without its hardware. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/driver.h>
#include <jam/netframe.h>
#include <os.h>
#include "args.h"
#include "arp.h"
#include "notx.h"
#include "utest.h"

/* A frame of len bytes whose bytes from 12 on are `tail` (n bytes); the
 * addresses are 0xee, so a classifier reading them would show up. */
static struct netframe_class frame(const uint8_t *tail, size_t n, size_t len)
{
    uint8_t f[64];
    memset(f, 0xee, sizeof(f));
    memcpy(f + 12, tail, n);
    return netframe_classify(f, len);
}

bool t_netframe_classify(void)
{
    static const uint8_t ipv4[] = { 0x08, 0x00 }, arp[] = { 0x08, 0x06 };
    struct netframe_class c = frame(ipv4, 2, 60);
    CHECK(c.kind == NETFRAME_UNTAGGED && c.ethertype == 0x0800 && !c.tpid && !c.vid);
    c = frame(arp, 2, 14);   /* the shortest untagged frame read */
    CHECK(c.kind == NETFRAME_UNTAGGED && c.ethertype == 0x0806);
    /* tagged 21, priority 5, ARP inside */
    static const uint8_t v21[] = { 0x81, 0x00, 0xa0, 0x15, 0x08, 0x06 };
    c = frame(v21, 6, 64);
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 21 && c.pcp == 5 && c.ethertype == 0x0806);
    CHECK_EQ(c.tpid, NETFRAME_TPID_8021Q);
    /* another VLAN, and the largest id; DEI set doesn't change the id */
    static const uint8_t v100[] = { 0x81, 0x00, 0x00, 0x64, 0x86, 0xdd };
    c = frame(v100, 6, 18);   /* the shortest tagged frame read */
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 100 && c.pcp == 0 && c.ethertype == 0x86dd);
    static const uint8_t vmax[] = { 0x81, 0x00, 0x1f, 0xff, 0x08, 0x00 };
    c = frame(vmax, 6, 64);
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 4095 && c.pcp == 0);
    /* priority-tagged: VLAN 0, a priority only */
    static const uint8_t prio[] = { 0x81, 0x00, 0xe0, 0x00, 0x08, 0x00 };
    c = frame(prio, 6, 64);
    CHECK(c.kind == NETFRAME_PRIORITY && c.vid == 0 && c.pcp == 7 && c.ethertype == 0x0800);
    /* QinQ: an 802.1ad outer tag on 21 over an 802.1Q one, and the old 0x9100 */
    static const uint8_t qinq[] = { 0x88, 0xa8, 0x00, 0x15, 0x81, 0x00 };
    c = frame(qinq, 6, 64);
    CHECK(c.kind == NETFRAME_OUTER && c.tpid == 0x88a8 && c.vid == 21 && c.ethertype == 0x8100);
    static const uint8_t q9100[] = { 0x91, 0x00, 0x00, 0x07, 0x08, 0x00 };
    c = frame(q9100, 6, 64);
    CHECK(c.kind == NETFRAME_OUTER && c.tpid == 0x9100 && c.vid == 7);
    return true;
}

/* Too short to read what its kind needs: a runt, with nothing read past
 * the end. */
bool t_netframe_short_frames(void)
{
    static const uint8_t ipv4[] = { 0x08, 0x00 }, v21[] = { 0x81, 0x00, 0x00, 0x15, 0x08, 0x00 };
    for (size_t len = 0; len < NETFRAME_HDR; len++)
        CHECK(frame(ipv4, 2, len).kind == NETFRAME_RUNT);
    for (size_t len = NETFRAME_HDR; len < NETFRAME_TAGGED; len++) {
        struct netframe_class c = frame(v21, 6, len);
        CHECK(c.kind == NETFRAME_RUNT && !c.ethertype && !c.vid);
    }
    CHECK(frame(v21, 6, NETFRAME_TAGGED).kind == NETFRAME_VLAN);
    /* A frame of exactly 14 bytes in a buffer that ends there. */
    uint8_t exact[14];
    memset(exact, 0, sizeof(exact));
    exact[12] = 0x08;
    exact[13] = 0x06;
    CHECK(netframe_classify(exact, sizeof(exact)).ethertype == 0x0806);
    CHECK(netframe_classify(NULL, 0).kind == NETFRAME_RUNT);
    return true;
}

/* ---- transmit ---- */

/* An untagged frame of len bytes: distinct address bytes, EtherType `type`,
 * then a payload counting up. */
static void untagged(uint8_t *f, size_t len, uint16_t type)
{
    for (size_t i = 0; i < len; i++)
        f[i] = (uint8_t)(i * 7 + 3);
    if (len >= 14)
        netframe_put_be16(f + 12, type);
}

/* out (n bytes) is in (len bytes) tagged with vid, then zeros. */
static bool is_tagged_copy(const uint8_t *out, size_t n, const uint8_t *in, size_t len,
                           uint16_t vid)
{
    CHECK(memcmp(out, in, 12) == 0);
    CHECK(out[12] == 0x81 && out[13] == 0x00 && netframe_be16(out + 14) == vid);
    CHECK(memcmp(out + 16, in + 12, len - 12) == 0);
    for (size_t i = len + 4; i < n; i++)
        CHECK_EQ(out[i], 0);
    return true;
}

bool t_netframe_tag(void)
{
    static uint8_t in[1600], out[1600];
    /* every length: 14..1514 tagged (short ones padded to 64), the rest refused */
    for (size_t len = 0; len <= 1600; len++) {
        untagged(in, len < sizeof(in) ? len : sizeof(in), 0x0800);
        memset(out, 0xaa, sizeof(out));   /* a buffer that held something before */
        size_t n = netframe_tag(out, sizeof(out), in, len, 21);
        if (len < NETFRAME_MIN_IN || len > NETFRAME_MAX_IN) {
            if (n)
                FAIL("length %u was tagged", (unsigned)len);
            continue;
        }
        size_t want = len + 4 < NETFRAME_PAD_OUT ? NETFRAME_PAD_OUT : len + 4;
        if (n != want)
            FAIL("length %u: tagged %u, want %u", (unsigned)len, (unsigned)n, (unsigned)want);
        CHECK(is_tagged_copy(out, n, in, len, 21));
        CHECK(netframe_tx_check(out, n, 21));
    }
    /* the VLAN: 1 and 4094 yes; 0, 4095 and anything wider no */
    untagged(in, 60, 0x0806);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 1), 64);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 4094), 64);
    CHECK(out[14] == 0x0f && out[15] == 0xfe);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 0), 0);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 4095), 0);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 0x8015), 0);   /* "priority 4, VLAN 21" */
    /* the room: exactly enough yes, a byte short no, none no */
    CHECK_EQ(netframe_tag(out, 64, in, 60, 21), 64);
    CHECK_EQ(netframe_tag(out, 63, in, 60, 21), 0);
    CHECK_EQ(netframe_tag(NULL, 0, in, 60, 21), 0);
    untagged(in, 1514, 0x0800);
    CHECK_EQ(netframe_tag(out, 1518, in, 1514, 21), 1518);
    CHECK_EQ(netframe_tag(out, 1517, in, 1514, 21), 0);
    return true;
}

/* A frame that already carries a tag never leaves, whatever its length,
 * and the refusal leaves nothing sendable in the buffer. */
bool t_netframe_tag_refuses_tagged(void)
{
    static const uint16_t tpids[] = { 0x8100, 0x88a8, 0x9100 };
    static uint8_t in[1514], out[1600];
    for (unsigned k = 0; k < 3; k++) {
        for (size_t len = NETFRAME_MIN_IN; len <= NETFRAME_MAX_IN; len += 25) {
            untagged(in, len, tpids[k]);
            if (len >= 16)
                netframe_put_be16(in + 14, 21);   /* even "already on our VLAN" */
            memset(out, 0, sizeof(out));
            CHECK_EQ(netframe_tag(out, sizeof(out), in, len, 21), 0);
            CHECK(!netframe_tx_check(out, len + 4, 21));
            CHECK(!netframe_tx_check(out, NETFRAME_PAD_OUT, 21));
        }
    }
    /* next to a TPID is not a TPID */
    untagged(in, 60, 0x8101);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 21), 64);
    untagged(in, 60, 0x88a9);
    CHECK_EQ(netframe_tag(out, sizeof(out), in, 60, 21), 64);
    return true;
}

bool t_netframe_tx_check(void)
{
    static uint8_t in[100], out[100], bad[100];
    untagged(in, 60, 0x0806);
    size_t n = netframe_tag(out, sizeof(out), in, 60, 21);
    CHECK_EQ(n, 64);
    CHECK(netframe_tx_check(out, n, 21));
    CHECK(!netframe_tx_check(out, n, 20) && !netframe_tx_check(out, n, 0));
    /* lengths: 18 and 1518 are the bounds */
    CHECK(netframe_tx_check(out, 18, 21) && !netframe_tx_check(out, 17, 21));
    CHECK(!netframe_tx_check(out, 0, 21) && !netframe_tx_check(out, 1519, 21));
    /* every change to bytes 12-17 that matters is caught */
    static const struct { unsigned at; uint8_t v; } edits[] = {
        { 12, 0x88 }, { 13, 0xa8 }, { 12, 0x08 },   /* another TPID; untagged */
        { 14, 0x20 }, { 14, 0x10 }, { 14, 0xe0 },   /* priority 1, DEI, priority 7 */
        { 15, 0x14 }, { 14, 0x01 },                  /* another VLAN */
        { 16, 0x81 },                                /* a tag inside the tag */
    };
    for (unsigned k = 0; k < sizeof(edits) / sizeof(edits[0]); k++) {
        memcpy(bad, out, sizeof(bad));
        bad[edits[k].at] = edits[k].v;
        if (edits[k].at == 16)
            bad[17] = 0x00;
        if (netframe_tx_check(bad, n, 21))
            FAIL("edit %u (byte %u = %#x) passed the check", k, edits[k].at, edits[k].v);
    }
    return true;
}

/* The check is on the driver's copy: what the caller does to its frame
 * after the copy changes nothing, and a change to the copy is caught. */
bool t_netframe_tag_copy_is_the_frame(void)
{
    static uint8_t in[200], out[200], snap[200];
    untagged(in, 150, 0x0800);
    size_t n = netframe_tag(out, sizeof(out), in, 150, 21);
    CHECK_EQ(n, 154);
    memcpy(snap, out, sizeof(snap));
    /* the caller rewrites its frame into a tagged one for another VLAN */
    netframe_put_be16(in + 12, 0x8100);
    netframe_put_be16(in + 14, 10);
    memset(in, 0, 12);
    CHECK(memcmp(out, snap, sizeof(snap)) == 0);
    CHECK(netframe_tx_check(out, n, 21));
    /* the copy itself changed (a driver bug): the last check refuses it */
    out[15] = 10;
    CHECK(!netframe_tx_check(out, n, 21));
    return true;
}

/* ---- receive ---- */

/* A frame tagged (tpid, tci) of len bytes with `inner` after the tag. */
static size_t tagged(uint8_t *f, size_t len, uint16_t tpid, uint16_t tci, uint16_t inner)
{
    for (size_t i = 0; i < len; i++)
        f[i] = (uint8_t)(i * 5 + 1);
    if (len >= 14)
        netframe_put_be16(f + 12, tpid);
    if (len >= 16)
        netframe_put_be16(f + 14, tci);
    if (len >= 18)
        netframe_put_be16(f + 16, inner);
    return len;
}

bool t_netframe_rx(void)
{
    static uint8_t f[1600], out[1600];
    /* our VLAN, any priority, DEI or not: kept */
    static const uint16_t tcis[] = { 21, 0xe015, 0x1015, 0x3015 };
    for (unsigned k = 0; k < 4; k++) {
        tagged(f, 64, 0x8100, tcis[k], 0x0806);
        CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_KEEP);
    }
    tagged(f, 1518, 0x8100, 21, 0x0800);
    CHECK_EQ(netframe_rx_check(f, 1518, 21), NETFRAME_RX_KEEP);
    CHECK_EQ(netframe_rx_check(f, 1519, 21), NETFRAME_RX_LONG);
    CHECK_EQ(netframe_rx_check(f, 0x3fff, 21), NETFRAME_RX_LONG);
    /* everything else is dropped, each by its own count */
    tagged(f, 64, 0x0800, 0, 0);
    CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_UNTAGGED);
    tagged(f, 64, 0x8100, 0xe000, 0x0800);
    CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_PRIORITY);
    static const uint16_t others[] = { 10, 11, 20, 22, 4095, 21 | 0x100 };
    for (unsigned k = 0; k < 6; k++) {
        tagged(f, 64, 0x8100, others[k], 0x0800);
        CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_OTHER_VLAN);
    }
    tagged(f, 64, 0x88a8, 21, 0x8100);
    CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_OUTER);
    tagged(f, 64, 0x9100, 21, 0x0800);
    CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_OUTER);
    static const uint16_t inner[] = { 0x8100, 0x88a8, 0x9100 };
    for (unsigned k = 0; k < 3; k++) {
        tagged(f, 64, 0x8100, 21, inner[k]);
        CHECK_EQ(netframe_rx_check(f, 64, 21), NETFRAME_RX_NESTED);
    }
    /* no VLAN configured (0) or an impossible one: nothing is kept */
    tagged(f, 64, 0x8100, 0, 0x0800);
    CHECK(netframe_rx_check(f, 64, 0) != NETFRAME_RX_KEEP);
    tagged(f, 64, 0x8100, 4095, 0x0800);
    CHECK(netframe_rx_check(f, 64, 4095) != NETFRAME_RX_KEEP);
    /* every short length: never kept */
    for (size_t len = 0; len < NETFRAME_MIN_OUT; len++) {
        tagged(f, len, 0x8100, 21, 0x0800);
        CHECK(netframe_rx_check(f, len, 21) != NETFRAME_RX_KEEP);
        CHECK_EQ(netframe_untag(out, sizeof(out), f, len), 0);
    }
    CHECK_EQ(netframe_rx_check(NULL, 0, 21), NETFRAME_RX_RUNT);
    /* the tag comes off: addresses, then everything after the tag */
    size_t len = tagged(f, 100, 0x8100, 0xa015, 0x0806);
    size_t n = netframe_untag(out, sizeof(out), f, len);
    CHECK_EQ(n, 96);
    CHECK(memcmp(out, f, 12) == 0 && memcmp(out + 12, f + 16, 84) == 0);
    CHECK(netframe_be16(out + 12) == 0x0806);
    CHECK_EQ(netframe_untag(out, 95, f, len), 0);
    CHECK_EQ(netframe_untag(out, 96, f, len), 96);
    CHECK_EQ(netframe_untag(out, sizeof(out), f, 1519), 0);
    CHECK_EQ(netframe_untag(out, sizeof(out), f, 18), 14);
    return true;
}

bool t_rtl8125_write_guard(void)
{
    /* the transmit rings' addresses, in any width or overlap */
    CHECK(!rtl_write_allowed(0x20, 4, 0) && !rtl_write_allowed(0x24, 4, 0));
    CHECK(!rtl_write_allowed(0x28, 4, 0) && !rtl_write_allowed(0x2f, 1, 0));
    CHECK(!rtl_write_allowed(0x1e, 4, 0));   /* 0x1e-0x21 reaches 0x20 */
    CHECK(rtl_write_allowed(0x10, 4, 0) && rtl_write_allowed(0x14, 4, 0));   /* DTCCR */
    CHECK(rtl_write_allowed(0x1c, 4, 0) && rtl_write_allowed(0x30, 4, 0));
    /* the command register: the receiver yes, the transmitter never */
    CHECK(rtl_write_allowed(RTL_CMD, 1, 0x08) && rtl_write_allowed(RTL_CMD, 1, 0x10));
    CHECK(rtl_write_allowed(RTL_CMD, 1, 0x88) && rtl_write_allowed(RTL_CMD, 1, 0));
    CHECK(!rtl_write_allowed(RTL_CMD, 1, 0x04) && !rtl_write_allowed(RTL_CMD, 1, 0x0c));
    CHECK(!rtl_write_allowed(0x34, 4, 0x04000000u));   /* TE in the top byte of a 32-bit write */
    CHECK(rtl_write_allowed(0x34, 4, 0x08000004u));    /* bit 2 of INT_CFG0 is not TE */
    CHECK(!rtl_write_allowed(0x36, 2, 0x0400));
    /* the transmit configuration and the doorbell */
    CHECK(!rtl_write_allowed(RTL_TXCFG, 4, 0) && !rtl_write_allowed(0x43, 1, 0));
    CHECK(rtl_write_allowed(0x44, 4, 0x41000c0fu));   /* RXCFG */
    CHECK(!rtl_write_allowed(RTL_TXSTART, 2, 1) && !rtl_write_allowed(0x93, 1, 0));
    CHECK(rtl_write_allowed(0x8c, 4, 0) && !rtl_write_allowed(0x8e, 4, 0));
    CHECK(rtl_write_allowed(0x94, 4, 0));
    /* the transmit descriptor fetch number, alone or inside a wider write */
    CHECK(!rtl_write_allowed(RTL_TDFNR, 1, 0x10) && !rtl_write_allowed(0x56, 2, 0));
    CHECK(!rtl_write_allowed(0x54, 4, 0) && rtl_write_allowed(0x56, 1, 0));   /* CFG5 yes */
    CHECK(rtl_write_allowed(0x58, 4, 0));
    /* only widths the chip has */
    CHECK(!rtl_write_allowed(0x50, 3, 0) && !rtl_write_allowed(0x50, 0, 0));
    CHECK(!rtl_write_allowed(0x50, 8, 0));
    return true;
}

/* tx.c's gate: full mode and a VLAN, nothing else. */
bool t_rtl8125_tx_gate(void)
{
    CHECK(rtl_tx_allowed(RTL_MODE_FULL, 21) && rtl_tx_allowed(RTL_MODE_FULL, 1));
    CHECK(rtl_tx_allowed(RTL_MODE_FULL, 4094));
    CHECK(!rtl_tx_allowed(RTL_MODE_FULL, 0) && !rtl_tx_allowed(RTL_MODE_FULL, 4095));
    CHECK(!rtl_tx_allowed(RTL_MODE_FULL, 0x10015));
    CHECK(!rtl_tx_allowed(RTL_MODE_PROBE, 21) && !rtl_tx_allowed(RTL_MODE_OFF, 21));
    CHECK(!rtl_tx_allowed((enum rtl_mode)7, 21));
    return true;
}

static struct rtl_args args_of(const char *a, const char *b, const char *c)
{
    const char *w[3] = { a, b, c };
    return rtl_args_parse(w, (unsigned)(a != NULL) + (b != NULL) + (c != NULL));
}

bool t_rtl8125_args(void)
{
    struct rtl_args a = args_of("netsend", "vlan=21", NULL);
    CHECK(a.mode == RTL_MODE_FULL && a.sendtest && a.vlan == 21);
    CHECK_EQ(a.arp_target, 0x0a021501);
    a = args_of("netprobe", NULL, NULL);
    CHECK(a.mode == RTL_MODE_PROBE && !a.sendtest && a.vlan == 0);
    /* netprobe wins: given both, the driver only listens */
    a = args_of("netsend", "netprobe", "vlan=21");
    CHECK(a.mode == RTL_MODE_PROBE && !a.sendtest);
    a = args_of(NULL, NULL, NULL);
    CHECK(a.mode == RTL_MODE_FULL && !a.sendtest && a.vlan == 0);
    /* words that only look like ours */
    a = args_of("netsendx", "netprobe2", "xvlan=21");
    CHECK(a.mode == RTL_MODE_FULL && !a.sendtest && a.vlan == 0);
    /* the VLAN: one valid value, else none */
    static const struct { const char *w; uint16_t v; } vl[] = {
        { "vlan=1", 1 }, { "vlan=21", 21 }, { "vlan=4094", 4094 }, { "vlan=0021", 21 },
        { "vlan=0", 0 }, { "vlan=4095", 0 }, { "vlan=40940", 0 }, { "vlan=", 0 },
        { "vlan=off", 0 }, { "vlan=-21", 0 }, { "vlan=+21", 0 }, { "vlan=21 ", 0 },
        { "vlan= 21", 0 }, { "vlan=2a", 0 }, { "vlan=99999999999", 0 }, { "vlan", 0 },
    };
    for (unsigned k = 0; k < sizeof(vl) / sizeof(vl[0]); k++) {
        const char *w[1] = { vl[k].w };
        if (rtl_vlan_arg(w, 1) != vl[k].v)
            FAIL("\"%s\": vlan %u, want %u", vl[k].w, rtl_vlan_arg(w, 1), vl[k].v);
    }
    CHECK_EQ(args_of("vlan=21", "vlan=21", NULL).vlan, 21);
    CHECK_EQ(args_of("vlan=21", "vlan=20", NULL).vlan, 0);   /* two answers: none */
    CHECK_EQ(args_of("vlan=21", "vlan=x", NULL).vlan, 0);
    CHECK_EQ(rtl_vlan_arg(NULL, 0), 0);
    /* the send test's target */
    CHECK_EQ(args_of("arpto=10.2.21.254", NULL, NULL).arp_target, 0x0a0215fe);
    CHECK_EQ(args_of("arpto=0.0.0.0", NULL, NULL).arp_target, 0);
    static const char *const bad[] = { "arpto=10.2.21", "arpto=10.2.21.256", "arpto=1.2.3.4.5",
                                       "arpto=1..2.3", "arpto=", "arpto=a.b.c.d",
                                       "arpto=1000.2.3.4", "arpto=1.2.3.4x" };
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        a = args_of(bad[k], NULL, NULL);
        if (!a.bad_target || a.arp_target != RTL_ARP_TARGET_DEFAULT)
            FAIL("\"%s\" was taken", bad[k]);
    }
    return true;
}

bool t_rtl8125_arp(void)
{
    static const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t f[ARP_FRAME_LEN], out[80];
    arp_probe(f, mac, 0x0a021501);
    static const uint8_t want[ARP_FRAME_LEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 2, 21, 1,
    };
    CHECK(memcmp(f, want, sizeof(want)) == 0);
    /* the probe goes out as any frame does: tagged, padded, checked */
    size_t n = netframe_tag(out, sizeof(out), f, sizeof(f), 21);
    CHECK_EQ(n, 64);
    CHECK(netframe_tx_check(out, n, 21));
    /* the router's answer, and the near misses */
    uint8_t r[60];
    memset(r, 0, sizeof(r));
    memcpy(r, mac, 6);
    netframe_put_be16(r + 12, 0x0806);
    static const uint8_t body[] = { 0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x02,
                                    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 10, 2, 21, 1 };
    memcpy(r + 14, body, sizeof(body));
    memcpy(r + 32, mac, 6);
    CHECK(arp_is_reply(r, sizeof(r), mac, 0x0a021501));
    CHECK(arp_is_reply(r, ARP_FRAME_LEN, mac, 0x0a021501));
    CHECK(!arp_is_reply(r, ARP_FRAME_LEN - 1, mac, 0x0a021501));
    CHECK(!arp_is_reply(r, sizeof(r), mac, 0x0a021502));   /* someone else */
    static const unsigned flips[] = { 12, 13, 15, 16, 18, 19, 21, 28, 31, 32, 37 };
    for (unsigned k = 0; k < sizeof(flips) / sizeof(flips[0]); k++) {
        uint8_t c[60];
        memcpy(c, r, sizeof(c));
        c[flips[k]] ^= 0x40;
        if (arp_is_reply(c, sizeof(c), mac, 0x0a021501))
            FAIL("byte %u changed, still a reply", flips[k]);
    }
    CHECK(!arp_is_reply(f, sizeof(f), mac, 0x0a021501));   /* our own probe is a request */
    return true;
}

/* drv/rtl8125 with `word` and `word2` (NULL: none) and only a channel for
 * DR_SERVE: it must end at once, by itself, with exit 0, and leave its job
 * empty. */
static bool runs_and_leaves(const char *word, const char *word2)
{
    handle_t job, a, b, proc;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    const char *argv[] = { "drv/rtl8125", word, word2 };
    struct spawn_handle x = { SR_DRIVER(DR_SERVE), b };
    struct spawn_args sa = {
        .path = "drv/rtl8125", .argc = word2 ? 3 : word ? 2 : 1, .argv = argv, .job = job,
        .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&sa, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(a), OK);
    struct job_info ji;
    CHECK_ST(jam_job_get_info(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("drv/rtl8125 (%s) left %lu units of kind %u", word ? word : "no word",
                 (unsigned long)ji.used[k], k);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* Without a VLAN the driver touches nothing (no word; `netsend` alone or
 * with a bad vlan=); the probe, or the send test with a VLAN, but without
 * the hardware (no RTL8125: QEMU) say so and end. */
bool t_rtl8125_stays_off(void)
{
    return runs_and_leaves(NULL, NULL) && runs_and_leaves("netprobe", NULL) &&
           runs_and_leaves("netsend", NULL) && runs_and_leaves("netsend", "vlan=4095") &&
           runs_and_leaves("netsend", "vlan=21") && runs_and_leaves("vlan=21", NULL);
}
