/* Application processor bring-up. The BSP makes a struct cpu (stack,
 * GDT/TSS) for every CPU of the boot list, then starts the APs one of two
 * ways and waits, bounded, for each to come online:
 *   - the kernel's own startup (apboot.c: INIT-SIPI-SIPI through a
 *     real-mode trampoline), the default on every boot, and the only way
 *     after a kexec, which has no loader;
 *   - the boot word `smp=loader`: Limine woke the APs and parked them, and
 *     each is released onto its struct cpu (boot_start_cpu). Kept as a
 *     fallback until the kernel's own startup is signed off on the PC.
 * Either way the AP ends in smp_ap_main on its own kernel stack and the
 * kernel's page tables: its per-CPU state, its APIC, its topology, then
 * the claim, then its timer, then online.
 *
 * The claim. A CPU's start state (start_state[index]) goes from STARTING
 * to ONLINE by the AP, or to ABANDONED by the BSP when it gives up, each
 * with one compare-and-swap, so exactly one side wins. An AP that loses
 * parks itself, and with the kernel's own startup the BSP also sends it
 * INIT. Before its claim an AP writes only its own struct cpu and its own
 * registers, so one that comes late breaks nothing; the BSP never frees
 * the struct cpu of a CPU that did not start. */
#include <jam/acpi.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>
#include "smp_internal.h"

#define KERNEL_STACK_SZ (64 * 1024)
#define AP_TIMEOUT_US   1000000
#define CLAIMED_WAIT_US 100000   /* from its claim to online is a few stores */

_Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg);

struct cpu cpu0;
struct cpu *cpus[MAX_CPUS];
uint32_t cpu_count;
static uint32_t online_count;   /* CPUs up; the APs add themselves (release) */
static const struct boot_cpu *boot_cpu_of[MAX_CPUS];

enum { AP_STARTING, AP_ONLINE, AP_ABANDONED };
static uint32_t start_state[MAX_CPUS];   /* AP_*, by compare-and-swap only */
/* Each AP's firmware state against the BSP's (cpu_match_bsp), written by
 * the AP before it sets online (release), read by the BSP after. */
static uint32_t ap_microcode[MAX_CPUS];
static uint8_t  ap_diff[MAX_CPUS];

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
    start_state[0] = AP_ONLINE;
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    online_count = 1;
}

/* ---- Test hooks (boot words, kernels with tests only) ------------------ */

#ifndef JAM_NO_KTESTS
/* smp_test_late=<cpu>: that AP waits before its claim until the BSP has
 * given up on it, then claims, which must fail. smp_test_skip=<cpu>: that
 * CPU gets no startup IPIs. 0 = off (the BSP is never either). */
static uint32_t test_late, test_skip;
static bool late_refused;   /* the late CPU saw its claim refused */

static void test_read_words(void)
{
    test_late = (uint32_t)cmdline_get_u64("smp_test_late", 0, 0);
    test_skip = (uint32_t)cmdline_get_u64("smp_test_skip", 0, 0);
}

/* On the late AP: hold back until the BSP gave up (bounded: 10 s). */
static void test_late_wait(const struct cpu *c)
{
    if (c->index != test_late)
        return;
    uint64_t end = rdtsc() + 10 * tsc_hz;
    while (__atomic_load_n(&start_state[c->index], __ATOMIC_ACQUIRE) == AP_STARTING &&
           rdtsc() < end)
        cpu_relax();
}

static void test_late_refused(const struct cpu *c)
{
    if (c->index == test_late)
        __atomic_store_n(&late_refused, true, __ATOMIC_RELEASE);
}

/* On the BSP, having given up on the late CPU: let it find out before it
 * is stopped, so the refused claim is what the test sees (bounded: 1 s). */
static void test_late_seen(const struct cpu *c)
{
    if (c->index != test_late)
        return;
    uint64_t end = rdtsc() + tsc_hz;
    while (!__atomic_load_n(&late_refused, __ATOMIC_ACQUIRE) && rdtsc() < end)
        cpu_relax();
    kprintf("smp: test: cpu %u came late and its claim was %s\n", c->index,
            __atomic_load_n(&late_refused, __ATOMIC_ACQUIRE) ? "refused: it parked itself"
                                                            : "NOT seen within 1 s");
}
#else
static const uint32_t test_skip;
static void test_read_words(void) {}
static void test_late_wait(const struct cpu *c) { (void)c; }
static void test_late_refused(const struct cpu *c) { (void)c; }
static void test_late_seen(const struct cpu *c) { (void)c; }
#endif

/* ---- The AP side -------------------------------------------------------- */

_Noreturn static void park(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

_Noreturn void smp_ap_main(struct cpu *c)
{
    cpu_bringup(c);
    test_late_wait(c);
    uint32_t want = AP_STARTING;
    if (!__atomic_compare_exchange_n(&start_state[c->index], &want, AP_ONLINE, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        test_late_refused(c);
        park();   /* the BSP gave up on this CPU (and may send it INIT) */
    }
    uint32_t ucode;
    ap_diff[c->index] = (uint8_t)cpu_match_bsp(&ucode);
    ap_microcode[c->index] = ucode;
    lapic_timer_start(TICK_HZ);
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&online_count, 1, __ATOMIC_RELEASE);
    sched_run_ap_idle();   /* this startup context becomes idle/N */
}

static void loader_ap_main(void *arg)
{
    smp_ap_main(arg);
}

/* Released by the loader: still on its stack and page tables. EFER.NXE
 * must be on before CR3 points at tables that use the NX bit. */
static void loader_ap_entry(void *arg)
{
    struct cpu *c = arg;
    cpu_enable_paging_features();
    write_cr3(vmm_kernel_pml4());
    stack_switch_call(c->kstack_top, loader_ap_main, c);
}

/* ---- The BSP side ------------------------------------------------------- */

/* The CPU list (the loader's, or the previous kernel's after a kexec)
 * against the MADT: say so where they disagree. The list is what is
 * started. */
static void check_madt(const struct boot_info *bi)
{
    for (uint32_t i = 0; i < acpi.cpu_count; i++) {
        if (!acpi.cpus[i].enabled)
            continue;
        bool listed = false;
        for (uint32_t j = 0; j < bi->cpu_count; j++)
            listed |= bi->cpus[j].lapic_id == acpi.cpus[i].apic_id;
        if (!listed)
            kprintf("smp: the MADT has an enabled CPU at lapic %u that the CPU list "
                    "lacks: not started\n", acpi.cpus[i].apic_id);
    }
}

enum how { HOW_OWN, HOW_LOADER, HOW_NONE };

static enum how choose(struct cpu *const *aps, uint32_t n)
{
    bool loader_can = true;
    for (uint32_t i = 0; i < n; i++)
        loader_can &= boot_cpu_of[aps[i]->index]->loader_handle != NULL;
    if (cmdline_has("smp=loader")) {
        if (loader_can)
            return HOW_LOADER;
        kprintf("smp: smp=loader, but the loader did not park every CPU\n");
    }
    if (apboot_prepare(aps, n))
        return HOW_OWN;
    if (loader_can) {
        kprintf("smp: the loader releases the CPUs instead\n");
        return HOW_LOADER;
    }
    kprintf("smp: no way to start the other CPUs: the BSP runs alone\n");
    return HOW_NONE;
}

/* Give up on a CPU that is not online. True if it was stopped; false if
 * it claimed its start meanwhile and is coming online after all. */
static bool give_up(struct cpu *c, enum how how)
{
    uint32_t want = AP_STARTING;
    if (!__atomic_compare_exchange_n(&start_state[c->index], &want, AP_ABANDONED, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return false;
    test_late_seen(c);
    if (how == HOW_OWN)
        apboot_stop(c);
    return true;
}

static bool wait_online(const struct cpu *c, uint64_t us)
{
    uint64_t end = rdtsc() + tsc_hz / 1000000 * us;
    while (!cpu_online(c) && rdtsc() < end)
        cpu_relax();
    return cpu_online(c);
}

/* After the wait: stop every CPU still not online. Returns how many. */
static uint32_t stop_stragglers(enum how how)
{
    uint32_t stopped = 0;
    for (uint32_t i = 1; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        if (cpu_online(c))
            continue;
        if (give_up(c, how)) {
            stopped++;
            kprintf("smp: cpu %u (lapic %u) did not start\n", i, c->lapic_id);
        } else if (!wait_online(c, CLAIMED_WAIT_US)) {
            kprintf("smp: cpu %u (lapic %u) claimed its start but is not online\n", i,
                    c->lapic_id);
        }
    }
    return stopped;
}

/* The firmware state each AP compared with the BSP's. */
static void report_firmware_state(void)
{
    uint32_t bad = 0, mtrr_set = 0;
    for (uint32_t i = 1; i < cpu_count; i++) {
        if (!cpu_online(cpus[i]))
            continue;
        mtrr_set += !!(ap_diff[i] & CPU_DIFF_MTRR_SET);
        if (ap_diff[i] & CPU_DIFF_MICROCODE)
            kprintf("smp: cpu %u runs microcode %x, the BSP %x\n", i, ap_microcode[i],
                    cpu_features.microcode);
        if (ap_diff[i] & CPU_DIFF_MTRR)
            kprintf("smp: cpu %u's MTRRs differ from the BSP's and could not be set\n", i);
        if (ap_diff[i] & CPU_DIFF_TSC_ADJUST)
            kprintf("smp: cpu %u's TSC_ADJUST differs from the BSP's\n", i);
        bad += !!(ap_diff[i] & (CPU_DIFF_MICROCODE | CPU_DIFF_MTRR | CPU_DIFF_TSC_ADJUST));
    }
    if (mtrr_set)
        kprintf("smp: %u CPUs had MTRRs other than the BSP's: set to the BSP's\n", mtrr_set);
    if (!bad && cpu_count > 1)
        kprintf("smp: every CPU matches the BSP%s (microcode %x, MTRRs, TSC_ADJUST)\n",
                mtrr_set ? " now" : "", cpu_features.microcode);
}

static const char *how_name(enum how how)
{
    switch (how) {
    case HOW_OWN:    return "started with INIT-SIPI-SIPI";
    case HOW_LOADER: return "released by the loader";
    default:         return "not started";
    }
}

/* Start the APs and wait for them. Returns the time it took. */
static uint64_t start_and_wait(enum how how)
{
    struct cpu *const *aps = &cpus[1];
    uint32_t n = cpu_count - 1;
    uint64_t t0 = rdtsc();
    if (how == HOW_OWN)
        apboot_kick(aps, n, test_skip);
    else if (how == HOW_LOADER)
        for (uint32_t i = 0; i < n; i++)
            if (aps[i]->index != test_skip)
                boot_start_cpu(boot_cpu_of[aps[i]->index], loader_ap_entry, aps[i]);
    uint64_t end = t0 + tsc_hz / 1000000 * AP_TIMEOUT_US;
    while (how != HOW_NONE && __atomic_load_n(&online_count, __ATOMIC_ACQUIRE) < cpu_count &&
           rdtsc() < end)
        cpu_relax();
    return rdtsc() - t0;
}

void smp_start_aps(const struct boot_info *bi)
{
    check_madt(bi);
    test_read_words();
    for (uint32_t i = 0; i < bi->cpu_count; i++) {
        if (bi->cpus[i].lapic_id == bi->bsp_lapic_id)
            continue;
        if (cpu_count == MAX_CPUS)
            break;
        new_cpu(&bi->cpus[i], cpu_count++);
    }
    enum how how = HOW_NONE;
    uint64_t took = 0;
    if (cpu_count > 1) {
        cpu_snapshot_bsp();
        how = choose(&cpus[1], cpu_count - 1);
        took = start_and_wait(how);
    }
    uint32_t stopped = stop_stragglers(how);
    if (how == HOW_OWN)
        apboot_finish(stopped != 0);

    uint32_t online = __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
    uint64_t us = took / (tsc_hz / 1000000);
    if (cpu_count == 1)
        kprintf("smp: 1 of 1 CPUs online (the CPU list has no other)\n");
    else
        kprintf("smp: %u of %u CPUs online in %lu.%03lu ms (%s)\n", online, cpu_count,
                us / 1000, us % 1000, how_name(how));
    report_firmware_state();
    __atomic_store_n(&ipi_ready, 1, __ATOMIC_RELEASE);
    sched_topology_init();
    heap_percpu_init();
    if (online != cpu_count) {
        /* A CPU that never answered might still be in the loader's code. */
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
        before[i] = cpu_ticks(cpus[i]);
    uint64_t t0 = rdtsc();
    udelay(window_ms * 1000);
    for (uint32_t i = 0; i < cpu_count; i++)
        after[i] = cpu_ticks(cpus[i]);
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
