#include <jam/ipi.h>
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

struct call {
    void (*fn)(void *);
    void *arg;
    volatile uint32_t pending;
};

/* One request per (target CPU, caller) at a time: each target has a small
 * inbox protected by its own lock. */
struct call_slot {
    struct list_node node;
    struct call     *call;
};

struct inbox {
    spinlock_t       lock;
    struct list_node items;
};

static struct inbox inboxes[MAX_CPUS];
volatile int ipi_ready;
static volatile int watchdog_target = -1;
static volatile uint32_t halted;

void ipi_send(uint32_t cpu, uint8_t vector)
{
    cpus[cpu]->ipis++;
    lapic_send_ipi(cpus[cpu]->lapic_id, vector);
}

static void on_reschedule(struct trap_frame *f)
{
    (void)f;
    this_cpu()->need_resched = true;
    lapic_eoi();
}

static void on_call(struct trap_frame *f)
{
    (void)f;
    struct inbox *in = &inboxes[this_cpu()->index];
    lapic_eoi();
    for (;;) {
        spin_lock(&in->lock);   /* IRQs are off in the handler */
        struct call_slot *s = NULL;
        if (!list_empty(&in->items)) {
            s = list_first(&in->items, struct call_slot, node);
            list_del(&s->node);
        }
        spin_unlock(&in->lock);
        if (!s)
            break;
        struct call *c = s->call;   /* s lives on the caller's stack: read first */
        c->fn(c->arg);
        __atomic_sub_fetch(&c->pending, 1, __ATOMIC_RELEASE);
    }
}

void ipi_init(void)
{
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        spin_init(&inboxes[i].lock, "ipi inbox");
        list_init(&inboxes[i].items);
    }
    irq_register(VEC_RESCHEDULE, on_reschedule);
    irq_register(VEC_CALL, on_call);
}

static void check_callable(void)
{
    /* Read per-CPU state with preemption off: irq_depth read preemptibly can
     * belong to another CPU after a migration and panic falsely. A cross-CPU
     * call while holding a spinlock can deadlock against a CPU spinning on
     * that lock with interrupts off, which lockdep cannot see, so refuse it
     * too. */
    preempt_disable();
    struct cpu *c = this_cpu();
    bool bad_irq = !irqs_enabled() || c->irq_depth;
    unsigned held = c->held_depth;
    preempt_enable_no_resched();
    if (bad_irq)
        panic("smp_call with interrupts disabled or from an interrupt handler");
    if (held)
        panic("smp_call while holding %u spinlock(s): a CPU spinning on one with "
              "interrupts off would deadlock", held);
}

static void post(uint32_t cpu, struct call_slot *slot, struct call *c)
{
    slot->call = c;
    struct inbox *in = &inboxes[cpu];
    uint64_t f = spin_lock_irqsave(&in->lock);
    list_add_tail(&in->items, &slot->node);
    spin_unlock_irqrestore(&in->lock, f);
    ipi_send(cpu, VEC_CALL);
}

static void wait_done(struct call *c)
{
    uint64_t start = rdtsc();
    while (__atomic_load_n(&c->pending, __ATOMIC_ACQUIRE)) {
        cpu_relax();
        if (rdtsc() - start > tsc_hz * 5)
            panic("smp_call: %u CPU(s) did not answer in 5 s", c->pending);
    }
}

void smp_call_on(uint32_t cpu, void (*fn)(void *), void *arg)
{
    check_callable();
    preempt_disable();   /* stay on this CPU while we compare indices */
    if (cpu == this_cpu()->index) {
        fn(arg);
        preempt_enable();
        return;
    }
    struct call c = { fn, arg, 1 };
    struct call_slot slot;
    post(cpu, &slot, &c);
    preempt_enable();
    wait_done(&c);
}

/* No memory for one slot per CPU (this runs under system calls, via
 * kernel TLB shootdowns, and must not fail): post from an on-stack array in
 * chunks instead, each chunk answered before the next goes out. Slower on
 * many CPUs, but it never allocates. */
static void call_others_chunked(void (*fn)(void *), void *arg)
{
    enum { CHUNK = 16 };
    struct call_slot slots[CHUNK];
    preempt_disable();
    uint32_t me = this_cpu()->index;
    for (uint32_t next = 0; next < cpu_count;) {
        uint32_t targets[CHUNK], n = 0;
        for (; next < cpu_count && n < CHUNK; next++)
            if (next != me && cpus[next]->online)
                targets[n++] = next;
        if (!n)
            continue;
        struct call c = { fn, arg, n };
        for (uint32_t i = 0; i < n; i++)
            post(targets[i], &slots[i], &c);
        wait_done(&c);
    }
    preempt_enable();
}

void smp_call_others(void (*fn)(void *), void *arg)
{
    check_callable();
    struct call_slot *slots = kmalloc(sizeof(*slots) * cpu_count);
    if (!slots) {
        call_others_chunked(fn, arg);
        return;
    }
    preempt_disable();
    uint32_t me = this_cpu()->index, n = 0;
    struct call c = { fn, arg, 0 };
    for (uint32_t i = 0; i < cpu_count; i++)
        if (i != me && cpus[i]->online)
            n++;
    c.pending = n;
    for (uint32_t i = 0; i < cpu_count; i++)
        if (i != me && cpus[i]->online)
            post(i, &slots[i], &c);
    preempt_enable();
    wait_done(&c);
    kfree(slots);
}

void smp_call_all(void (*fn)(void *), void *arg)
{
    smp_call_others(fn, arg);
    preempt_disable();
    fn(arg);
    preempt_enable();
}

/* ---- TLB shootdown -------------------------------------------------------- */

struct flush_range {
    uint64_t va, len;
};

static void flush_local(void *arg)
{
    struct flush_range *r = arg;
    if (r->len > 64 * PAGE_SIZE) {
        /* Cheaper to flush everything: toggling PGE drops global entries too. */
        uint64_t cr4 = read_cr4();
        write_cr4(cr4 & ~CR4_PGE);
        write_cr4(cr4);
        return;
    }
    for (uint64_t a = r->va; a < r->va + r->len; a += PAGE_SIZE)
        invlpg(a);
}

void tlb_shootdown(uint64_t va, uint64_t len)
{
    if (!ipi_ready)
        return;
    struct flush_range r = { va, len };
    smp_call_others(flush_local, &r);
}

/* Per-CPU count of masked shootdowns handled (for tests: which CPUs an
 * address-space unmap actually interrupted). */
static volatile uint64_t mask_flushes[MAX_CPUS];

static void flush_masked(void *arg)
{
    flush_local(arg);
    __atomic_add_fetch(&mask_flushes[this_cpu()->index], 1, __ATOMIC_RELAXED);
}

/* Flush [va, va+len) on the CPUs in `mask` only (the calling CPU flushes
 * itself if it is in the mask). Used for user address spaces: a CPU not in
 * the address space's active mask has loaded another CR3 since it last
 * used it, which already dropped every non-global entry (and every
 * paging-structure cache entry) it held, so it has nothing to flush.
 *
 * Posts in chunks from an on-stack array so it never allocates (it runs on
 * paths that must not fail). Preemption stays off from the "which CPU am I"
 * decision until every chunk has answered, so the local flush and the
 * remote ones together cover the mask even if the caller would otherwise
 * migrate in between (the rule vmm_unmap follows). */
void tlb_shootdown_mask(const cpumask_t *mask, uint64_t va, uint64_t len)
{
    enum { CHUNK = 16 };
    struct flush_range r = { va, len };
    if (ipi_ready)
        check_callable();
    preempt_disable();
    uint32_t me = this_cpu()->index;
    if (cpumask_has(mask, me))
        flush_masked(&r);
    if (!ipi_ready) {
        preempt_enable();
        return;
    }
    struct call_slot slots[CHUNK];
    uint32_t targets[CHUNK];
    for (uint32_t next = 0; next < cpu_count;) {
        uint32_t n = 0;
        for (; next < cpu_count && n < CHUNK; next++)
            if (next != me && cpumask_has(mask, next) && cpus[next]->online)
                targets[n++] = next;
        if (!n)
            continue;
        struct call c = { flush_masked, &r, n };
        for (uint32_t i = 0; i < n; i++)
            post(targets[i], &slots[i], &c);
        wait_done(&c);
    }
    preempt_enable();
}

uint64_t tlb_mask_flush_count(uint32_t cpu)
{
    return __atomic_load_n(&mask_flushes[cpu], __ATOMIC_RELAXED);
}

/* Flush a range from the CALLING CPU's TLB. The caller keeps preemption off
 * around this and tlb_shootdown so the "current" CPU can't change between the
 * two and escape both flushes (test: repro_unmap_migrate_stale_tlb). */
void tlb_flush_local(uint64_t va, uint64_t len)
{
    struct flush_range r = { va, len };
    flush_local(&r);
}

/* ---- NMIs: panic halt and watchdog ----------------------------------------- */

uint32_t ipi_halt_others(void)
{
    if (!ipi_ready)
        return 0;
    uint32_t others = 0;
    for (uint32_t i = 0; i < cpu_count; i++)
        others += cpus[i]->online && cpus[i] != this_cpu();
    lapic_send_nmi_others();
    uint64_t start = rdtsc();
    while (halted < others && rdtsc() - start < tsc_hz / 10)
        cpu_relax();
    return halted;
}

void watchdog_fire(uint32_t cpu)
{
    if (watchdog_target >= 0)
        return;
    watchdog_target = (int)cpu;
    lapic_send_nmi(cpus[cpu]->lapic_id);
}

extern volatile int panic_in_progress;

void nmi_handler(struct trap_frame *f)
{
    if (panic_in_progress) {
        __atomic_add_fetch(&halted, 1, __ATOMIC_RELEASE);
        halt_forever();
    }
    if (watchdog_target == (int)this_cpu()->index) {
        watchdog_target = -1;
        panic_watchdog(f);
    }
    panic_trap(f);   /* an NMI nobody asked for: usually a hardware error */
}
