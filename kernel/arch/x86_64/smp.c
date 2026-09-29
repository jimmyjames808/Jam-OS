/* Application processor bring-up. Limine has already woken the APs and
 * parked them; each one is released with its own struct cpu, switches to
 * the kernel's page tables and stack, loads its per-CPU state and starts
 * its timer. */
#include <jam/cpu.h>
#include <jam/ipi.h>
#include <jam/sched.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/report.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define KERNEL_STACK_SZ (64 * 1024)
#define AP_TIMEOUT_US   1000000

_Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg);

struct cpu cpu0;
struct cpu *cpus[MAX_CPUS];
uint32_t cpu_count;
static uint32_t online_count;   /* CPUs up; the APs add themselves (release) */
static const struct boot_cpu *boot_cpu_of[MAX_CPUS];

static struct cpu *new_cpu(const struct boot_cpu *bc, uint32_t index)
{
    /* The BSP keeps the static cpu0 GS has pointed at since kmain began
     * (it may hold live lock-checker state). */
    struct cpu *c = index == 0 ? &cpu0 : kzalloc(sizeof(*c));
    if (!c)
        panic("smp: out of memory");
    c->index = index;
    c->lapic_id = bc->lapic_id;
    c->acpi_uid = bc->acpi_uid;
    c->kstack_top = kstack_alloc(KERNEL_STACK_SZ);
    gdt_init_cpu(c);
    cpus[index] = c;
    boot_cpu_of[index] = bc;
    return c;
}

static void cpu_bringup(struct cpu *c)
{
    percpu_load(c);
    lapic_init_cpu(c);
    cpu_detect_topology(c);
}

void smp_init_bsp(const struct boot_info *bi)
{
    const struct boot_cpu *bsp = NULL;
    for (uint32_t i = 0; i < bi->cpu_count; i++)
        if (bi->cpus[i].lapic_id == bi->bsp_lapic_id)
            bsp = &bi->cpus[i];
    if (!bsp)
        panic("smp: BSP lapic %u not in the CPU list", bi->bsp_lapic_id);

    struct cpu *c = new_cpu(bsp, 0);
    cpu_count = 1;
    cpu_bringup(c);
    if (lapic_id() != c->lapic_id)
        panic("smp: BSP reports lapic %u, loader said %u", lapic_id(), c->lapic_id);
    c->online = true;
    online_count = 1;
}

_Noreturn static void ap_main(void *arg)
{
    struct cpu *c = arg;
    cpu_bringup(c);
    lapic_timer_start(TICK_HZ);
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&online_count, 1, __ATOMIC_RELEASE);
    sched_run_ap_idle();   /* this startup context becomes idle/N */
}

/* Still on the loader's stack and page tables. EFER.NXE must be on before
 * CR3 points at tables that use the NX bit. */
static void ap_entry(void *arg)
{
    struct cpu *c = arg;
    cpu_enable_paging_features();
    write_cr3(vmm_kernel_pml4());
    stack_switch_call(c->kstack_top, ap_main, c);
}

void smp_start_aps(const struct boot_info *bi)
{
    for (uint32_t i = 0; i < bi->cpu_count; i++) {
        if (bi->cpus[i].lapic_id == bi->bsp_lapic_id)
            continue;
        if (cpu_count == MAX_CPUS)
            break;
        new_cpu(&bi->cpus[i], cpu_count++);
    }
    for (uint32_t i = 1; i < cpu_count; i++)
        boot_start_cpu(boot_cpu_of[i], ap_entry, cpus[i]);

    uint64_t start = rdtsc();
    while (__atomic_load_n(&online_count, __ATOMIC_ACQUIRE) < cpu_count &&
           rdtsc() - start < tsc_hz / 1000000 * AP_TIMEOUT_US)
        cpu_relax();

    uint32_t online = __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
    kprintf("smp: %u of %u CPUs online\n", online, cpu_count);
    __atomic_store_n(&ipi_ready, 1, __ATOMIC_RELEASE);
    sched_topology_init();
    heap_percpu_init();
    if (online != cpu_count) {
        for (uint32_t i = 0; i < cpu_count; i++)
            if (!cpus[i]->online)
                kprintf("smp: cpu %u (lapic %u) did not start\n", i, cpus[i]->lapic_id);
        /* Stuck APs may still be running loader code: keep its memory. */
        kprintf("smp: NOT reclaiming loader memory\n");
        return;
    }
    uint64_t freed = 0;
    for (size_t i = 0; i < bi->memmap_count; i++)
        if (bi->memmap[i].type == BOOT_MEM_LOADER_RECLAIMABLE)
            freed += pmm_add_range(bi->memmap[i].base, bi->memmap[i].length);
    kprintf("pmm: reclaimed %lu KiB of loader memory\n", freed / 1024);
}

static const char *type_name(enum core_type t)
{
    switch (t) {
    case CORE_PERFORMANCE: return "P-core";
    case CORE_EFFICIENCY:  return "E-core";
    default:               return "core";
    }
}

bool smp_report(uint64_t window_ms)
{
    /* Snapshot every count before printing anything: printing is slow on
     * a big framebuffer, and a CPU read after N printed lines would get a
     * window N lines longer than the others. */
    static uint64_t before[MAX_CPUS], after[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++)
        before[i] = cpus[i]->ticks;
    uint64_t t0 = rdtsc();
    udelay(window_ms * 1000);
    for (uint32_t i = 0; i < cpu_count; i++)
        after[i] = cpus[i]->ticks;
    uint64_t measured_us = (rdtsc() - t0) / (tsc_hz / 1000000);

    uint64_t expect = TICK_HZ * window_ms / 1000;
    uint32_t p = 0, e = 0, bad = 0;
    bool smt = false;
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        uint64_t n = after[i] - before[i];
        bool ok = n + expect / 10 >= expect && n <= expect + expect / 10;
        if (!ok)
            bad++;
        p += c->type == CORE_PERFORMANCE;
        e += c->type == CORE_EFFICIENCY;
        for (uint32_t j = 0; j < i; j++)
            if (cpus[j]->core_id == c->core_id)
                smt = true;
        kprintf("  cpu %-3u lapic %-4u %-7s core %-3u thread %u  %lu ticks%s\n", c->index,
                c->lapic_id, type_name(c->type), c->core_id, c->smt_id, n, ok ? "" : "  <-- off");
    }
    if (p || e)
        report("topology: %u CPUs: %u P-cores + %u E-cores, Hyper-Threading %s", cpu_count, p, e,
                smt ? "ON" : "off");
    else
        report("topology: %u CPUs, SMT %s", cpu_count, smt ? "on" : "off");
    report("timer: %s at %u Hz, expected ~%lu ticks per CPU in %lu.%03lu ms: %s",
            lapic_timer_mode(), TICK_HZ, expect, measured_us / 1000, measured_us % 1000,
            bad ? "MISMATCH" : "all ok");
    uint64_t errors = __atomic_load_n(&lapic_errors, __ATOMIC_RELAXED);
    uint64_t unexpected = __atomic_load_n(&irq_unexpected, __ATOMIC_RELAXED);
    if (errors || unexpected)
        report("irq: %lu LAPIC errors (last ESR %x), %lu unexpected (last vector %u)",
               errors, __atomic_load_n(&lapic_last_esr, __ATOMIC_RELAXED), unexpected,
               __atomic_load_n(&irq_last_unexpected, __ATOMIC_RELAXED));
    return bad == 0 && !errors;
}
