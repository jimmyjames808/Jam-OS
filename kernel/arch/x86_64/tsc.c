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
    uint64_t period_fs = h[0] >> 32;   /* capabilities: counter period */
    if (!period_fs || period_fs > 100000000)
        return 0;
    h[0x10 / 8] |= 1;                  /* general config: enable counter */

    uint64_t target = (uint64_t)CAL_MS * 1000000000000ull / period_fs;
    uint64_t h0 = h[0xf0 / 8], t0 = rdtsc();
    uint64_t h1;
    while ((h1 = h[0xf0 / 8]) - h0 < target)
        __asm__ volatile("pause");
    uint64_t t1 = rdtsc();
    uint64_t ns = (h1 - h0) * period_fs / 1000000;
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

    kprintf("tsc: frequency sources%s:\n", cpu_features.tsc_invariant ? "" : " (TSC NOT invariant)");
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

uint64_t uptime_ns(void)
{
    uint64_t d = rdtsc() - tsc_boot;
    return d / tsc_hz * 1000000000ull + (d % tsc_hz) * 1000000000ull / tsc_hz;
}
