/* Tests for interrupt remapping (kernel/dev/vtd_irq.c, <jam/irq_remap.h>,
 * and its callers: interrupt objects, irq.c's msi_message, the I/O APIC).
 *
 * The pure one runs everywhere: which units may remap, and with which
 * destination width. The rest need remapping on (QEMU's intel-iommu with
 * intremap=on, and the boot word iommu=on: tools/vtd-test.sh) and skip
 * themselves without it; the device ones need QEMU's edu (1234:11e8, MSI,
 * not maskable; its DMA engine) and skip without a free one:
 *   - the units remap, with the table, CFI and EIME as they should be, and
 *     COM1's I/O APIC pin is in remappable format, through an entry that
 *     only the I/O APIC's requester id (from the DMAR table) may use;
 *   - an interrupt object's MSI names its own entry (remappable format,
 *     source validation by the function's requester id), its interrupts
 *     arrive, and the entry is cleared and given back when it closes;
 *   - a device's message naming another function's entry (fault 26h), a
 *     freed entry (22h) or one past the table (21h) raises nothing and is
 *     recorded as an interrupt-remapping fault naming the device;
 *   - a write by edu's DMA engine into the interrupt window raises nothing
 *     and is recorded (see vtd_irq_window_write_blocked for what QEMU 10.0
 *     doesn't model);
 *   - the timer, IPIs and COM1 keep working;
 *   - remapping goes off (irq_remap_off, kexec's step) and on again. */
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/interrupt.h>
#include <jam/interrupt_test.h>
#include <jam/ioapic.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/irq_remap.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vmo.h>
#include <jam/vtd.h>

#include "../dev/vtd_irq.h"

/* Remapping on, or skip the test (return from it). */
#define NEED_REMAP()                                                                   \
    do {                                                                               \
        if (!irq_remap_on() || !vtd_irq_table()) {                                     \
            kprintf("ktest %s: interrupt remapping is off (iommu=on with an "          \
                    "intel-iommu that remaps), skipped\n", ktest_current);             \
            return;                                                                    \
        }                                                                              \
    } while (0)

#define EDU_STATUS  0x24   /* interrupt status */
#define EDU_RAISE   0x60   /* ORs the value into the status and sends the MSI */
#define EDU_ACK     0x64   /* clears those status bits */

#define MSI_CTL_ENABLE 1u
#define SID_NONE       0xffffu   /* QEMU's requester id for a write with none (see below) */

/* A free edu, its registers mapped, memory decode and bus mastering on
 * (an MSI is a memory write by the device). NULL, said, without one. */
static struct pci_dev *edu_up(volatile uint32_t **regs)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return NULL;
    *regs = (volatile uint32_t *)kt_edu_regs(d);
    KT_EQ(pci_set_bus_master(d, true), OK);
    KT_EQ((*regs)[0] & 0xff, 0xed);
    (*regs)[EDU_ACK / 4] = 0xffffffff;
    return d;
}

static uint16_t rid_of(const struct pci_dev *d)
{
    return vtd_ir_rid(d->info.bus, d->info.dev, d->info.fn);
}

/* The entry a remappable MSI address names (VT-d 5.1.2.2). */
static uint32_t msi_index(uint32_t addr)
{
    return ((addr >> 5) & 0x7fff) | ((addr & 4) ? 0x8000 : 0);
}

/* ---- a vector that counts what reaches it --------------------------------------- */

static uint64_t hits;

static void count_hit(void *ctx)
{
    (void)ctx;
    __atomic_add_fetch(&hits, 1, __ATOMIC_RELAXED);
}

static uint64_t hits_now(void)
{
    return __atomic_load_n(&hits, __ATOMIC_RELAXED);
}

/* ---- the faults the units record (kt_vtd_faults_watch) ---------------------------- */

/* An interrupt-remapping fault with `reason` and `index` from requester
 * sid (or from sid_alt). */
struct ir_fault {
    uint32_t reason, index;
    uint16_t sid, sid_alt;
};

static bool ir_fault_is(const struct vtd_fault_rec *r, const void *arg)
{
    const struct ir_fault *w = arg;
    uint16_t s = (uint16_t)VTD_FRCD_SID(r->hi);
    return VTD_FRCD_REASON(r->hi) == w->reason && VTD_FRCD_INDEX(r->lo) == w->index &&
           (s == w->sid || s == w->sid_alt);
}

/* Wait for such a fault; copy it to *out. */
static bool wait_fault(uint32_t reason, uint32_t index, uint16_t sid, uint16_t sid_alt,
                       struct vtd_fault_rec *out)
{
    const struct ir_fault w = { reason, index, sid, sid_alt };
    return kt_vtd_fault_wait(ir_fault_is, &w, out);
}

/* The log line for an interrupt-remapping fault (the probe's corrected
 * decode, 11.4.7.6: the index from FI 63:48, no read/write), as written
 * out by hand. */
static void line_is(const struct vtd_fault_rec *r)
{
    uint32_t sid = (uint32_t)VTD_FRCD_SID(r->hi), reason = (uint32_t)VTD_FRCD_REASON(r->hi);
    char got[192], want[192];
    vtd_fault_line(got, sizeof(got), 0, r->lo, r->hi);
    ksnprintf(want, sizeof(want), "vtd: fault: unit 0: %02x:%02x.%x interrupt, index %lx, "
              "reason %x: %s", sid >> 8, (sid >> 3) & 0x1f, sid & 7, VTD_FRCD_INDEX(r->lo),
              reason, vtd_fault_reason_words(reason));
    KT_ASSERT(!strcmp(got, want));
    kprintf("ktest %s: %s\n", ktest_current, got);
}

/* ---- pure ---------------------------------------------------------------------------- */

KTEST(vtd_irq_check_units_pure)
{
    /* The PC's expected unit (ecap f050da: QI, IR, EIM) and QEMU's
     * (f00f5a with eim=on, f00f4a without). */
    const uint64_t pc = 0xf050da, qemu = 0xf00f5a, qemu_noeim = 0xf00f4a;
    bool eim = false;
    KT_EQ(vtd_irq_check_units(&pc, 1, true, &eim), OK);
    KT_ASSERT(eim);                                        /* x2APIC ids */
    KT_EQ(vtd_irq_check_units(&pc, 1, false, &eim), OK);
    KT_ASSERT(!eim);                                       /* xAPIC mode: 8-bit ids */
    const uint64_t two[2] = { qemu, qemu_noeim };
    eim = true;
    KT_EQ(vtd_irq_check_units(two, 2, true, &eim), OK);
    KT_ASSERT(!eim);                                       /* one without EIM: all 8-bit */
    const uint64_t no_ir[2] = { pc, pc & ~0x8ull }, no_qi = pc & ~0x2ull;
    KT_EQ(vtd_irq_check_units(no_ir, 2, true, &eim), ERR_NOT_SUPPORTED);
    KT_EQ(vtd_irq_check_units(&no_qi, 1, true, &eim), ERR_NOT_SUPPORTED);
    KT_EQ(vtd_irq_check_units(&pc, 0, true, &eim), ERR_NOT_FOUND);
    /* The remappable MSI address decodes back to its entry. */
    struct vtd_ir_msi m;
    KT_EQ(vtd_ir_msi_encode(0x8123, &m), OK);
    KT_EQ(msi_index((uint32_t)m.address), 0x8123);
}

/* ---- the units and the I/O APIC ------------------------------------------------------- */

KTEST(vtd_irq_on_and_ioapic)
{
    NEED_REMAP();
    struct vtd_ir_table *t = vtd_irq_table();
    struct vtd_unit *u[VTD_MAX_UNITS];
    uint32_t n = vtd_irq_units(u, VTD_MAX_UNITS);
    KT_ASSERT(n >= 1);
    bool all_eim = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = vtd_rd32(u[i], VTD_GSTS);
        KT_ASSERT(g & VTD_GSTS_IRES);
        KT_ASSERT(g & VTD_GSTS_IRTPS);
        KT_ASSERT(!(g & VTD_GSTS_CFIS));   /* compatibility format blocked */
        KT_EQ(vtd_rd64(u[i], VTD_IRTA), vtd_ir_irta(t));
        all_eim &= VTD_ECAP_EIM(u[i]->ecap) != 0;
    }
    KT_EQ(t->eim, lapic_x2apic() && all_eim);
    KT_EQ(vtd_ir_irta(t) & 0xf, 9);   /* 2^(9 + 1) = 1024 entries */

    /* COM1: its pin names an entry in remappable format (5.1.5.1). */
    bool com1 = false;
    for (uint32_t i = 0; i < ioapic_route_count(); i++) {
        struct ioapic_route r;
        uint64_t e;
        KT_ASSERT(ioapic_route_get(i, &r, &e));
        if (r.vector != VEC_COM1)
            continue;
        com1 = true;
        KT_ASSERT(r.remap != 0);
        KT_ASSERT(e & (1ull << 48));        /* interrupt format: remappable */
        KT_ASSERT(!(e & (1ull << 16)));     /* unmasked */
        KT_EQ(e & 0xff, VEC_COM1);          /* the IRTE's vector, for the EOI */
        KT_EQ((e >> 49) | ((e >> 11) & 1) << 15, r.remap);
        struct vtd_irte_spec s = { r.apic_id, VEC_COM1, r.level, { 0, 0, 0 } };
        KT_EQ(vtd_irq_ioapic_source(r.ioapic_id, &s.src), OK);
        struct vtd_irte want;
        KT_EQ(vtd_irte_encode(&s, t->eim, &want), OK);
        KT_EQ(t->entries[r.remap].lo, want.lo);
        KT_EQ(t->entries[r.remap].hi, want.hi);
        KT_EQ(t->entries[r.remap].hi >> 16, 1u << 2);   /* SVT 01 (requester id), SQ 00 */
        kprintf("ktest %s: COM1 -> entry %u, validated against %02x:%02x.%x\n", ktest_current,
                r.remap, s.src.sid >> 8, (s.src.sid >> 3) & 0x1f, s.src.sid & 7);
    }
    KT_ASSERT(com1 || ioapic_route_count() == 0);
}

/* ---- interrupt objects ------------------------------------------------------------------ */

KTEST(vtd_irq_msi_through_entry)
{
    NEED_REMAP();
    volatile uint32_t *regs;
    struct pci_dev *d = edu_up(&regs);
    if (!d)
        return;
    struct vtd_ir_table *t = vtd_irq_table();
    uint32_t used = irq_remap_used();
    struct kobject *irq;
    KT_EQ(interrupt_create_msi(d, 0, 0, &irq), OK);
    uint32_t idx = interrupt_remap_of(irq);
    KT_ASSERT(idx != 0 && idx < t->size);
    KT_GLOBAL_EQ(irq_remap_used(), used + 1);

    /* The device's message: remappable, naming idx, data 0 (5.1.5.2). */
    uint32_t c = d->cap_msi;
    uint32_t addr = pci_cfg_read(d, c + 4, 4);
    KT_EQ(addr & 0xfff0001f, 0xfee00010);
    KT_EQ(msi_index(addr), idx);
    KT_EQ(pci_cfg_read(d, c + (d->msi_64 ? 0x0c : 0x08), 2), 0);
    /* Its entry: our (cpu, vector), edge, and only edu's requester id. */
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(irq, &cpu, &vec));
    struct vtd_irte_spec s = { cpus[cpu]->lapic_id, vec, false,
                               vtd_ir_source_device(d->info.bus, d->info.dev, d->info.fn) };
    struct vtd_irte want;
    KT_EQ(vtd_irte_encode(&s, t->eim, &want), OK);
    KT_EQ(t->entries[idx].lo, want.lo);
    KT_EQ(t->entries[idx].hi, (uint64_t)rid_of(d) | 1ull << 18);

    struct port *p;
    KT_EQ(port_create(&p), OK);
    KT_EQ(port_bind(p, irq, 0xed, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    for (int round = 0; round < 3; round++) {
        struct port_packet pkt;
        regs[EDU_RAISE / 4] = 1u << round;
        KT_EQ(port_wait(p, uptime_ns() + kt_patience_ms(1000) * NS_PER_MS, &pkt), OK);
        KT_EQ(pkt.key, 0xed);
        regs[EDU_ACK / 4] = 1u << round;
        KT_EQ(interrupt_ack(irq), OK);
    }
    KT_EQ(interrupt_fire_count(irq), 3);
    kobject_unref(&p->base);
    kobject_unref(irq);
    /* Closed: MSI off at the device, the entry cleared and given back. */
    KT_ASSERT(!(pci_cfg_read(d, c + 2, 2) & MSI_CTL_ENABLE));
    KT_EQ(t->entries[idx].lo, 0);
    KT_EQ(t->entries[idx].hi, 0);
    KT_GLOBAL_EQ(irq_remap_used(), used);
    KT_EQ(pci_set_bus_master(d, false), OK);
}

KTEST(vtd_irq_entries_freed_on_close)
{
    NEED_REMAP();
    volatile uint32_t *regs;
    struct pci_dev *d = edu_up(&regs);
    if (!d)
        return;
    struct vtd_ir_table *t = vtd_irq_table();
    uint32_t used = irq_remap_used();
    for (int i = 0; i < 40; i++) {
        struct kobject *irq, *dup;
        KT_EQ(interrupt_create_msi(d, 0, 0, &irq), OK);
        uint32_t idx = interrupt_remap_of(irq);
        KT_ASSERT(idx != 0 && (t->entries[idx].lo & VTD_IRTE_P));
        /* A refused second object gives its entry back at once. */
        KT_EQ(interrupt_create_msi(d, 0, 0, &dup), ERR_ALREADY_BOUND);
        KT_GLOBAL_EQ(irq_remap_used(), used + 1);
        kobject_unref(irq);
        KT_EQ(t->entries[idx].lo, 0);
        KT_GLOBAL_EQ(irq_remap_used(), used);
    }
    KT_EQ(pci_set_bus_master(d, false), OK);
}

/* edu's MSI pointed (by hand) at addr: raise it, and expect a fault of
 * `reason` naming edu and `index`, with nothing delivered. */
static void raise_refused(struct pci_dev *d, volatile uint32_t *regs, uint64_t addr,
                          uint32_t reason, uint32_t index)
{
    KT_EQ(pci_msi_set(d, false, 0, addr, 0), OK);
    uint64_t h0 = hits_now();
    kt_vtd_faults_watch(true);   /* afresh */
    regs[EDU_RAISE / 4] = 1;
    struct vtd_fault_rec r;
    KT_ASSERT(wait_fault(reason, index, rid_of(d), rid_of(d), &r));
    thread_sleep_ms(5);
    KT_EQ(hits_now(), h0);   /* blocked: nothing reached the vector */
    regs[EDU_ACK / 4] = 1;
    line_is(&r);
}

KTEST(vtd_irq_foreign_and_stale_source_refused)
{
    NEED_REMAP();
    volatile uint32_t *regs;
    struct pci_dev *d = edu_up(&regs);
    if (!d)
        return;
    struct pci_dev *other = NULL;   /* any other function: its entry isn't edu's */
    for (uint32_t i = 0; i < pci_count() && !other; i++)
        if (pci_get(i) != d)
            other = pci_get(i);
    KT_ASSERT(other);
    uint32_t cpu;
    uint8_t vec;
    KT_EQ(vector_alloc(count_hit, NULL, &cpu, &vec), OK);
    uint32_t apic = cpus[cpu]->lapic_id;
    kt_vtd_faults_watch(true);
    KT_EQ(pci_msi_enable(d, false, true), OK);

    /* Control: edu's own entry for the vector delivers. */
    uint32_t mine, data;
    uint64_t addr;
    KT_EQ(irq_remap_alloc_pci(d, apic, vec, &mine, &addr, &data), OK);
    KT_EQ(pci_msi_set(d, false, 0, addr, data), OK);
    uint64_t h0 = hits_now();
    regs[EDU_RAISE / 4] = 1;
    uint64_t until = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (hits_now() == h0 && uptime_ns() < until)
        thread_sleep_ms(1);
    KT_EQ(hits_now(), h0 + 1);
    regs[EDU_ACK / 4] = 1;
    irq_remap_free(mine);

    /* Another function's entry: source validation fails (26h). */
    uint32_t foreign;
    KT_EQ(irq_remap_alloc_pci(other, apic, vec, &foreign, &addr, &data), OK);
    raise_refused(d, regs, addr, 0x26, foreign);
    /* The same entry once freed: not present (22h). */
    irq_remap_free(foreign);
    raise_refused(d, regs, addr, 0x22, foreign);
    /* An index past the table (21h). */
    raise_refused(d, regs, 0xfee00000u | 0x7fffu << 5 | 0x10, 0x21, 0x7fff);

    KT_EQ(pci_msi_enable(d, false, false), OK);
    kt_vtd_faults_watch(false);
    vector_free(cpu, vec);
    KT_EQ(pci_set_bus_master(d, false), OK);
}

/* ---- the interrupt window, written by a device's DMA ---------------------------------------- */

/* edu's buffer (4 bytes: the data of an old-format MSI, our vector) to
 * `addr` in the interrupt window; whether the vector was hit. */
static bool window_write(volatile uint32_t *regs, uint64_t addr)
{
    uint64_t h0 = hits_now();
    kt_vtd_faults_watch(true);   /* afresh */
    KT_ASSERT(kt_edu_dma((volatile uint8_t *)regs, 0, addr, 4, true));
    thread_sleep_ms(10);
    return hits_now() != h0;
}

/* QEMU 10.0 gives a DMA engine's writes (pci_dma_write) no requester id,
 * so a fault from one names SID_NONE where the PC's names the device; and
 * it passes compatibility-format writes through even with remapping on
 * (vtd_interrupt_remap_msi never looks at CFIS or EIME), where VT-d 5.1.4
 * blocks them (fault 25h). So here the window is written in remappable
 * format, which QEMU does check: an entry that is never present (index 0:
 * vtd_ir.c never hands it out) and one past the table. The literal
 * 0xfee00000 write is the PC's check (hda's vtdtest, M11 stage 5): it is
 * made here too, and its outcome only logged. */
KTEST(vtd_irq_window_write_blocked)
{
    NEED_REMAP();
    volatile uint32_t *regs;
    struct pci_dev *d = edu_up(&regs);
    if (!d)
        return;
    KT_EQ(pci_set_bus_master(d, false), OK);   /* the cap below turns it on */
    uint32_t cpu;
    uint8_t vec;
    KT_EQ(vector_alloc(count_hit, NULL, &cpu, &vec), OK);
    struct vmo *v;
    KT_EQ(vmo_create(PAGE_SIZE, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    uint32_t word = vec;
    KT_EQ(vmo_write(v, 0, &word, sizeof(word)), OK);
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    kobject_ref(cap);
    struct khandle kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa, id;
    KT_EQ(vmo_pin(v, cap, 0, PAGE_SIZE, &pa, 1, &id), OK);
    KT_ASSERT(kt_edu_dma((volatile uint8_t *)regs, pa, 0, 4, false));   /* the word into edu's buffer */
    kt_vtd_faults_watch(true);

    struct vtd_fault_rec r;
    KT_ASSERT(!window_write(regs, 0xfee00010));   /* remappable, entry 0 */
    KT_ASSERT(wait_fault(0x22, 0, rid_of(d), SID_NONE, &r));
    KT_ASSERT(!window_write(regs, 0xfee00000u | 0x7fffu << 5 | 0x10));
    KT_ASSERT(wait_fault(0x21, 0x7fff, rid_of(d), SID_NONE, &r));
    line_is(&r);

    uint64_t compat = 0xfee00000ull | (uint64_t)cpus[cpu]->lapic_id << 12;
    bool hit = window_write(regs, compat);
    thread_sleep_ms(20);   /* a fault, if any, reaches the log thread */
    bool fault = kt_vtd_faults_seen() > 0;
    kprintf("ktest %s: old-format write to %lx: %s, %s (VT-d: blocked, fault 25h; QEMU 10.0 "
            "passes it)\n", ktest_current, compat, hit ? "DELIVERED" : "blocked",
            fault ? "fault recorded" : "no fault");

    kt_vtd_faults_watch(false);
    KT_EQ(vmo_unpin(v, cap, id), OK);
    khandle_release(&kh);
    kobject_unref(cap);
    kobject_unref(vmo_kobject(v));
    vector_free(cpu, vec);
}

/* ---- what remapping must not touch --------------------------------------------------------- */

static uint32_t called;

static void count_call(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&called, 1, __ATOMIC_RELAXED);
}

KTEST(vtd_irq_timer_ipis_and_com1_unaffected)
{
    NEED_REMAP();
    uint64_t t0[MAX_CPUS] = { 0 };
    uint32_t online = 0;
    for (uint32_t i = 0; i < cpu_count; i++)
        if (cpus[i] && cpu_online(cpus[i])) {
            t0[i] = __atomic_load_n(&cpus[i]->ticks, __ATOMIC_RELAXED);
            online++;
        }
    thread_sleep_ms(50);
    for (uint32_t i = 0; i < cpu_count; i++)
        if (cpus[i] && cpu_online(cpus[i]))
            KT_ASSERT(__atomic_load_n(&cpus[i]->ticks, __ATOMIC_RELAXED) > t0[i]);
    __atomic_store_n(&called, 0, __ATOMIC_RELAXED);
    smp_call_all(count_call, NULL);   /* an IPI to every other CPU, waited for */
    KT_EQ(__atomic_load_n(&called, __ATOMIC_RELAXED), online);
    /* COM1's transmit interrupt comes through its remapped entry. */
    if (ioapic_route_count() && !serial_irq_broken()) {
        uint64_t s0 = __atomic_load_n(&serial_irqs, __ATOMIC_RELAXED);
        kprintf("ktest %s: a line for COM1's transmit interrupt\n", ktest_current);
        uint64_t until = uptime_ns() + kt_patience_ms(500) * NS_PER_MS;
        while (__atomic_load_n(&serial_irqs, __ATOMIC_RELAXED) == s0 && uptime_ns() < until)
            thread_sleep_ms(1);
        KT_ASSERT(__atomic_load_n(&serial_irqs, __ATOMIC_RELAXED) > s0);
    }
}

/* ---- off (kexec, panic) and on again ------------------------------------------------------- */

static void remap_state_is(bool on)
{
    struct vtd_unit *u[VTD_MAX_UNITS];
    uint32_t n = vtd_irq_units(u, VTD_MAX_UNITS);
    for (uint32_t i = 0; i < n; i++)
        KT_EQ(!!(vtd_rd32(u[i], VTD_GSTS) & VTD_GSTS_IRES), on);
    KT_EQ(irq_remap_on(), on);
    for (uint32_t i = 0; i < ioapic_route_count(); i++) {
        struct ioapic_route r;
        uint64_t e;
        KT_ASSERT(ioapic_route_get(i, &r, &e));
        KT_EQ(!(e & (1ull << 16)), on);   /* masked while off */
        KT_ASSERT(e & (1ull << 48));      /* still the remappable entry */
    }
}

KTEST(vtd_irq_off_and_on_again)
{
    NEED_REMAP();
    KT_SKIP_LIVE("turns interrupt remapping off: every device's interrupts stop meanwhile");
    KT_NEEDS_IDLE("turns interrupt remapping off: every device's interrupts stop meanwhile");
    for (int panic = 0; panic < 2; panic++) {
        uint64_t t0 = uptime_ns();
        irq_remap_off(panic);
        uint64_t ns = uptime_ns() - t0;
        remap_state_is(false);
        KT_EQ(vtd_irq_reenable(), OK);
        remap_state_is(true);
        kprintf("ktest %s: off (%s) in %lu us, on again\n", ktest_current,
                panic ? "panic" : "kexec", (uint64_t)(ns / NS_PER_US));
    }
}
