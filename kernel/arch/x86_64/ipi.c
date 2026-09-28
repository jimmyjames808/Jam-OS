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
    if (!irqs_enabled() || this_cpu()->irq_depth)
        panic("smp_call with interrupts disabled or from an interrupt handler");
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

void smp_call_others(void (*fn)(void *), void *arg)
{
    check_callable();
    struct call_slot *slots = kmalloc(sizeof(*slots) * cpu_count);
    if (!slots)
        panic("smp_call: out of memory");
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
