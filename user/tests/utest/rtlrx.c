/* utest: the RTL8125 driver's receive descriptor, as pure functions
 * (drivers/rtl8125/rxdesc.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "rxdesc.h"
#include "utest.h"

/* The descriptor is rge's 32 bytes (the chip's version 3, RXCFG bit 24),
 * and a descriptor handed back carries the ownership bit, the buffer's
 * size and, on the last one only, the ring's end. */
bool t_rtl8125_rxdesc(void)
{
    CHECK_EQ(RX_DESC_SIZE, 32);
    CHECK(RX_DESC_ADDR == 16 && RX_DESC_EXTSTS == 24 && RX_DESC_CMDSTS == 28);
    CHECK_EQ(0x41000c00u & RTL_RXCFG_DESC_V3, RTL_RXCFG_DESC_V3);   /* rge's RXCFG has it */
    for (uint32_t i = 0; i < 600; i++) {
        uint32_t c = rtl_rxd_cmd(i, 256, 2048);
        CHECK(c & RX_OWN);
        CHECK_EQ(c & RX_LEN, 2048);
        CHECK_EQ(c & (RX_SOF | RX_EOF | RX_ERRSUM), 0);
        if (!!(c & RX_EOR) != (i % 256 == 255))
            FAIL("descriptor %u: end of ring %s", i, c & RX_EOR ? "set" : "missing");
    }
    static uint8_t d[RX_DESC_SIZE];
    memset(d, 0xa5, sizeof(d));
    rtl_rxd_arm(d, 255, 256, 2048);
    uint32_t c, x;
    memcpy(&c, d + RX_DESC_CMDSTS, 4);
    memcpy(&x, d + RX_DESC_EXTSTS, 4);
    CHECK_EQ(c, RX_OWN | RX_EOR | 2048);
    CHECK_EQ(x, 0);
    return true;
}
