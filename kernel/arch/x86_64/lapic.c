#include <jam/acpi.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
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
#define REG_ICR_LO  0x300
#define REG_ICR_HI  0x310
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

/* Timer modes. Each CPU's timer interrupt serves two clients: the 100 Hz
 * scheduler tick and the earliest deadline on that CPU's sleeper queue
 * (wait.c). In the two one-shot modes the timer is re-armed after
 * every interrupt for whichever comes first, both kept as absolute TSC
 * values in struct cpu (tick_deadline, timer_deadline):
 *   - TSC-deadline: the CPU has it and "nodeadline" is not given; arming is
 *     one MSR write of the TSC value itself (the PC).
 *   - APIC one-shot: no TSC-deadline (QEMU's TCG, which is how the one-shot
 *     logic gets tested there); arming converts the distance to APIC timer
 *     counts (calibrated like the periodic mode, divide by 16).
 *   - APIC periodic: "nodeadline". A fixed 100 Hz; sleepers are woken by
 *     their CPU's tick, so sleeps have tick (10 ms) resolution.
 * The tick is kept (there is no tickless idle yet). `lapic_oneshot` (boot:
 * "nooneshot") switches the sleeper deadlines off at run time in either
 * one-shot mode: sleepers then wait for their CPU's next tick, which is how
 * the benchmark shows what the one-shot timers buy. */
enum { TMR_MODE_DEADLINE, TMR_MODE_ONESHOT, TMR_MODE_PERIODIC };
static int timer_mode = TMR_MODE_PERIODIC;
static uint32_t apic_ticks_per_sec;   /* with divide-by-16 */
static uint64_t tsc_period;           /* TSC cycles per tick */
volatile bool lapic_oneshot = true;
volatile uint64_t lapic_early_irqs;   /* one-shot interrupts that found nothing due */

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

#define ICR_NMI        (4u << 8)
#define ICR_ASSERT     (1u << 14)
#define ICR_PENDING    (1u << 12)
#define ICR_ALL_BUT_ME (3u << 18)

static void send_icr(uint32_t dest, uint32_t low)
{
    uint64_t f = irq_save();   /* xAPIC: the two ICR writes must not be split */
    /* Per the SDM the x2APIC ICR WRMSR is not a serialising store barrier, so
     * fence first: any store the IPI's target reads (e.g. watchdog_target
     * before an NMI) must be globally visible before the IPI is sent. */
    __asm__ volatile("mfence" ::: "memory");
    if (x2) {
        wrmsr(0x830, (uint64_t)dest << 32 | low);
    } else {
        while (rd(REG_ICR_LO) & ICR_PENDING)
            cpu_relax();
        wr(REG_ICR_HI, dest << 24);
        wr(REG_ICR_LO, low);
    }
    irq_restore(f);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
    send_icr(apic_id, ICR_ASSERT | vector);
}

void lapic_send_ipi_others(uint8_t vector)
{
    send_icr(0, ICR_ASSERT | ICR_ALL_BUT_ME | vector);
}

void lapic_send_nmi(uint32_t apic_id)
{
    send_icr(apic_id, ICR_ASSERT | ICR_NMI);
}

void lapic_send_nmi_others(void)
{
    send_icr(0, ICR_ASSERT | ICR_ALL_BUT_ME | ICR_NMI);
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

/* Interrupts off: arm this CPU's timer for the earlier of its next tick and
 * its sleeper deadline. A deadline already past fires at once. */
static void timer_program(struct cpu *c)
{
    uint64_t when = c->tick_deadline;
    if (lapic_oneshot && c->timer_deadline < when)
        when = c->timer_deadline;
    if (when == c->timer_armed)
        return;
    c->timer_armed = when;
    if (timer_mode == TMR_MODE_DEADLINE) {
        wrmsr(MSR_TSC_DEADLINE, when);
    } else {
        uint64_t now = rdtsc();
        uint64_t d = when > now ? when - now : 0;
        /* Up to the next tick at most, so this can't overflow 32 bits. */
        uint64_t count = d * apic_ticks_per_sec / tsc_hz + 1;
        wr(REG_TMR_INIT, count > 0xffffffffu ? 0xffffffffu : (uint32_t)count);
    }
}

void lapic_timer_set(uint64_t when_tsc)
{
    struct cpu *c = this_cpu();
    c->timer_deadline = when_tsc ? when_tsc : UINT64_MAX;
    if (timer_mode != TMR_MODE_PERIODIC)
        timer_program(c);
}

static void on_timer(struct trap_frame *f)
{
    (void)f;
    struct cpu *c = this_cpu();
    if (timer_mode == TMR_MODE_PERIODIC) {
        c->ticks++;
        lapic_eoi();
        sched_timer_expire();
        sched_tick();
        return;
    }
    c->timer_armed = 0;   /* it fired: the next timer_program must arm again */
    uint64_t now = rdtsc();
    bool tick = now >= c->tick_deadline;
    if (tick) {
        c->ticks++;
        c->tick_deadline += tsc_period;
        if (c->tick_deadline <= now)   /* missed ticks (a long stall): don't catch up */
            c->tick_deadline = now + tsc_period;
    }
    lapic_eoi();
    if (!sched_timer_expire() && !tick)
        lapic_early_irqs++;   /* e.g. the APIC count rounded short: re-armed below */
    if (tick)
        sched_tick();
    timer_program(c);
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
    if (cmdline_has("nodeadline"))
        timer_mode = TMR_MODE_PERIODIC;
    else if (cpu_features.tsc_deadline)
        timer_mode = TMR_MODE_DEADLINE;
    else
        timer_mode = TMR_MODE_ONESHOT;
    lapic_oneshot = !cmdline_has("nooneshot");
    if (timer_mode == TMR_MODE_DEADLINE)
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

/* Every CPU, once, with interrupts off. */
void lapic_timer_start(unsigned hz)
{
    struct cpu *c = this_cpu();
    tsc_period = tsc_hz / hz;
    if (!c->timer_deadline)
        c->timer_deadline = UINT64_MAX;   /* none (struct cpu starts zeroed) */
    c->timer_armed = 0;
    switch (timer_mode) {
    case TMR_MODE_DEADLINE:
        wr(REG_LVT_TMR, TMR_TSC_DEADLINE | VEC_TIMER);
        __asm__ volatile("mfence" ::: "memory");   /* SDM: order LVT before MSR */
        c->tick_deadline = rdtsc() + tsc_period;
        timer_program(c);
        break;
    case TMR_MODE_ONESHOT:
        wr(REG_TMR_DIV, 0x3);
        wr(REG_LVT_TMR, VEC_TIMER);   /* one-shot: counts down once per arming */
        c->tick_deadline = rdtsc() + tsc_period;
        timer_program(c);
        break;
    default:
        wr(REG_TMR_DIV, 0x3);
        wr(REG_LVT_TMR, TMR_PERIODIC | VEC_TIMER);
        wr(REG_TMR_INIT, apic_ticks_per_sec / hz);
        break;
    }
}

const char *lapic_timer_mode(void)
{
    switch (timer_mode) {
    case TMR_MODE_DEADLINE: return lapic_oneshot ? "TSC-deadline, one-shot timers" : "TSC-deadline";
    case TMR_MODE_ONESHOT:  return lapic_oneshot ? "APIC one-shot, one-shot timers" : "APIC one-shot";
    default:                return "APIC periodic";
    }
}

bool lapic_timer_has_oneshot(void)
{
    return timer_mode != TMR_MODE_PERIODIC;
}
