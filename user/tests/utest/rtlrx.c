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
    rtl_rxd_arm(d, 0x86de02800ull, 255, 256, 2048);
    uint32_t c, x;
    memcpy(&c, d + RX_DESC_CMDSTS, 4);
    memcpy(&x, d + RX_DESC_EXTSTS, 4);
    CHECK_EQ(c, RX_OWN | RX_EOR | 2048);
    CHECK_EQ(x, 0);
    CHECK_EQ(rtl_rxd_addr(d), 0x86de02800ull);
    return true;
}

/* ---- a chip writing back as the version-3 descriptor lets it ------------------- */

#define LAP_DESCS 16u
#define LAP_BUF   2048u

static uint64_t lap_buf(uint32_t i)
{
    return 0x86de00000ull + (uint64_t)i * LAP_BUF;
}

/* The chip receives one frame of len bytes into descriptor *at: false if
 * the driver hasn't handed it over. *addr: where the frame's bytes went.
 * The write-back puts the frame's timestamp where the address was (the
 * union in Realtek's struct RxDescV3) and RSS and header information in
 * the first 16 bytes, then the status with the ownership bit clear. */
static bool lap_chip_rx(uint8_t *ring, uint32_t *at, uint32_t len, uint64_t stamp,
                        uint64_t *addr)
{
    uint8_t *d = ring + *at * RX_DESC_SIZE;
    uint32_t c;
    memcpy(&c, d + RX_DESC_CMDSTS, 4);
    if (!(c & RX_OWN))
        return false;
    memcpy(addr, d + RX_DESC_ADDR, 8);
    memset(d, 0x5a, 16);
    memcpy(d + RX_DESC_ADDR, &stamp, 8);
    uint32_t st = (c & RX_EOR) | RX_SOF | RX_EOF | ((len + RX_CRC) & RX_LEN);
    memcpy(d + RX_DESC_CMDSTS, &st, 4);
    *at = c & RX_EOR ? 0 : *at + 1;
    return true;
}

/* Three laps of the ring: every frame lands in its descriptor's own
 * buffer, the second and third laps too. The PC's receive stopped after
 * the first lap (boot-0075) while the driver wrote the address only once,
 * at the start: a descriptor handed back held what the chip had written
 * there. */
bool t_rtl8125_rxdesc_laps(void)
{
    static uint8_t ring[LAP_DESCS * RX_DESC_SIZE];
    memset(ring, 0, sizeof(ring));
    for (uint32_t i = 0; i < LAP_DESCS; i++)
        rtl_rxd_arm(ring + i * RX_DESC_SIZE, lap_buf(i), i, LAP_DESCS, LAP_BUF);
    uint32_t at = 0, next = 0;
    for (uint32_t f = 0; f < 3 * LAP_DESCS; f++) {
        uint64_t addr = 0;
        if (!lap_chip_rx(ring, &at, 60 + f, 0x1000 + f, &addr))
            FAIL("frame %u: descriptor %u still the driver's", f, f % LAP_DESCS);
        if (addr != lap_buf(f % LAP_DESCS))
            FAIL("frame %u (lap %u): the chip wrote to %#lx, not descriptor %u's buffer %#lx", f,
                 f / LAP_DESCS + 1, (unsigned long)addr, f % LAP_DESCS,
                 (unsigned long)lap_buf(f % LAP_DESCS));
        /* the driver: the frame back, its length and status as written */
        uint8_t *d = ring + next * RX_DESC_SIZE;
        uint32_t st;
        memcpy(&st, d + RX_DESC_CMDSTS, 4);
        CHECK(!(st & RX_OWN));
        CHECK_EQ(st & RX_LEN, 60 + f + RX_CRC);
        CHECK_EQ(rtl_rxd_addr(d), 0x1000 + f);   /* what the write-back left there */
        rtl_rxd_arm(d, lap_buf(next), next, LAP_DESCS, LAP_BUF);
        next = (next + 1) % LAP_DESCS;
    }
    CHECK_EQ(at, 0);
    return true;
}
