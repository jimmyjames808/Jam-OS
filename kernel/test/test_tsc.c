/* TSC calibration (arch/x86_64/tsc.c) against a reference that is broken.
 *
 *   tsc_stuck_hpet_refused
 *       An HPET whose counter never moves is given up on (0, so the boot
 *       falls back to the PM timer) instead of hanging the boot. The HPET
 *       here is a block of RAM laid out like one: a sane period, a counter
 *       that stays at 0. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/time.h>

#define HPET_PERIOD_10NS (10000000ull << 32)   /* capabilities: 10 ns period, 32-bit counter */

static uint64_t stuck_hpet[0x100 / 8];   /* up to the main counter at 0xf0 */

KTEST(tsc_stuck_hpet_refused)
{
    stuck_hpet[0] = HPET_PERIOD_10NS;
    uint64_t t0 = uptime_ns();
    uint64_t hz = tsc_measure_hpet(stuck_hpet);
    uint64_t ms = (uptime_ns() - t0) / NS_PER_MS;
    kprintf("ktest %s: a stuck HPET gave %lu Hz after %lu ms\n", ktest_current, hz, ms);
    KT_EQ(hz, 0);
    KT_EQ(stuck_hpet[0x10 / 8], 1);   /* it did try: the counter was enabled */
}
