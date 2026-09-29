/* M6 Track B: device vectors and interrupt objects.
 *
 * No device is needed: virtual interrupt objects own a real (cpu, vector)
 * like an MSI does, so a fixed IPI with that vector to that CPU takes the
 * same path as the device's message (irq_dispatch -> fire -> signal ->
 * port), and interrupt_fire_virtual calls fire directly from any context
 * (threads, IPI handlers on every CPU). The lock checker is on, so every
 * lock the fire path takes is checked for use from interrupt handlers.
 * The MSI path itself needs Track A's PCI core: interrupt_edu_msi (end of
 * file) is the phase-2 test with QEMU's edu device and skips until then. */
#include <jam/cpu.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/interrupt.h>
#include <jam/interrupt_test.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/time.h>
#include <jam/x86.h>

#define MS 1000000ull
#define IRQ_RIGHTS (RIGHTS_BASIC | RIGHTS_IO)

static void pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
}

static void unpin_self(void)
{
    cpumask_t m;
    cpumask_all(&m);
    thread_set_affinity(current_thread(), &m);
}

static struct thread *spawn_on(uint32_t cpu, const char *name, void (*fn)(void *), void *arg)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    return thread_create_on(name, fn, arg, PRIO_DEFAULT, &m);
}

static struct port *new_port(void)
{
    struct port *p;
    KT_EQ(port_create(&p), OK);
    return p;
}

static struct kobject *new_irq(bool maskable)
{
    struct kobject *o;
    KT_EQ(interrupt_create_virtual_ex(NULL, maskable, &o), OK);
    return o;
}

/* A packet within ms milliseconds, or the status. */
static status_t take(struct port *p, uint64_t ms, struct port_packet *pkt)
{
    return port_wait(p, uptime_ns() + ms * MS, pkt);
}

static bool nothing_queued(struct port *p)
{
    struct port_packet pkt;
    return port_wait(p, 0, &pkt) == ERR_TIMED_OUT;
}

/* A CPU the allocator may target: online, xAPIC-addressable, and not CPU 0
 * unless it is alone. */
static bool vector_cpu(uint32_t i)
{
    uint32_t usable = 0;
    for (uint32_t k = 0; k < cpu_count; k++)
        usable += cpus[k]->online && cpus[k]->lapic_id < 0xff;
    return cpus[i]->online && cpus[i]->lapic_id < 0xff && (i != 0 || usable == 1);
}

/* Wait (bounded) until *v >= want. */
static bool wait_at_least(volatile uint64_t *v, uint64_t want, uint64_t ms)
{
    uint64_t end = uptime_ns() + ms * MS;
    while (__atomic_load_n(v, __ATOMIC_ACQUIRE) < want) {
        if (uptime_ns() > end)
            return false;
        thread_yield();
    }
    return true;
}

/* ---- the vector allocator ---------------------------------------------------- */

static struct vector_cpu_view fv[MAX_CPUS];

/* The PC: 16 P-core threads (0-15), 12 E-cores (16-27). */
static void fake_pc(void)
{
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        fv[i] = (struct vector_cpu_view){
            .type = i < 16 ? CORE_PERFORMANCE : CORE_EFFICIENCY,
            .usable = i < 28,
            .nvec = 0,
        };
}

static uint32_t fake_take(uint32_t n)
{
    uint32_t c = vector_pick_cpu(fv, n);
    if (c != UINT32_MAX)
        fv[c].nvec++;
    return c;
}

KTEST(interrupt_vector_pick_policy_fake_topology)
{
    fake_pc();
    /* E-cores first, spread evenly (fewest vectors, then lowest index). */
    for (uint32_t i = 0; i < 24; i++)
        KT_EQ(fake_take(28), 16 + i % 12);
    /* They take every vector before any P-core gets one... */
    for (uint32_t i = 24; i < 12 * VEC_DEVICE_COUNT; i++) {
        uint32_t c = fake_take(28);
        KT_ASSERT(c >= 16 && c < 28);
    }
    for (uint32_t i = 16; i < 28; i++)
        KT_EQ(fv[i].nvec, VEC_DEVICE_COUNT);
    /* ...then the P-cores, spread, never CPU 0 while others are usable. */
    KT_EQ(fake_take(28), 1);
    KT_EQ(fake_take(28), 2);
    for (uint32_t i = 2; i < 15 * VEC_DEVICE_COUNT; i++) {
        uint32_t c = fake_take(28);
        KT_ASSERT(c >= 1 && c < 16);
    }
    KT_EQ(fake_take(28), UINT32_MAX);   /* full: CPU 0 is not a fallback */
    KT_EQ(fv[0].nvec, 0);

    /* The least-loaded E-core wins even over emptier P-cores; a freed
     * vector brings its CPU back first. */
    fake_pc();
    fv[16].nvec = 5;
    fv[17].nvec = 3;
    for (uint32_t i = 18; i < 28; i++)
        fv[i].nvec = 4;
    KT_EQ(vector_pick_cpu(fv, 28), 17);
    fv[20].nvec = 1;
    KT_EQ(vector_pick_cpu(fv, 28), 20);
    /* An APIC ID past the xAPIC range (usable = false) is never a target. */
    fv[20].usable = false;
    KT_EQ(vector_pick_cpu(fv, 28), 17);

    /* CPU 0 alone (or the only usable one) takes vectors. */
    fake_pc();
    KT_EQ(vector_pick_cpu(fv, 1), 0);
    fv[1].usable = false;
    KT_EQ(vector_pick_cpu(fv, 2), 0);
    fv[0].nvec = VEC_DEVICE_COUNT;
    KT_EQ(vector_pick_cpu(fv, 2), UINT32_MAX);

    /* QEMU's shape: no core types. Round robin over CPUs 1-3. */
    for (uint32_t i = 0; i < 4; i++)
        fv[i] = (struct vector_cpu_view){ .type = CORE_UNKNOWN, .usable = true };
    for (uint32_t i = 0; i < 9; i++)
        KT_EQ(fake_take(4), 1 + i % 3);
}

static void no_op(void *ctx)
{
    (void)ctx;
}

static uint64_t seen[MAX_CPUS][4];   /* vector bitmap per CPU */

/* Allocate until every usable CPU is full (falling over from one CPU to
 * the next as each fills), check nothing is handed out twice, then free. */
KTEST(interrupt_vector_alloc_fill_and_free)
{
    uint32_t base[MAX_CPUS], expect = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        base[i] = vector_count(i);
        if (vector_cpu(i))
            expect += VEC_DEVICE_COUNT - base[i];
    }
    struct pair { uint16_t cpu; uint8_t vec; } *got = kmalloc(sizeof(*got) * (expect + 1));
    KT_ASSERT(got);
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        seen[i][0] = seen[i][1] = seen[i][2] = seen[i][3] = 0;
    uint32_t n = 0;
    status_t st;
    for (;;) {
        uint32_t cpu;
        uint8_t vec;
        st = vector_alloc(no_op, NULL, &cpu, &vec);
        if (st != OK)
            break;
        KT_ASSERT(n < expect);
        KT_ASSERT(vector_cpu(cpu));
        KT_ASSERT(vec >= VEC_DEVICE_FIRST && vec <= VEC_DEVICE_LAST);
        KT_ASSERT(!(seen[cpu][vec / 64] & (1ull << (vec % 64))));
        seen[cpu][vec / 64] |= 1ull << (vec % 64);
        got[n++] = (struct pair){ (uint16_t)cpu, vec };
    }
    KT_EQ(st, ERR_NO_RESOURCES);
    KT_EQ(n, expect);
    for (uint32_t i = 0; i < cpu_count; i++)
        KT_EQ(vector_count(i), vector_cpu(i) ? VEC_DEVICE_COUNT : base[i]);
    for (uint32_t k = 0; k < n; k++)
        vector_free(got[k].cpu, got[k].vec);
    for (uint32_t i = 0; i < cpu_count; i++)
        KT_EQ(vector_count(i), base[i]);
    kfree(got);
    /* The MSI message for a vector: fixed delivery, edge, physical mode. */
    for (uint32_t i = 0; i < cpu_count; i++)
        if (vector_cpu(i))
            KT_EQ(msi_address(i), 0xfee00000ull | (uint64_t)cpus[i]->lapic_id << 12);
    KT_EQ(msi_data(0x45), 0x45);
}

struct hit {
    volatile uint32_t cpu;
    volatile uint64_t n;
};

static void record(void *ctx)
{
    struct hit *h = ctx;
    h->cpu = this_cpu()->index;
    __atomic_add_fetch(&h->n, 1, __ATOMIC_RELEASE);
}

/* A vector is delivered on the CPU it was allocated on, to its owner; one
 * with no owner is counted and EOI'd (the next one still arrives). */
KTEST(interrupt_vector_ipi_delivery_and_unowned)
{
    static struct hit hits[8];
    uint32_t cpus_used[8];
    uint8_t vecs[8];
    for (uint32_t k = 0; k < 8; k++) {
        hits[k].cpu = UINT32_MAX;
        hits[k].n = 0;
        KT_EQ(vector_alloc(record, &hits[k], &cpus_used[k], &vecs[k]), OK);
        ipi_send(cpus_used[k], vecs[k]);
        KT_ASSERT(wait_at_least(&hits[k].n, 1, 1000));
        KT_EQ(hits[k].cpu, cpus_used[k]);
    }
    for (uint32_t k = 0; k < 8; k++)
        KT_EQ(hits[k].n, 1);   /* nobody else's IPI landed on it */
    /* Unowned: free one, then send its vector. */
    uint64_t before = irq_device_unowned;
    vector_free(cpus_used[0], vecs[0]);
    ipi_send(cpus_used[0], vecs[0]);
    KT_ASSERT(wait_at_least(&irq_device_unowned, before + 1, 1000));
    KT_EQ(irq_device_last_unowned, vecs[0]);
    KT_EQ(hits[0].n, 1);
    /* The CPU still takes interrupts after it (the EOI was sent). */
    ipi_send(cpus_used[1], vecs[1]);
    KT_ASSERT(wait_at_least(&hits[1].n, 2, 1000));
    for (uint32_t k = 1; k < 8; k++)
        vector_free(cpus_used[k], vecs[k]);
}

static volatile uint64_t slow_in, slow_out;

static void slow_handler(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&slow_in, 1, __ATOMIC_RELEASE);
    uint64_t end = rdtsc() + tsc_hz / 20;   /* 50 ms with interrupts off */
    while (rdtsc() < end)
        cpu_relax();
    __atomic_store_n(&slow_out, 1, __ATOMIC_RELEASE);
}

/* vector_free on one CPU while the handler runs on another: it must not
 * return before the handler has. */
KTEST(interrupt_vector_free_waits_for_running_handler)
{
    if (cpu_count < 2)
        return;
    pin_self(0);   /* vectors never land on CPU 0 when there are others */
    uint32_t cpu;
    uint8_t vec;
    slow_in = slow_out = 0;
    KT_EQ(vector_alloc(slow_handler, NULL, &cpu, &vec), OK);
    KT_ASSERT(cpu != 0);
    ipi_send(cpu, vec);
    uint64_t end = uptime_ns() + 1000 * MS;
    while (!__atomic_load_n(&slow_in, __ATOMIC_ACQUIRE))
        KT_ASSERT(uptime_ns() < end);
    vector_free(cpu, vec);
    KT_EQ(__atomic_load_n(&slow_out, __ATOMIC_ACQUIRE), 1);
    unpin_self();
}

/* ---- interrupt objects -------------------------------------------------------- */

KTEST(interrupt_coalesces_fires_into_one_packet)
{
    uint64_t live = interrupt_live_count();
    struct kobject *irq = new_irq(false);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 7, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    KT_ASSERT(nothing_queued(p));
    for (int i = 0; i < 3; i++)
        interrupt_fire_virtual(irq);
    struct port_packet pkt;
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.key, 7);
    KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
    KT_EQ(pkt.signal.trigger, SIG_INTERRUPT);
    KT_ASSERT(pkt.signal.observed & SIG_INTERRUPT);
    KT_EQ(pkt.signal.count, 3);
    KT_ASSERT(nothing_queued(p));
    KT_EQ(interrupt_fire_count(irq), 3);
    KT_ASSERT(kobject_signals(irq) & SIG_INTERRUPT);
    kobject_unref(&p->base);
    kobject_unref(irq);
    KT_EQ(interrupt_live_count(), live);
}

KTEST(interrupt_ack_rearms_and_loses_nothing)
{
    struct kobject *irq = new_irq(false);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 1, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    struct port_packet pkt;
    interrupt_fire_virtual(irq);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    /* Fired again after the packet was read but before the ack: a new
     * packet, not folded into the signal the ack is about to clear. */
    interrupt_fire_virtual(irq);
    KT_EQ(interrupt_ack(irq), OK);
    KT_EQ(kobject_signals(irq) & SIG_INTERRUPT, 0);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    KT_ASSERT(nothing_queued(p));
    KT_EQ(interrupt_ack(irq), OK);   /* nothing pending: harmless */
    KT_EQ(object_wait_one(irq, SIG_INTERRUPT, uptime_ns(), NULL), ERR_TIMED_OUT);
    /* After the ack the next fire is a new edge. */
    interrupt_fire_virtual(irq);
    KT_EQ(object_wait_one(irq, SIG_INTERRUPT, uptime_ns(), NULL), OK);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    KT_EQ(interrupt_ack(irq), OK);
    /* A ONCE binding takes exactly one fire. */
    struct port *q = new_port();
    KT_EQ(port_bind(q, irq, 2, SIG_INTERRUPT, PORT_BIND_ONCE), OK);
    interrupt_fire_virtual(irq);
    interrupt_fire_virtual(irq);
    KT_EQ(take(q, 0, &pkt), OK);
    KT_EQ(pkt.key, 2);
    KT_ASSERT(nothing_queued(q));
    KT_EQ(take(p, 0, &pkt), OK);   /* the persistent one saw both */
    KT_EQ(pkt.signal.count, 2);
    KT_EQ(interrupt_fire_count(irq), 5);
    kobject_unref(&q->base);
    kobject_unref(&p->base);
    kobject_unref(irq);
}

/* A maskable vector (MSI-X): masked from the first fire until the ack;
 * fires meanwhile are latched once (the pending bit) and delivered by the
 * ack's unmask. */
KTEST(interrupt_masked_until_ack)
{
    struct kobject *irq = new_irq(true);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 3, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    struct port_packet pkt;
    KT_ASSERT(!interrupt_is_masked(irq));
    interrupt_fire_virtual(irq);
    KT_ASSERT(interrupt_is_masked(irq));
    interrupt_fire_virtual(irq);
    interrupt_fire_virtual(irq);
    KT_EQ(interrupt_fire_count(irq), 1);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    KT_ASSERT(nothing_queued(p));
    /* The ack unmasks; the latched fire comes through at once and masks it
     * again. */
    KT_EQ(interrupt_ack(irq), OK);
    KT_EQ(interrupt_fire_count(irq), 2);
    KT_ASSERT(interrupt_is_masked(irq));
    KT_ASSERT(kobject_signals(irq) & SIG_INTERRUPT);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    KT_EQ(interrupt_ack(irq), OK);
    KT_ASSERT(!interrupt_is_masked(irq));
    KT_EQ(kobject_signals(irq) & SIG_INTERRUPT, 0);
    KT_ASSERT(nothing_queued(p));
    interrupt_fire_virtual(irq);
    KT_EQ(take(p, 0, &pkt), OK);
    KT_EQ(pkt.signal.count, 1);
    kobject_unref(&p->base);
    kobject_unref(irq);
}

/* The consumer: reads packets and acks until told to stop. */
struct consumer {
    struct port      *port;
    struct kobject   *irq;
    volatile uint64_t sum, packets;
    volatile bool     stop;
};

static void consume(void *arg)
{
    struct consumer *c = arg;
    while (!c->stop) {
        struct port_packet pkt;
        if (take(c->port, 20, &pkt) != OK)
            continue;
        KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
        KT_ASSERT(pkt.signal.count >= 1);
        KT_EQ(interrupt_ack(c->irq), OK);
        __atomic_add_fetch(&c->packets, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&c->sum, pkt.signal.count, __ATOMIC_RELEASE);
    }
}

#define FIRES_PER_CALL 100

static void fire_burst(void *arg)
{
    for (int i = 0; i < FIRES_PER_CALL; i++)
        interrupt_fire_virtual(arg);
}

/* Fired from IPI handlers (interrupt context) on every other CPU at once
 * and from this thread, while a consumer on another CPU reads and acks:
 * every fire is counted in exactly one packet. */
KTEST(interrupt_concurrent_fires_all_counted)
{
    struct consumer c = { .port = new_port(), .irq = new_irq(false) };
    KT_EQ(port_bind(c.port, c.irq, 5, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    struct thread *t = spawn_on(cpu_count - 1, "irq-consumer", consume, &c);
    pin_self(0);
    uint64_t total = 0;
    for (int round = 0; round < 8; round++) {
        smp_call_others(fire_burst, c.irq);
        total += (uint64_t)FIRES_PER_CALL * (cpu_count - 1);
        fire_burst(c.irq);
        total += FIRES_PER_CALL;
    }
    KT_ASSERT(wait_at_least(&c.sum, total, 5000));
    c.stop = true;
    thread_join(t);
    KT_EQ(c.sum, total);
    KT_EQ(interrupt_fire_count(c.irq), total);
    kprintf("interrupt: %lu fires from %u CPUs in %lu packets\n", total, cpu_count,
            c.packets);
    kobject_unref(&c.port->base);
    kobject_unref(c.irq);
    unpin_self();
}

/* The real vector path wakes a thread blocked in port_wait. */
struct waiter {
    struct port     *port;
    volatile uint64_t got;
    struct port_packet pkt;
};

static void port_waiter(void *arg)
{
    struct waiter *w = arg;
    KT_EQ(take(w->port, 5000, &w->pkt), OK);
    __atomic_store_n(&w->got, 1, __ATOMIC_RELEASE);
}

KTEST(interrupt_vector_wakes_port_waiter)
{
    struct kobject *irq = new_irq(false);
    struct waiter w = { .port = new_port() };
    KT_EQ(port_bind(w.port, irq, 11, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(irq, &cpu, &vec));
    struct thread *t = spawn_on(cpu_count > 2 ? 1 : 0, "irq-waiter", port_waiter, &w);
    thread_sleep_ns(5 * MS);   /* let it block */
    ipi_send(cpu, vec);
    thread_join(t);
    KT_EQ(w.got, 1);
    KT_EQ(w.pkt.key, 11);
    KT_EQ(w.pkt.signal.count, 1);
    KT_EQ(interrupt_fire_count(irq), 1);
    kobject_unref(&w.port->base);
    kobject_unref(irq);
}

struct firer {
    struct kobject   *irq;
    volatile bool    *stop;
    volatile uint64_t n;
};

static void fire_loop(void *arg)
{
    struct firer *f = arg;
    while (!*f->stop) {
        interrupt_fire_virtual(f->irq);
        f->n++;
    }
}

/* Its last handle closes while other CPUs fire it in a loop (holding their
 * own references): it is torn down under them, later fires are only
 * counted, and it is freed when the last of them lets go. */
KTEST(interrupt_close_while_firing)
{
    uint64_t live = interrupt_live_count();
    struct kobject *irq = new_irq(true);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 4, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    uint32_t vcpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(irq, &vcpu, &vec));
    uint32_t vcount = vector_count(vcpu);
    struct handle_table t;
    handle_table_init(&t);
    handle_t h;
    kobject_ref(irq);   /* this test's own */
    struct khandle kh = khandle_from_new(irq, IRQ_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &h), OK);

    volatile bool stop = false;
    uint32_t nf = cpu_count > 1 ? (cpu_count - 1 < 3 ? cpu_count - 1 : 3) : 0;
    struct firer f[3];
    struct thread *th[3];
    for (uint32_t i = 0; i < nf; i++) {
        kobject_ref(irq);
        f[i] = (struct firer){ .irq = irq, .stop = &stop };
        th[i] = spawn_on(1 + i, "irq-firer", fire_loop, &f[i]);
    }
    pin_self(0);
    struct port_packet pkt;
    for (int i = 0; i < 20; i++) {   /* ack under fire meanwhile */
        if (take(p, 5, &pkt) == OK)
            KT_EQ(interrupt_ack(irq), OK);
    }
    handle_table_destroy(&t);   /* last handle: torn down, vector freed */
    KT_ASSERT(!interrupt_vector_of(irq, &vcpu, &vec));
    KT_EQ(vector_count(vcpu), vcount - 1);
    uint64_t fires = interrupt_fire_count(irq);
    KT_EQ(interrupt_ack(irq), ERR_BAD_STATE);
    if (nf) {
        uint64_t late = interrupt_late_fires(irq);
        KT_ASSERT(wait_at_least(&f[0].n, f[0].n + 100, 2000));
        KT_ASSERT(interrupt_late_fires(irq) > late);
    }
    interrupt_fire_virtual(irq);
    KT_EQ(interrupt_fire_count(irq), fires);
    stop = true;
    for (uint32_t i = 0; i < nf; i++) {
        thread_join(th[i]);
        kobject_unref(irq);
    }
    kobject_unref(&p->base);
    KT_EQ(interrupt_live_count(), live + 1);
    kobject_unref(irq);
    KT_EQ(interrupt_live_count(), live);
    unpin_self();
}

/* The real race: the vector keeps arriving (IPIs from another CPU, like a
 * device's MSIs) while the object's last reference goes. Nothing on the
 * interrupt side holds a reference, so destroy must wait until no CPU is
 * in its handler; later arrivals find no owner. A use after free would
 * trip fire()'s magic check. 40 rounds. */
static volatile uint32_t storm_cpu = UINT32_MAX;
static volatile uint8_t storm_vec;
static volatile bool storm_stop;
static volatile uint64_t storm_sent;

static void storm(void *arg)
{
    (void)arg;
    while (!storm_stop) {
        uint32_t c = __atomic_load_n(&storm_cpu, __ATOMIC_ACQUIRE);
        if (c != UINT32_MAX) {
            ipi_send(c, storm_vec);
            storm_sent++;
        }
        for (int i = 0; i < 50; i++)
            cpu_relax();
    }
}

KTEST(interrupt_destroy_under_vector_storm)
{
    if (cpu_count < 3)
        return;
    uint64_t live = interrupt_live_count();
    uint64_t unowned = irq_device_unowned;
    uint64_t ticks[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++)
        ticks[i] = cpus[i]->ticks;
    pin_self(0);
    storm_stop = false;
    storm_cpu = UINT32_MAX;
    struct thread *s = spawn_on(cpu_count - 1, "irq-storm", storm, NULL);
    for (int round = 0; round < 40; round++) {
        struct kobject *irq = new_irq(round & 1);
        struct port *p = new_port();
        KT_EQ(port_bind(p, irq, (uint64_t)round, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
        uint32_t cpu;
        uint8_t vec;
        KT_ASSERT(interrupt_vector_of(irq, &cpu, &vec));
        storm_vec = vec;
        __atomic_store_n(&storm_cpu, cpu, __ATOMIC_RELEASE);
        uint64_t end = uptime_ns() + 1000 * MS;
        while (interrupt_fire_count(irq) < 2) {
            struct port_packet pkt;
            if (take(p, 1, &pkt) == OK)
                KT_EQ(interrupt_ack(irq), OK);
            KT_ASSERT(uptime_ns() < end);
        }
        /* Drop ours, then the port's binding holds the last reference: the
         * port's destroy reaps it, which tears the object down mid-storm. */
        kobject_unref(irq);
        kobject_unref(&p->base);
        KT_EQ(interrupt_live_count(), live);
        /* Keep the storm on the dead vector a moment: it must be unowned. */
        uint64_t sent = storm_sent;
        while (storm_sent < sent + 20)
            thread_yield();
        __atomic_store_n(&storm_cpu, UINT32_MAX, __ATOMIC_RELEASE);
    }
    storm_stop = true;
    thread_join(s);
    KT_ASSERT(irq_device_unowned > unowned);
    /* No CPU was lost: every one still ticks. */
    thread_sleep_ns(50 * MS);
    for (uint32_t i = 0; i < cpu_count; i++)
        KT_ASSERT(cpus[i]->ticks > ticks[i]);
    kprintf("interrupt: storm sent %lu vectors, %lu unowned after teardown\n", storm_sent,
            irq_device_unowned - unowned);
    unpin_self();
}

/* The port goes away with the interrupt's packet still queued, and the
 * other order: the interrupt's last reference is the port's binding. */
KTEST(interrupt_port_closed_with_packet_queued)
{
    uint64_t live = interrupt_live_count();
    struct port_stats before, after;
    port_get_stats(&before);
    struct kobject *irq = new_irq(true);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 6, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    interrupt_fire_virtual(irq);
    interrupt_fire_virtual(irq);
    kobject_unref(&p->base);   /* queued packet and binding go with it */
    interrupt_fire_virtual(irq);   /* no observers now */
    KT_EQ(interrupt_ack(irq), OK);   /* delivers the latched fire to nobody */
    KT_EQ(interrupt_fire_count(irq), 2);
    port_get_stats(&after);
    KT_EQ(after.ports, before.ports);
    KT_EQ(after.bindings, before.bindings);

    p = new_port();
    KT_EQ(port_bind(p, irq, 6, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    struct port_packet pkt;
    KT_EQ(take(p, 0, &pkt), OK);   /* still signalled: bound -> packet at once */
    interrupt_fire_virtual(irq);
    kobject_unref(irq);   /* the binding keeps it */
    KT_EQ(interrupt_live_count(), live + 1);
    kobject_unref(&p->base);
    KT_EQ(interrupt_live_count(), live);
    port_get_stats(&after);
    KT_EQ(after.ports, before.ports);
    KT_EQ(after.bindings, before.bindings);
}

/* One JOB_LIMIT_HANDLES unit from creation until freed; closing the last
 * handle releases the vector even while a reference remains. */
KTEST(interrupt_job_charge)
{
    struct job *j;
    KT_EQ(job_create(NULL, &j), OK);
    uint64_t base = job_used(j, JOB_LIMIT_HANDLES);
    struct kobject *irq;
    KT_EQ(interrupt_create_virtual_ex(j, false, &irq), OK);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), base + 1);
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(irq, &cpu, &vec));
    uint32_t vcount = vector_count(cpu);
    kobject_ref(irq);
    struct handle_table t;
    handle_table_init(&t);
    handle_t h;
    struct khandle kh = khandle_from_new(irq, IRQ_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    interrupt_fire_virtual(irq);
    KT_EQ(sys_interrupt_ack(&t, h), OK);
    handle_table_destroy(&t);
    KT_EQ(vector_count(cpu), vcount - 1);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), base + 1);   /* the struct is still there */
    kobject_unref(irq);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), base);

    /* Over the limit: refused, nothing allocated or charged. */
    uint32_t counts[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++)
        counts[i] = vector_count(i);
    uint64_t live = interrupt_live_count();
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, base), OK);
    KT_EQ(interrupt_create_virtual_ex(j, false, &irq), ERR_NO_RESOURCES);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), base);
    KT_EQ(interrupt_live_count(), live);
    for (uint32_t i = 0; i < cpu_count; i++)
        KT_EQ(vector_count(i), counts[i]);
    job_unref(j);
}

/* The syscalls' error paths (the success path of interrupt_create_msi
 * needs a real RES_PCI_DEV: phase 2), and the argument checks of
 * interrupt_create_msi on made-up functions. */
KTEST(interrupt_syscall_errors)
{
    KT_EQ(sysc_interrupt_ack(1), ERR_BAD_STATE);   /* a kernel thread: no process */
    KT_EQ(sysc_interrupt_create_msi(1, 0, 0, 0), ERR_BAD_STATE);

    struct handle_table t;
    handle_table_init(&t);
    handle_t ev, irqh, weak, out;
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(sys_interrupt_create_msi(&t, 12345, 0, 0, &out), ERR_BAD_HANDLE);
    KT_EQ(sys_interrupt_create_msi(&t, ev, 0, 0, &out), ERR_WRONG_TYPE);
    KT_EQ(sys_interrupt_create_msi(&t, ev, 0, 2, &out), ERR_INVALID_ARGS);
    KT_EQ(sys_interrupt_ack(&t, ev), ERR_WRONG_TYPE);
    KT_EQ(sys_interrupt_ack(&t, 12345), ERR_BAD_HANDLE);
    struct kobject *irq = new_irq(false);
    struct khandle kh = khandle_from_new(irq, IRQ_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &irqh), OK);
    KT_EQ(handle_duplicate(&t, irqh, RIGHTS_BASIC, &weak), OK);
    KT_EQ(sys_interrupt_ack(&t, weak), ERR_ACCESS_DENIED);
    KT_EQ(sys_interrupt_ack(&t, irqh), OK);
    handle_table_destroy(&t);

    uint32_t counts[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++)
        counts[i] = vector_count(i);
    uint64_t live = interrupt_live_count();
    struct kobject *o;
    static struct pci_dev fake;
    fake = (struct pci_dev){ 0 };
    KT_EQ(interrupt_create_msi(NULL, 0, 0, &o), ERR_INVALID_ARGS);
    KT_EQ(interrupt_create_msi(&fake, 0, 4, &o), ERR_INVALID_ARGS);
    KT_EQ(interrupt_create_msi(&fake, 0, 0, &o), ERR_NOT_SUPPORTED);
    KT_EQ(interrupt_create_msi(&fake, 0, IRQ_MSIX, &o), ERR_NOT_SUPPORTED);
    fake.cap_msi = 0x50;
    fake.info.msi_vectors = 1;
    fake.cap_msix = 0x70;
    fake.info.msix_vectors = 4;
    KT_EQ(interrupt_create_msi(&fake, 1, 0, &o), ERR_OUT_OF_RANGE);
    KT_EQ(interrupt_create_msi(&fake, 4, IRQ_MSIX, &o), ERR_OUT_OF_RANGE);
    fake.info.flags = PCI_INFO_DISPLAY;
    KT_EQ(interrupt_create_msi(&fake, 0, 0, &o), ERR_ACCESS_DENIED);
    fake.info.flags = PCI_INFO_BRIDGE;
    KT_EQ(interrupt_create_msi(&fake, 0, IRQ_MSIX, &o), ERR_ACCESS_DENIED);
    fake.info.flags = 0;
    if (pci_count() == 0) {
        /* Track A's stubs refuse to program it: the vector is given back. */
        KT_EQ(interrupt_create_msi(&fake, 0, 0, &o), ERR_NOT_SUPPORTED);
        KT_EQ(interrupt_create_msi(&fake, 3, IRQ_MSIX, &o), ERR_NOT_SUPPORTED);
    }
    KT_EQ(interrupt_live_count(), live);
    for (uint32_t i = 0; i < cpu_count; i++)
        KT_EQ(vector_count(i), counts[i]);
}

/* TODO(phase 2): the MSI path end to end with QEMU's edu device (1234:11e8,
 * MSI, not maskable). Needs Track A's PCI core; skips while pci_find finds
 * no edu (in Track B's tree the PCI core is weak stubs). edu BAR0: 0x00
 * identification (low byte 0xed), 0x24 interrupt status, 0x60 raise (ORs
 * the value into the status and sends the MSI), 0x64 acknowledge (clears
 * those status bits). An MSI is a memory write by the device, so Bus
 * Master Enable must be on. */
KTEST(interrupt_edu_msi)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("interrupt_edu_msi: no edu device (PCI core not merged): skipped\n");
        return;
    }
    KT_ASSERT(d->cap_msi);
    KT_ASSERT(d->info.bar[0].phys && d->info.bar[0].size >= 0x100);
    KT_EQ(pci_enable_memory(d), OK);
    KT_EQ(pci_set_bus_master(d, true), OK);
    volatile uint32_t *regs = vmm_map_mmio(d->info.bar[0].phys, PAGE_SIZE);
    KT_EQ(regs[0] & 0xff, 0xed);
    regs[0x64 / 4] = 0xffffffff;   /* start from a clear status */

    struct kobject *irq;
    KT_EQ(interrupt_create_msi(d, 0, 0, &irq), OK);
    struct kobject *dup;
    KT_EQ(interrupt_create_msi(d, 0, 0, &dup), ERR_ALREADY_BOUND);
    KT_EQ(interrupt_create_msi(d, 0, IRQ_MSIX, &dup), d->cap_msix ? ERR_BAD_STATE
                                                                  : ERR_NOT_SUPPORTED);
    struct port *p = new_port();
    KT_EQ(port_bind(p, irq, 0xed, SIG_INTERRUPT, PORT_BIND_PERSISTENT), OK);
    struct port_packet pkt;
    for (int round = 0; round < 3; round++) {
        uint64_t t0 = uptime_ns();
        regs[0x60 / 4] = 1u << round;
        KT_EQ(take(p, 1000, &pkt), OK);
        uint64_t t1 = uptime_ns();
        KT_EQ(pkt.key, 0xed);
        KT_ASSERT(pkt.signal.count >= 1);
        KT_ASSERT(regs[0x24 / 4] & (1u << round));
        regs[0x64 / 4] = 1u << round;   /* device first, then the object */
        KT_EQ(interrupt_ack(irq), OK);
        kprintf("interrupt_edu_msi: round %d: MSI -> port packet in %lu us\n", round,
                (t1 - t0) / 1000);
    }
    KT_EQ(interrupt_fire_count(irq), 3);
    kobject_unref(&p->base);
    kobject_unref(irq);   /* MSI off at the device, vector freed */
    regs[0x60 / 4] = 1;   /* disabled: must not arrive anywhere */
    thread_sleep_ns(10 * MS);
    regs[0x64 / 4] = 1;
    KT_EQ(pci_set_bus_master(d, false), OK);
}
