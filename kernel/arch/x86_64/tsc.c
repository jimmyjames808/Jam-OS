/* TSC frequency. The HPET is the preferred reference, then the ACPI PM timer
 * (some recent Intel boards disable it), then CPUID 0x15, then the loader. */
#include <jam/acpi.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/time.h>
#include <jam/x86.h>

#define CAL_MS 50

uint64_t tsc_hz;
static uint64_t tsc_boot;

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint64_t measure_hpet(void)
{
    volatile uint64_t *h = vmm_map_mmio(acpi.hpet_phys, 1024);
    uint64_t caps = h[0];                 /* general capabilities/id */
    uint64_t period_fs = caps >> 32;      /* counter period */
    if (!period_fs || period_fs > 100000000)
        return 0;
    /* Capability bit 13 (COUNT_SIZE_CAP): 1 = 64-bit counter, 0 = 32-bit.
     * A 32-bit counter wraps at 2^32, so mask reads and compute deltas
     * modulo the width. CAL_MS at a sane period keeps target < 2^32. */
    uint64_t mask = (caps & (1ull << 13)) ? ~0ull : 0xffffffffull;
    h[0x10 / 8] |= 1;                  /* general config: enable counter */

    uint64_t target = (uint64_t)CAL_MS * 1000000000000ull / period_fs;
    uint64_t h0 = h[0xf0 / 8] & mask, t0 = rdtsc();
    uint64_t h1;
    while ((((h1 = h[0xf0 / 8] & mask) - h0) & mask) < target)
        __asm__ volatile("pause");
    uint64_t t1 = rdtsc();
    uint64_t ns = ((h1 - h0) & mask) * period_fs / 1000000;
    return ns ? (t1 - t0) * 1000000000ull / ns : 0;
}

static uint64_t measure_pm_timer(void)
{
    uint16_t port = (uint16_t)acpi.pm_timer_port;
    uint32_t mask = acpi.pm_timer_32bit ? 0xffffffffu : 0xffffffu;
    const uint64_t pm_hz = 3579545;
    uint64_t target = pm_hz * CAL_MS / 1000;

    uint32_t last = inl(port) & mask;
    uint64_t elapsed = 0, t0 = rdtsc();
    for (uint64_t spins = 0; elapsed < target; spins++) {
        uint32_t now = inl(port) & mask;
        elapsed += (now - last) & mask;
        last = now;
        if (spins > 100000000)
            return 0;   /* timer not ticking */
    }
    uint64_t t1 = rdtsc();
    uint64_t ns = elapsed * 1000000000ull / pm_hz;
    return (t1 - t0) * 1000000000ull / ns;
}

static void show(const char *name, uint64_t hz)
{
    if (hz)
        kprintf("  %-10s %lu.%03lu MHz\n", name, hz / 1000000, (hz / 1000) % 1000);
    else
        kprintf("  %-10s unavailable\n", name);
}

void tsc_calibrate_with_loader(uint64_t loader_hz)
{
    uint64_t hpet = acpi.hpet_phys ? measure_hpet() : 0;
    uint64_t pm = acpi.pm_timer_port ? measure_pm_timer() : 0;
    uint64_t cpuid = 0;
    if (cpu_features.crystal_hz && cpu_features.tsc_ratio_den)
        cpuid = (uint64_t)cpu_features.crystal_hz * cpu_features.tsc_ratio_num /
                cpu_features.tsc_ratio_den;

    kprintf("tsc: frequency sources%s:\n",
            cpu_features.tsc_invariant ? "" : " (TSC NOT invariant)");
    show("HPET", hpet);
    show("PM timer", pm);
    show("CPUID 15h", cpuid);
    show("loader", loader_hz);

    uint64_t hz = hpet ? hpet : pm ? pm : cpuid ? cpuid : loader_hz;
    if (!hz)
        panic("tsc: no way to measure the TSC frequency");
    tsc_boot = rdtsc();   /* log timestamps count from here */
    tsc_hz = hz;
    kprintf("tsc: using %lu.%03lu MHz from %s\n", tsc_hz / 1000000, (tsc_hz / 1000) % 1000,
            hpet ? "HPET" : pm ? "PM timer" : cpuid ? "CPUID 15h" : "loader");
}

void tsc_calibrate(void)
{
    tsc_calibrate_with_loader(0);
}

void udelay(uint64_t us)
{
    uint64_t end = rdtsc() + tsc_hz / 1000000 * us;
    while (rdtsc() < end)
        __asm__ volatile("pause");
}

uint64_t tsc_to_ns(uint64_t d)
{
    return d / tsc_hz * 1000000000ull + (d % tsc_hz) * 1000000000ull / tsc_hz;
}

uint64_t uptime_ns(void)
{
    return tsc_to_ns(rdtsc() - tsc_boot);
}

uint64_t uptime_to_tsc(uint64_t ns)
{
    if (ns == UINT64_MAX)
        return UINT64_MAX;
    /* Rounded up: at the returned TSC value uptime_ns() is already >= ns. */
    uint64_t q = ns / 1000000000ull, r = ns % 1000000000ull;
    /* Saturate: a deadline ~2^64 TSC cycles out (INT64_MAX ns on a >2 GHz
     * TSC) must not wrap into the past and fire at once, forever. */
    uint64_t frac = (r * tsc_hz + 999999999ull) / 1000000000ull;
    uint64_t room = UINT64_MAX - tsc_boot - tsc_hz;   /* frac <= tsc_hz */
    if (q > room / tsc_hz)
        return UINT64_MAX;
    return tsc_boot + q * tsc_hz + frac;
}
