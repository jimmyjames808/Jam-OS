/* utest: the RTL8125 driver's transmit descriptor and bookkeeping, as
 * pure functions (drivers/rtl8125/txdesc.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "txdesc.h"
#include "utest.h"

/* The descriptor is rge's 32 bytes, the transmitter goes on only when the
 * chip's format bit says the same, and the command word marks the ring's
 * end on the last descriptor alone. */
bool t_rtl8125_txdesc(void)
{
    CHECK_EQ(RTL_TXD_SIZE, 32);
    CHECK(RTL_TXD_CMDSTS == 0 && RTL_TXD_EXTSTS == 4 && RTL_TXD_ADDR == 8);
    CHECK_EQ(RTL_TXD_RESERVED, 16);
    /* the bit read back from MAC OCP 0xeb58: only bit 0 decides */
    CHECK(rtl_txd_format_ok(0x0001) && rtl_txd_format_ok(0xffff));
    CHECK(!rtl_txd_format_ok(0x0000) && !rtl_txd_format_ok(0xfffe));   /* 16-byte: refused */
    for (uint32_t i = 0; i < 600; i++) {
        uint32_t c = rtl_txd_cmd(i, 256, 64);
        CHECK(c & RTL_TXD_OWN && c & RTL_TXD_SOF && c & RTL_TXD_EOF);
        CHECK_EQ(c & RTL_TXD_LEN, 64);
        if (!!(c & RTL_TXD_EOR) != (i % 256 == 255))
            FAIL("descriptor %u: end of ring %s", i, c & RTL_TXD_EOR ? "set" : "missing");
    }
    CHECK_EQ(rtl_txd_cmd(0, 256, 1518) & RTL_TXD_LEN, 1518);
    CHECK_EQ(rtl_txd_cmd(0, 256, 1518) & (RTL_TXD_ERR | RTL_TXD_COLL), 0);
    /* A chip reading the ring in its own steps finds each descriptor the
     * driver wrote, in order, and wraps where the driver put EOR. */
    static uint8_t ring[256 * RTL_TXD_SIZE];
    memset(ring, 0, sizeof(ring));
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = rtl_txd_cmd(i, 256, 60 + i);
        memcpy(ring + i * RTL_TXD_SIZE + RTL_TXD_CMDSTS, &c, 4);
    }
    uint32_t at = 0;
    for (uint32_t k = 0; k < 256; k++) {
        uint32_t c;
        memcpy(&c, ring + at * RTL_TXD_SIZE + RTL_TXD_CMDSTS, 4);
        if (!(c & RTL_TXD_OWN) || (c & RTL_TXD_LEN) != 60 + k)
            FAIL("step %u: the chip read %#x", k, c);
        at = c & RTL_TXD_EOR ? 0 : at + 1;
    }
    CHECK_EQ(at, 0);
    return true;
}

/* The doorbell again for a descriptor the chip still owns: the PC rang it
 * 69912 times in 4 s, once per interrupt. Looked at every 50 us for 4 s,
 * a stuck descriptor now gets a handful (1 ms after it was queued, then
 * gaps growing to 1 s); descriptors handed back within 0.5 ms get none; a
 * new descriptor to wait for starts over. */
bool t_rtl8125_kick(void)
{
    const uint64_t ms = 1000000;
    struct rtl_kick k = { 0 };
    unsigned kicks = 0;
    uint64_t first = 0, last = 0, min_gap = ~0ull;
    for (uint64_t now = 100 * ms; now < 4100 * ms; now += 50000) {
        if (!rtl_kick_due(&k, 7, 100 * ms, now))
            continue;
        if (!kicks)
            first = now;
        else if (now - last < min_gap)
            min_gap = now - last;
        last = now;
        kicks++;
    }
    if (kicks < 5 || kicks > 10)
        FAIL("%u doorbells for one stuck descriptor in 4 s", kicks);
    CHECK(first >= 101 * ms && first < 102 * ms);
    CHECK(min_gap >= 4 * ms);
    CHECK(rtl_kick_due(&k, 7, 100 * ms, last + RTL_KICK_MAX_NS));   /* then one a second */
    CHECK(!rtl_kick_due(&k, 7, 100 * ms, last + RTL_KICK_MAX_NS + ms));
    /* progress: each descriptor back 0.5 ms after it was queued */
    struct rtl_kick p = { 0 };
    for (uint32_t c = 0; c < 1000; c++)
        for (uint64_t dt = 0; dt < ms / 2; dt += 50000)
            if (rtl_kick_due(&p, c, c * ms, c * ms + dt))
                FAIL("descriptor %u: a doorbell after %lu ns", c, (unsigned long)dt);
    /* a new descriptor: its own 1 ms first */
    CHECK(!rtl_kick_due(&k, 8, last + 2 * RTL_KICK_MAX_NS, last + 2 * RTL_KICK_MAX_NS));
    CHECK(rtl_kick_due(&k, 8, last + 2 * RTL_KICK_MAX_NS, last + 2 * RTL_KICK_MAX_NS + ms));
    return true;
}
