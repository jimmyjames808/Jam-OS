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
