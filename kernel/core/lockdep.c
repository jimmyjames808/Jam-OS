/* Spinlock implementation and the lock-order checker.
 *
 * Classes are keyed by (name, subclass). The dependency graph is a 64x64
 * bit matrix: deps[a] has bit b set once class b was taken while a was
 * held. Taking B while holding A is an inversion if A is already reachable
 * from B. The checker's own state is guarded by a raw flag, not a
 * spinlock_t, so it cannot recurse into itself. */
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define MAX_CLASSES 64
#define STUCK_SECONDS 5

struct lock_class {
    const char *name;
    uint8_t     subclass;
    bool        used_in_irq;     /* acquired inside an interrupt handler */
    bool        used_irqs_on;    /* acquired with interrupts enabled */
};

static struct lock_class classes[MAX_CLASSES];
static uint64_t deps[MAX_CLASSES];
static unsigned class_count;
static volatile bool graph_busy;
static volatile bool disabled;

/* Interrupts must be off while the graph flag is held: an interrupt
 * handler that takes any spinlock re-enters the checker, and would spin on
 * the flag its own CPU holds. */
static uint64_t graph_lock(void)
{
    uint64_t f = irq_save();
    while (__atomic_test_and_set(&graph_busy, __ATOMIC_ACQUIRE))
        cpu_relax();
    return f;
}

static void graph_unlock(uint64_t f)
{
    __atomic_clear(&graph_busy, __ATOMIC_RELEASE);
    irq_restore(f);
}

void lockdep_off(void)
{
    disabled = true;
}

static unsigned class_of(spinlock_t *l, unsigned subclass)
{
    const char *name = l->name ? l->name : "(unnamed)";
    if (subclass == 0 && l->cls)
        return l->cls - 1;
    uint64_t f = graph_lock();
    unsigned i;
    for (i = 0; i < class_count; i++)
        if (classes[i].subclass == subclass && !strcmp(classes[i].name, name))
            break;
    if (i == class_count) {
        if (class_count == MAX_CLASSES) {
            graph_unlock(f);
            panic("lockdep: more than %d lock classes", MAX_CLASSES);
        }
        classes[class_count++] = (struct lock_class){ name, (uint8_t)subclass, false, false };
    }
    graph_unlock(f);
    if (subclass == 0)
        l->cls = (uint16_t)(i + 1);
    return i;
}

static bool reachable(unsigned from, unsigned to)
{
    uint64_t seen = 1ull << from, frontier = deps[from];
    while (frontier & ~seen) {
        uint64_t next = 0, fresh = frontier & ~seen;
        seen |= fresh;
        while (fresh) {
            unsigned b = __builtin_ctzll(fresh);
            fresh &= fresh - 1;
            next |= deps[b];
        }
        frontier = next;
    }
    return seen & (1ull << to);
}

static const char *cname(unsigned c)
{
    static char buf[4][48];
    static unsigned slot;
    char *b = buf[slot++ & 3];
    if (classes[c].subclass)
        ksnprintf(b, 48, "%s/%u", classes[c].name, classes[c].subclass);
    else
        ksnprintf(b, 48, "%s", classes[c].name);
    return b;
}

void lockdep_print_held(void)
{
    struct cpu *c = this_cpu();
    kprintf("  cpu %u holds %u lock(s):", c->index, c->held_depth);
    for (unsigned i = 0; i < c->held_depth; i++)
        kprintf(" %s", c->held[i]->name);
    kprintf("\n");
}

static void acquire_checks(spinlock_t *l, unsigned subclass)
{
    if (disabled)
        return;
    struct cpu *c = this_cpu();
    unsigned cls = class_of(l, subclass);

    for (unsigned i = 0; i < c->held_depth; i++) {
        if (c->held[i] == l)
            panic("lockdep: cpu %u takes lock \"%s\" it already holds (self-deadlock)",
                  c->index, l->name);
    }

    /* Interrupt-safety: a class taken inside an interrupt handler must
     * never be taken with interrupts enabled, or the handler can
     * interrupt the holder on the same CPU and spin forever. */
    bool in_irq = c->irq_depth > 0, irqs_on = irqs_enabled();
    uint64_t f = graph_lock();
    struct lock_class *k = &classes[cls];
    if (in_irq)
        k->used_in_irq = true;
    if (irqs_on)
        k->used_irqs_on = true;
    bool irq_bug = k->used_in_irq && k->used_irqs_on;

    /* Ordering: record held -> this, refusing edges that close a cycle. */
    unsigned bad_from = MAX_CLASSES;
    for (unsigned i = 0; i < c->held_depth && bad_from == MAX_CLASSES; i++) {
        unsigned h = c->held_cls[i];
        if (deps[h] & (1ull << cls))
            continue;
        if (h == cls || reachable(cls, h))
            bad_from = h;
        else
            deps[h] |= 1ull << cls;
    }
    graph_unlock(f);

    if (irq_bug)
        panic("lockdep: lock \"%s\" is taken both in interrupt handlers and with "
              "interrupts enabled; use spin_lock_irqsave", cname(cls));
    if (bad_from != MAX_CLASSES) {
        lockdep_print_held();
        panic("lockdep: lock order inversion: taking \"%s\" while holding \"%s\", "
              "but \"%s\" has been taken while holding \"%s\" before (ABBA deadlock)",
              cname(cls), cname(bad_from), cname(bad_from), cname(cls));
    }

    if (c->held_depth == MAX_HELD_LOCKS)
        panic("lockdep: cpu %u holds more than %d locks", c->index, MAX_HELD_LOCKS);
    c->held[c->held_depth] = l;
    c->held_cls[c->held_depth] = (uint8_t)cls;
    c->held_depth++;
}

static void release_checks(spinlock_t *l)
{
    if (disabled)
        return;
    struct cpu *c = this_cpu();
    for (unsigned i = c->held_depth; i-- > 0;) {
        if (c->held[i] != l)
            continue;
        for (unsigned j = i; j + 1 < c->held_depth; j++) {
            c->held[j] = c->held[j + 1];
            c->held_cls[j] = c->held_cls[j + 1];
        }
        c->held_depth--;
        return;
    }
    panic("lockdep: cpu %u releases \"%s\" which it does not hold", c->index, l->name);
}

/* ---- the lock itself ---------------------------------------------------- */

static void wait_turn(spinlock_t *l, uint16_t ticket)
{
    uint64_t start = 0;
    for (uint32_t spins = 0;; spins++) {
        if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) == ticket)
            return;
        cpu_relax();
        if ((spins & 0xffff) == 0 && tsc_hz && !disabled) {
            if (!start)
                start = rdtsc();
            else if (rdtsc() - start > tsc_hz * STUCK_SECONDS) {
                uint16_t h = l->holder;
                panic("spinlock \"%s\" stuck for %d s on cpu %u, held by cpu %d", l->name,
                      STUCK_SECONDS, this_cpu()->index, (int)h - 1);
            }
        }
    }
}

static void lock_common(spinlock_t *l, unsigned subclass)
{
    preempt_disable();
    acquire_checks(l, subclass);   /* before spinning: report, don't hang */
    uint16_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    wait_turn(l, ticket);
    l->holder = (uint16_t)(this_cpu()->index + 1);
}

void spin_lock(spinlock_t *l)
{
    lock_common(l, 0);
}

void spin_lock_nested(spinlock_t *l, unsigned subclass)
{
    lock_common(l, subclass);
}

bool spin_trylock(spinlock_t *l)
{
    preempt_disable();
    uint16_t owner = __atomic_load_n(&l->owner, __ATOMIC_RELAXED);
    uint16_t expect = owner;
    if (!__atomic_compare_exchange_n(&l->next, &expect, (uint16_t)(owner + 1), false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        preempt_enable();
        return false;
    }
    acquire_checks(l, 0);
    l->holder = (uint16_t)(this_cpu()->index + 1);
    return true;
}

static void unlock_common(spinlock_t *l)
{
    release_checks(l);
    l->holder = 0;
    __atomic_store_n(&l->owner, (uint16_t)(l->owner + 1), __ATOMIC_RELEASE);
}

void spin_unlock(spinlock_t *l)
{
    unlock_common(l);
    preempt_enable();
}

void spin_unlock_no_resched(spinlock_t *l)
{
    unlock_common(l);
    preempt_enable_no_resched();
}

void spin_force_unlock(spinlock_t *l)
{
    l->holder = 0;
    __atomic_store_n(&l->owner, l->next, __ATOMIC_RELEASE);
}
