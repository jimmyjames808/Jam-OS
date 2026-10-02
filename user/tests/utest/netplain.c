/* utest: the untagged network mode of <jam/netframe.h> (netframe_plain,
 * netframe_tx_check_plain, netframe_rx_check_plain) and the functions
 * that pick by the mode (netframe_tx_copy, netframe_tx_final,
 * netframe_rx_mode, netframe_rx_take), over every length, every tag and
 * every mode value a driver could hold; and <jam/netdev.h>'s start words
 * for the modes (vlan=none, netdev_mode_word, netdev_mode_str). The rule
 * under test: in the untagged mode no frame leaves tagged and no tagged
 * frame is taken in; a mode that is neither a VLAN nor untagged sends and
 * takes in nothing. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/netdev.h>
#include <jam/netframe.h>
#include <os.h>
#include "utest.h"

static const uint16_t tpids[] = { NETFRAME_TPID_8021Q, NETFRAME_TPID_8021AD, NETFRAME_TPID_9100 };
#define NTPIDS (sizeof(tpids) / sizeof(tpids[0]))

/* A frame of len bytes: distinct bytes, EtherType (or TPID) `type` at 12. */
static void frame(uint8_t *f, size_t len, uint16_t type)
{
    for (size_t i = 0; i < len; i++)
        f[i] = (uint8_t)(i * 13 + 5);
    if (len >= 14)
        netframe_put_be16(f + 12, type);
}

/* Does buf (len bytes) pass the last check of some VLAN? */
static bool passes_any_vlan(const uint8_t *buf, size_t len)
{
    for (uint16_t vid = 0; vid <= 4095; vid++)
        if (netframe_tx_check(buf, len, vid))
            return true;
    return false;
}

/* Every length: 14..1514 copied as they are and padded with zeros to 60,
 * the rest refused; the copy passes the untagged check and never a VLAN's. */
bool t_netframe_plain(void)
{
    static uint8_t in[1600], out[1600];
    for (size_t len = 0; len <= 1600; len++) {
        frame(in, len, 0x0800);
        memset(out, 0xaa, sizeof(out));   /* a buffer that held something before */
        size_t n = netframe_plain(out, sizeof(out), in, len);
        if (len < NETFRAME_MIN_IN || len > NETFRAME_MAX_IN) {
            if (n)
                FAIL("length %u was copied", (unsigned)len);
            continue;
        }
        size_t want = len < NETFRAME_PAD_PLAIN ? NETFRAME_PAD_PLAIN : len;
        if (n != want)
            FAIL("length %u: copied %u, want %u", (unsigned)len, (unsigned)n, (unsigned)want);
        CHECK(memcmp(out, in, len) == 0);
        for (size_t i = len; i < n; i++)
            CHECK_EQ(out[i], 0);
        CHECK(netframe_tx_check_plain(out, n));
        CHECK(!netframe_tx_check(out, n, 21) && !netframe_tx_check(out, n, 2048));
    }
    /* the room: exactly enough yes, a byte short no, none no */
    frame(in, 40, 0x0806);
    CHECK_EQ(netframe_plain(out, 60, in, 40), 60);
    CHECK_EQ(netframe_plain(out, 59, in, 40), 0);
    CHECK_EQ(netframe_plain(NULL, 0, in, 40), 0);
    frame(in, 1514, 0x0800);
    CHECK_EQ(netframe_plain(out, 1514, in, 1514), 1514);
    CHECK_EQ(netframe_plain(out, 1513, in, 1514), 0);
    /* any EtherType that isn't a tag's goes: 802.3 lengths, IPv6, next to a TPID */
    static const uint16_t fine[] = { 0x0000, 0x0040, 0x05dc, 0x0806, 0x86dd, 0x8101, 0x80ff,
                                     0x88a7, 0x88a9, 0x9101, 0x9200, 0xffff };
    for (unsigned k = 0; k < sizeof(fine) / sizeof(fine[0]); k++) {
        frame(in, 60, fine[k]);
        CHECK_EQ(netframe_plain(out, sizeof(out), in, 60), 60);
    }
    return true;
}

/* A frame that carries a tag never leaves untagged-mode copying, whatever
 * its length, and the refusal leaves nothing any last check passes. */
bool t_netframe_plain_refuses_tagged(void)
{
    static uint8_t in[1514], out[1600];
    for (unsigned k = 0; k < NTPIDS; k++) {
        for (size_t len = NETFRAME_MIN_IN; len <= NETFRAME_MAX_IN; len += 25) {
            frame(in, len, tpids[k]);
            if (len >= 16)
                netframe_put_be16(in + 14, 21);   /* "on VLAN 21": still refused */
            memset(out, 0, sizeof(out));
            CHECK_EQ(netframe_plain(out, sizeof(out), in, len), 0);
            size_t n = len < NETFRAME_PAD_PLAIN ? NETFRAME_PAD_PLAIN : len;
            CHECK(!netframe_tx_check_plain(out, n));
            CHECK(!passes_any_vlan(out, n < NETFRAME_MIN_OUT ? NETFRAME_MIN_OUT : n));
        }
    }
    return true;
}

/* The last check in the untagged mode: lengths, and bytes 12-13 edited to
 * a TPID after the copy (what a hostile writer of the buffer could do). */
bool t_netframe_tx_check_plain(void)
{
    static uint8_t in[1514], out[1600];
    frame(in, 100, 0x0800);
    size_t n = netframe_plain(out, sizeof(out), in, 100);
    CHECK_EQ(n, 100);
    CHECK(netframe_tx_check_plain(out, 100));
    CHECK(netframe_tx_check_plain(out, 60) && !netframe_tx_check_plain(out, 59));
    CHECK(!netframe_tx_check_plain(out, 0) && !netframe_tx_check_plain(NULL, 100));
    CHECK(netframe_tx_check_plain(out, 1514) && !netframe_tx_check_plain(out, 1515));
    CHECK(!netframe_tx_check_plain(out, 1518));
    for (unsigned k = 0; k < NTPIDS; k++) {
        netframe_put_be16(out + 12, tpids[k]);
        CHECK(!netframe_tx_check_plain(out, n));
    }
    netframe_put_be16(out + 12, 0x0800);
    CHECK(netframe_tx_check_plain(out, n));
    return true;
}

/* tx_copy then tx_final for every value a driver's mode could hold, on an
 * untagged and a tagged frame: a frame leaves only in a VLAN mode, tagged
 * with exactly that VLAN, or in the untagged mode, untagged. */
bool t_netframe_tx_modes(void)
{
    static uint8_t in[200], out[256];
    for (uint32_t m = 0; m <= 0xffff; m++) {
        for (unsigned tagged = 0; tagged < 2; tagged++) {
            frame(in, 100, tagged ? NETFRAME_TPID_8021Q : 0x0806);
            memset(out, 0, sizeof(out));
            size_t n = netframe_tx_copy(out, sizeof(out), in, 100, (uint16_t)m);
            bool sent = n && netframe_tx_final(out, n, (uint16_t)m);
            bool want = !tagged && netframe_mode_ok(m);
            if (sent != want)
                FAIL("mode %#x, %s frame: %s", m, tagged ? "a tagged" : "an untagged",
                     sent ? "sent" : "refused");
            if (!sent)
                continue;
            struct netframe_class c = netframe_classify(out, n);
            if (m == NETFRAME_MODE_UNTAGGED)
                CHECK(c.kind == NETFRAME_UNTAGGED && n == 100);
            else
                CHECK(c.kind == NETFRAME_VLAN && c.vid == m && c.pcp == 0 && n == 104);
        }
    }
    /* a buffer made for one mode fails the other's last check */
    frame(in, 100, 0x0800);
    size_t n = netframe_tx_copy(out, sizeof(out), in, 100, 21);
    CHECK(netframe_tx_final(out, n, 21) && !netframe_tx_final(out, n, NETFRAME_MODE_UNTAGGED));
    n = netframe_tx_copy(out, sizeof(out), in, 100, NETFRAME_MODE_UNTAGGED);
    CHECK(netframe_tx_final(out, n, NETFRAME_MODE_UNTAGGED) && !netframe_tx_final(out, n, 21));
    CHECK(!netframe_tx_final(out, n, 0) && !netframe_tx_final(out, n, 0x1001));
    return true;
}

/* Receive in the untagged mode: only untagged frames of 14..1514 are kept,
 * every tagged one dropped by its reason (VLAN 0 too); and receive for
 * modes that are off keeps nothing. */
bool t_netframe_rx_plain(void)
{
    static uint8_t f[1600], out[1600];
    const uint16_t U = NETFRAME_MODE_UNTAGGED;
    for (size_t len = 0; len <= 1600; len++) {
        frame(f, len, 0x0800);
        enum netframe_rx v = netframe_rx_check_plain(f, len);
        enum netframe_rx want = len < NETFRAME_HDR ? NETFRAME_RX_RUNT
                                : len > NETFRAME_MAX_IN ? NETFRAME_RX_LONG : NETFRAME_RX_KEEP;
        if (v != want)
            FAIL("untagged, length %u: %u, want %u", (unsigned)len, v, want);
        CHECK_EQ(netframe_rx_mode(f, len, U), v);
        size_t n = netframe_rx_take(out, sizeof(out), f, len, U);
        CHECK_EQ(n, v == NETFRAME_RX_KEEP ? len : 0);
        CHECK(!n || memcmp(out, f, len) == 0);
    }
    /* tagged: dropped whatever the VLAN, priority or length */
    static const uint16_t tcis[] = { 0, 0xe000, 1, 21, 0xa015, 4094, 4095 };
    for (unsigned k = 0; k < sizeof(tcis) / sizeof(tcis[0]); k++) {
        for (size_t len = NETFRAME_TAGGED; len <= 1518; len += 50) {
            frame(f, len, NETFRAME_TPID_8021Q);
            netframe_put_be16(f + 14, tcis[k]);
            enum netframe_rx v = netframe_rx_check_plain(f, len);
            CHECK_EQ(v, (tcis[k] & NETFRAME_VID_MASK) ? NETFRAME_RX_OTHER_VLAN
                                                      : NETFRAME_RX_PRIORITY);
        }
    }
    for (unsigned k = 1; k < NTPIDS; k++) {
        frame(f, 64, tpids[k]);
        CHECK_EQ(netframe_rx_check_plain(f, 64), NETFRAME_RX_OUTER);
    }
    for (size_t len = NETFRAME_HDR; len < NETFRAME_TAGGED; len++) {   /* a tag cut short */
        frame(f, len, NETFRAME_TPID_8021Q);
        CHECK_EQ(netframe_rx_check_plain(f, len), NETFRAME_RX_RUNT);
    }
    CHECK_EQ(netframe_rx_check_plain(NULL, 0), NETFRAME_RX_RUNT);
    /* modes that are off: nothing kept, nothing taken */
    static const uint16_t off[] = { 0, 4095, 0x1001, 0x0fff | 0x8000, 0xffff };
    frame(f, 64, 0x0800);
    for (unsigned k = 0; k < sizeof(off) / sizeof(off[0]); k++) {
        CHECK(netframe_rx_mode(f, 64, off[k]) != NETFRAME_RX_KEEP);
        CHECK_EQ(netframe_rx_take(out, sizeof(out), f, 64, off[k]), 0);
    }
    /* a VLAN mode is netframe_rx_check, and takes the tag off */
    frame(f, 64, NETFRAME_TPID_8021Q);
    netframe_put_be16(f + 14, 21);
    netframe_put_be16(f + 16, 0x0800);
    CHECK_EQ(netframe_rx_mode(f, 64, 21), NETFRAME_RX_KEEP);
    CHECK_EQ(netframe_rx_mode(f, 64, U), NETFRAME_RX_OTHER_VLAN);
    CHECK_EQ(netframe_rx_take(out, sizeof(out), f, 64, 21), 60);
    CHECK_EQ(netframe_rx_take(out, 59, f, 64, 21), 0);
    frame(f, 64, 0x0800);
    CHECK_EQ(netframe_rx_mode(f, 64, 21), NETFRAME_RX_UNTAGGED);
    CHECK_EQ(netframe_rx_take(out, 63, f, 64, U), 0);
    return true;
}

/* The start words and the mode's names. */
bool t_netdev_mode_words(void)
{
    const uint16_t U = NETFRAME_MODE_UNTAGGED;
    CHECK_EQ(netdev_vlan_word("vlan=none"), U);
    CHECK_EQ(netdev_vlan_word("vlan=untagged"), U);
    static const char *const bad[] = { "vlan=None", "vlan=NONE", "vlan=nonex", "vlan=non",
                                       "vlan=untag", "vlan=untaggedx", "vlan=none ",
                                       "vlan= none", "vlan=4096", "vlan=off", "none",
                                       "untagged", "vlan:none" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (netdev_vlan_word(bad[i]))
            FAIL("\"%s\" gave mode %#x", bad[i], netdev_vlan_word(bad[i]));
    const char *same[] = { "vlan=none", "vlan=untagged" };
    CHECK_EQ(netdev_vlan_args(same, 2), U);
    const char *differ[] = { "vlan=none", "vlan=21" };
    CHECK_EQ(netdev_vlan_args(differ, 2), 0);
    const char *off[] = { "vlan=none", "vlan=off" };
    CHECK_EQ(netdev_vlan_args(off, 2), 0);
    /* every mode value: its word reads back as itself, or there is none */
    char w[NETDEV_MODE_TEXT], s[NETDEV_MODE_TEXT];
    for (uint32_t m = 0; m <= 0xffff; m++) {
        netdev_mode_word((uint16_t)m, w);
        uint16_t back = w[0] ? netdev_vlan_word(w) : 0;
        if (back != (netframe_mode_ok(m) ? m : 0))
            FAIL("mode %#x: word \"%s\" reads back as %#x", m, w, back);
    }
    netdev_mode_word(21, w);
    CHECK(!strcmp(w, "vlan=21"));
    netdev_mode_word(U, w);
    CHECK(!strcmp(w, "vlan=none"));
    CHECK(!strcmp(netdev_mode_str(4094, s), "VLAN 4094"));
    CHECK(!strcmp(netdev_mode_str(1, s), "VLAN 1"));
    CHECK(!strcmp(netdev_mode_str(U, s), "untagged"));
    CHECK(!strcmp(netdev_mode_str(0, s), "off") && !strcmp(netdev_mode_str(4095, s), "off"));
    return true;
}
