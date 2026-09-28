#include <jam/acpi.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/time.h>
#include <jam/trap.h>
#include <jam/x86.h>

#define MSR_APIC_BASE    0x1b
#define MSR_TSC_DEADLINE 0x6e0
#define APIC_BASE_X2     (1ull << 10)
#define APIC_BASE_EN     (1ull << 11)

/* Register offsets (xAPIC MMIO); x2APIC MSR = 0x800 + offset / 16. */
#define REG_ID      0x020
#define REG_TPR     0x080
#define REG_EOI     0x0b0
#define REG_SVR     0x0f0
#define REG_ESR     0x280
#define REG_LVT_TMR 0x320
#define REG_LVT_LINT0 0x350
#define REG_LVT_LINT1 0x360
#define REG_LVT_ERR 0x370
#define REG_TMR_INIT 0x380
#define REG_TMR_CUR  0x390
#define REG_TMR_DIV  0x3e0

#define LVT_MASKED      (1u << 16)
#define LVT_NMI         (4u << 8)
#define LVT_LEVEL       (1u << 15)
#define LVT_ACTIVE_LOW  (1u << 13)
#define TMR_PERIODIC    (1u << 17)
#define TMR_TSC_DEADLINE (2u << 17)

static bool x2;
static volatile uint32_t *mmio;
static bool use_deadline;
static uint32_t apic_ticks_per_sec;   /* with divide-by-16 */
static uint64_t tsc_period;           /* TSC cycles per tick */

static uint32_t rd(uint32_t reg)
{
    return x2 ? (uint32_t)rdmsr(0x800 + reg / 16) : mmio[reg / 4];
}

static void wr(uint32_t reg, uint32_t v)
{
    if (x2)
        wrmsr(0x800 + reg / 16, v);
    else
        mmio[reg / 4] = v;
}

bool lapic_x2apic_mode(void)
{
    return x2;
}

void lapic_init_bsp(bool x2apic)
{
    x2 = x2apic;
    if (!x2) {
        uint64_t base = rdmsr(MSR_APIC_BASE) & 0x000ffffffffff000ull;
        if (base != acpi.lapic_phys)
            kprintf("lapic: MSR base %lx differs from MADT %lx\n", base, acpi.lapic_phys);
        mmio = vmm_map_mmio(base, PAGE_SIZE);
    }
}

uint32_t lapic_id(void)
{
    return x2 ? rd(REG_ID) : rd(REG_ID) >> 24;
}

void lapic_eoi(void)
{
    wr(REG_EOI, 0);
}

static void on_spurious(struct trap_frame *f)
{
    (void)f;   /* no EOI for spurious interrupts */
}

/* Handlers can't log (the interrupted code may hold the log lock), so
 * errors are counted and reported later. */
volatile uint64_t lapic_errors;
volatile uint32_t lapic_last_esr;

static void on_error(struct trap_frame *f)
{
    (void)f;
    wr(REG_ESR, 0);
    lapic_last_esr = rd(REG_ESR);
    __atomic_add_fetch(&lapic_errors, 1, __ATOMIC_RELAXED);
    lapic_eoi();
}

static void on_timer(struct trap_frame *f)
{
    (void)f;
    this_cpu()->ticks++;
    if (use_deadline)
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + tsc_period);
    lapic_eoi();
}

void lapic_init_cpu(struct cpu *c)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (x2 && !(base & APIC_BASE_X2))
        panic("lapic: cpu %u is not in x2APIC mode", c->index);
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);

    wr(REG_TPR, 0);
    wr(REG_LVT_TMR, LVT_MASKED);
    wr(REG_LVT_LINT0, LVT_MASKED);
    wr(REG_LVT_LINT1, LVT_MASKED);
    /* Wire the LINT pins the MADT says carry NMIs. */
    for (uint32_t i = 0; i < acpi.nmi_count; i++) {
        if (acpi.nmis[i].uid != 0xffffffff && acpi.nmis[i].uid != c->acpi_uid)
            continue;
        uint32_t lvt = LVT_NMI;
        if ((acpi.nmis[i].flags & 3) == 3)
            lvt |= LVT_ACTIVE_LOW;
        if (((acpi.nmis[i].flags >> 2) & 3) == 3)
            lvt |= LVT_LEVEL;
        wr(acpi.nmis[i].lint ? REG_LVT_LINT1 : REG_LVT_LINT0, lvt);
    }
    wr(REG_LVT_ERR, VEC_LAPIC_ERROR);
    wr(REG_ESR, 0);
    wr(REG_ESR, 0);
    wr(REG_SVR, 0x100 | VEC_SPURIOUS);   /* software enable */
    lapic_eoi();

    if (c->index == 0) {
        irq_register(VEC_SPURIOUS, on_spurious);
        irq_register(VEC_LAPIC_ERROR, on_error);
        irq_register(VEC_TIMER, on_timer);
    }
}

void lapic_timer_calibrate(void)
{
    use_deadline = cpu_features.tsc_deadline && !cmdline_has("nodeadline");
    if (use_deadline)
        return;
    /* Count APIC timer ticks against the TSC, 5 times, and keep the median.
     * Each run times the exact interval between starting the counter and
     * reading it back, so a delay after the wait (a preempted virtual CPU,
     * an SMI) cannot inflate the result. */
    enum { RUNS = 5 };
    uint64_t rate[RUNS];
    wr(REG_TMR_DIV, 0x3);   /* divide by 16 */
    wr(REG_LVT_TMR, LVT_MASKED);
    for (int r = 0; r < RUNS; r++) {
        uint64_t t0 = rdtsc();
        wr(REG_TMR_INIT, 0xffffffff);
        udelay(10000);
        uint32_t cur = rd(REG_TMR_CUR);
        uint64_t t1 = rdtsc();
        uint64_t elapsed = 0xffffffffu - cur;
        rate[r] = elapsed * tsc_hz / (t1 - t0);
        for (int i = r; i > 0 && rate[i] < rate[i - 1]; i--) {   /* insertion sort */
            uint64_t t = rate[i]; rate[i] = rate[i - 1]; rate[i - 1] = t;
        }
    }
    wr(REG_TMR_INIT, 0);
    apic_ticks_per_sec = (uint32_t)rate[RUNS / 2];
    kprintf("lapic: timer runs at %u Hz (divide 16; runs spread %lu-%lu)\n",
            apic_ticks_per_sec, rate[0], rate[RUNS - 1]);
}

void lapic_timer_start(unsigned hz)
{
    if (use_deadline) {
        tsc_period = tsc_hz / hz;
        wr(REG_LVT_TMR, TMR_TSC_DEADLINE | VEC_TIMER);
        __asm__ volatile("mfence" ::: "memory");   /* SDM: order LVT before MSR */
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + tsc_period);
    } else {
        wr(REG_TMR_DIV, 0x3);
        wr(REG_LVT_TMR, TMR_PERIODIC | VEC_TIMER);
        wr(REG_TMR_INIT, apic_ticks_per_sec / hz);
    }
}

const char *lapic_timer_mode(void)
{
    return use_deadline ? "TSC-deadline" : "APIC periodic";
}
