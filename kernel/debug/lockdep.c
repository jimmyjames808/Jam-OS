/* Spinlock implementation and the lock-order checker.
 *
 * Classes are keyed by (name, subclass, sleeping). The dependency graph is
 * a 256x256 bit matrix: deps[a] has bit b set once class b was taken while
 * a was held. Taking B while holding A is an inversion if A is already
 * reachable from B.
 *
 * Fast path: dependency bits and a class's interrupt flags are only ever
 * SET, never cleared, so reading them without a lock is safe: a bit seen
 * set was validated when it was added. Only an acquisition that needs a
 * NEW edge (or a new class) takes the graph lock, rechecks, and runs the
 * cycle search. Once a workload's lock pairs have all been seen, taking a
 * spinlock writes nothing shared. The graph lock is a raw flag, not a
 * spinlock_t, so the checker never recurses into itself.
 *
 * Sleeping locks (mutexes) are tracked per thread, since a thread holds
 * them across sleeps and migrations. Only mutex -> mutex edges are
 * recorded: taking a mutex with a spinlock held is refused outright, so no
 * spinlock -> mutex edge can exist and no cycle can pass through both. */
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define MAX_CLASSES    LOCKDEP_MAX_CLASSES
#define WORDS          (MAX_CLASSES / 64)
#define MAX_SUBCLASSES 8
#define STUCK_SECONDS  5

#define F_IN_IRQ  1u   /* acquired inside an interrupt handler */
#define F_IRQS_ON 2u   /* acquired with interrupts enabled */

struct lock_class {
    const char *name;                  /* the lock's class name */
    uint8_t     subclass;              /* spin_lock_nested's level, 0 = plain */
    bool        sleeping;              /* a mutex class (may sleep) */
    uint8_t     flags;                 /* F_*, set-only */
    uint16_t    sub[MAX_SUBCLASSES];   /* subclass -> class index + 1 */
};

static struct lock_class classes[MAX_CLASSES];
static uint64_t deps[MAX_CLASSES][WORDS];
static unsigned class_count;
static bool graph_busy;   /* the graph lock: __atomic_test_and_set / __atomic_clear */
static bool disabled;     /* lockdep_off: the panic path, no more checks */

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

static bool dep_test(unsigned a, unsigned b)
{
    return __atomic_load_n(&deps[a][b / 64], __ATOMIC_RELAXED) & (1ull << (b % 64));
}

unsigned lockdep_class_count(void)
{
    return __atomic_load_n(&class_count, __ATOMIC_ACQUIRE);
}

void lockdep_off(void)
{
    __atomic_store_n(&disabled, true, __ATOMIC_RELAXED);
}

/* With the graph lock held: find or add (name, subclass, sleeping). */
static unsigned find_or_add_locked(const char *name, unsigned subclass, bool sleeping)
{
    unsigned i;
    for (i = 0; i < class_count; i++)
        if (classes[i].subclass == subclass && classes[i].sleeping == sleeping &&
            !strcmp(classes[i].name, name))
            return i;
    if (class_count == MAX_CLASSES) {
        __atomic_clear(&graph_busy, __ATOMIC_RELEASE);
        panic("lockdep: more than %d lock classes", MAX_CLASSES);
    }
    classes[i] = (struct lock_class){ .name = name, .subclass = (uint8_t)subclass,
                                      .sleeping = sleeping };
    __atomic_store_n(&class_count, i + 1, __ATOMIC_RELEASE);
    return i;
}

/* The class of a lock whose index + 1 is cached in *cache (0 = not yet). */
static unsigned base_class(const char *name, uint16_t *cache, bool sleeping)
{
    uint16_t c = __atomic_load_n(cache, __ATOMIC_RELAXED);
    if (c)
        return c - 1u;
    uint64_t f = graph_lock();
    unsigned i = find_or_add_locked(name, 0, sleeping);
    graph_unlock(f);
    __atomic_store_n(cache, (uint16_t)(i + 1), __ATOMIC_RELAXED);
    return i;
}

static unsigned class_of(spinlock_t *l, unsigned subclass)
{
    const char *name = l->name ? l->name : "(unnamed)";
    unsigned base = base_class(name, &l->cls, false);
    if (subclass == 0)
        return base;
    if (subclass >= MAX_SUBCLASSES)
        panic("lockdep: subclass %u of \"%s\" (max %d)", subclass, name, MAX_SUBCLASSES - 1);
    uint16_t c = __atomic_load_n(&classes[base].sub[subclass], __ATOMIC_ACQUIRE);
    if (c)
        return c - 1u;
    uint64_t f = graph_lock();
    unsigned i = find_or_add_locked(name, subclass, false);
    __atomic_store_n(&classes[base].sub[subclass], (uint16_t)(i + 1), __ATOMIC_RELEASE);
    graph_unlock(f);
    return i;
}

/* One breadth-first step over the dependency graph: every class in
 * frontier not seen yet is marked seen and its dependencies go into next.
 * False if none was new. */
static bool bfs_step(const uint64_t *frontier, uint64_t *seen, uint64_t *next)
{
    bool any = false;
    for (unsigned w = 0; w < WORDS; w++) {
        uint64_t fresh = frontier[w] & ~seen[w];
        seen[w] |= fresh;
        while (fresh) {
            unsigned b = w * 64 + (unsigned)__builtin_ctzll(fresh);
            fresh &= fresh - 1;
            any = true;
            for (unsigned x = 0; x < WORDS; x++)
                next[x] |= deps[b][x];
        }
    }
    return any;
}

/* With the graph lock held: can `to` be reached from `from`? */
static bool reachable(unsigned from, unsigned to)
{
    uint64_t seen[WORDS] = { 0 }, frontier[WORDS];
    seen[from / 64] |= 1ull << (from % 64);
    memcpy(frontier, deps[from], sizeof(frontier));
    for (;;) {
        uint64_t next[WORDS] = { 0 };
        if (!bfs_step(frontier, seen, next))
            break;
        memcpy(frontier, next, sizeof(frontier));
    }
    return seen[to / 64] & (1ull << (to % 64));
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
    struct thread *t = c->current;
    if (t && t->sleep_depth) {
        kprintf("; thread \"%s\" holds mutex(es):", t->name);
        for (unsigned i = 0; i < t->sleep_depth; i++)
            kprintf(" %s", cname(t->sleep_cls[i]));
    }
    kprintf("\n");
}

/* Record held[i] -> cls for every held class, refusing an edge that would
 * close a cycle. Returns the held class that would, or MAX_CLASSES. */
static unsigned add_edges(const uint8_t *held, unsigned n, unsigned cls)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (!dep_test(held[i], cls))
            break;
    if (i == n)
        return MAX_CLASSES;   /* every edge already known: nothing to write */

    unsigned bad = MAX_CLASSES;
    uint64_t f = graph_lock();
    for (; i < n; i++) {
        unsigned h = held[i];
        if (dep_test(h, cls))
            continue;
        if (h == cls || reachable(cls, h)) {
            bad = h;
            break;
        }
        __atomic_or_fetch(&deps[h][cls / 64], 1ull << (cls % 64), __ATOMIC_RELAXED);
    }
    graph_unlock(f);
    return bad;
}

static void inversion(unsigned cls, unsigned bad_from)
{
    lockdep_print_held();
    if (bad_from == cls)
        panic("lockdep: taking a second \"%s\" while holding one; use "
              "spin_lock_nested with a subclass for an ordered pair", cname(cls));
    panic("lockdep: lock order inversion: taking \"%s\" while holding \"%s\", "
          "but \"%s\" has been taken while holding \"%s\" before (ABBA deadlock)",
          cname(cls), cname(bad_from), cname(bad_from), cname(cls));
}

/* The held-lock list is per CPU and updated in two steps (write the slot,
 * bump the depth). An interrupt handler in between would push and pop its
 * own lock through the same slot and leave it behind in ours, so both
 * directions run with interrupts off. (Found by the real-PC stress test:
 * "releases a lock it does not hold" after 230 s on 28 CPUs.) */
static void acquire_checks_locked(spinlock_t *l, unsigned subclass, bool irqs_on);

static void acquire_checks(spinlock_t *l, unsigned subclass)
{
    if (__atomic_load_n(&disabled, __ATOMIC_RELAXED))
        return;
    bool irqs_on = irqs_enabled();   /* as the caller had them */
    uint64_t f = irq_save();
    PATH_COUNT(PATH_LOCK_SLOW);
    acquire_checks_locked(l, subclass, irqs_on);
    irq_restore(f);
}

static void acquire_checks_locked(spinlock_t *l, unsigned subclass, bool irqs_on)
{
    struct cpu *c = this_cpu();
    unsigned cls = class_of(l, subclass);

    for (unsigned i = 0; i < c->held_depth; i++) {
        if (c->held[i] == l)
            panic("lockdep: cpu %u takes lock \"%s\" it already holds (self-deadlock)",
                  c->index, l->name);
    }

    /* Interrupt-safety: a class taken inside an interrupt handler must
     * never be taken with interrupts enabled, or the handler can interrupt
     * the holder on the same CPU and spin forever. Whichever CPU sets the
     * second flag sees both in its atomic OR's result. */
    uint8_t want = (c->irq_depth > 0 ? F_IN_IRQ : 0) | (irqs_on ? F_IRQS_ON : 0);
    uint8_t flags = __atomic_load_n(&classes[cls].flags, __ATOMIC_RELAXED);
    if ((flags & want) != want)
        flags = __atomic_or_fetch(&classes[cls].flags, want, __ATOMIC_SEQ_CST);
    if ((flags & (F_IN_IRQ | F_IRQS_ON)) == (F_IN_IRQ | F_IRQS_ON))
        panic("lockdep: lock \"%s\" is taken both in interrupt handlers and with "
              "interrupts enabled; use spin_lock_irqsave", cname(cls));

    unsigned bad = add_edges(c->held_cls, c->held_depth, cls);
    if (bad != MAX_CLASSES)
        inversion(cls, bad);

    if (c->held_depth == MAX_HELD_LOCKS)
        panic("lockdep: cpu %u holds more than %d locks", c->index, MAX_HELD_LOCKS);
    c->held[c->held_depth] = l;
    c->held_cls[c->held_depth] = (uint8_t)cls;
    c->held_depth++;
}

static void release_checks(const spinlock_t *l)
{
    if (__atomic_load_n(&disabled, __ATOMIC_RELAXED))
        return;
    uint64_t f = irq_save();
    PATH_COUNT(PATH_LOCK_SLOW);
    struct cpu *c = this_cpu();
    for (unsigned i = c->held_depth; i-- > 0;) {
        if (c->held[i] != l)
            continue;
        for (unsigned j = i; j + 1 < c->held_depth; j++) {
            c->held[j] = c->held[j + 1];
            c->held_cls[j] = c->held_cls[j + 1];
        }
        c->held_depth--;
        irq_restore(f);
        return;
    }
    lockdep_print_held();
    panic("lockdep: cpu %u releases \"%s\" which it does not hold", c->index, l->name);
}

/* ---- sleeping locks --------------------------------------------------------- */

void lockdep_sleep_acquire(const void *lock, const char *name, uint16_t *cache)
{
    if (__atomic_load_n(&disabled, __ATOMIC_RELAXED))
        return;
    struct thread *t = current_thread();
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    if (c->irq_depth)
        panic("lockdep: mutex \"%s\" taken in an interrupt handler", name);
    if (c->held_depth) {
        lockdep_print_held();
        panic("lockdep: mutex \"%s\" taken with a spinlock held (it may sleep)", name);
    }
    irq_restore(f);

    unsigned cls = base_class(name, cache, true);
    for (unsigned i = 0; i < t->sleep_depth; i++)
        if (t->sleep_held[i] == lock)
            panic("lockdep: thread \"%s\" takes mutex \"%s\" it already holds", t->name, name);
    unsigned bad = add_edges(t->sleep_cls, t->sleep_depth, cls);
    if (bad != MAX_CLASSES)
        inversion(cls, bad);
    if (t->sleep_depth == MAX_HELD_MUTEXES)
        panic("lockdep: thread \"%s\" holds more than %d mutexes", t->name, MAX_HELD_MUTEXES);
    t->sleep_held[t->sleep_depth] = lock;
    t->sleep_cls[t->sleep_depth] = (uint8_t)cls;
    t->sleep_depth++;
}

void lockdep_sleep_release(const void *lock)
{
    if (__atomic_load_n(&disabled, __ATOMIC_RELAXED))
        return;
    struct thread *t = current_thread();
    for (unsigned i = t->sleep_depth; i-- > 0;) {
        if (t->sleep_held[i] != lock)
            continue;
        for (unsigned j = i; j + 1 < t->sleep_depth; j++) {
            t->sleep_held[j] = t->sleep_held[j + 1];
            t->sleep_cls[j] = t->sleep_cls[j + 1];
        }
        t->sleep_depth--;
        return;
    }
    panic("lockdep: thread \"%s\" releases a mutex it does not hold", t->name);
}

/* ---- the lock itself ---------------------------------------------------- */

static void wait_turn(const spinlock_t *l, uint16_t ticket)
{
    uint64_t start = 0;
    for (uint32_t spins = 0;; spins++) {
        if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) == ticket)
            return;
        cpu_relax();
        if ((spins & 0xffff) == 0 && tsc_hz && !__atomic_load_n(&disabled, __ATOMIC_RELAXED)) {
            if (!start)
                start = rdtsc();
            else if (rdtsc() - start > tsc_hz * STUCK_SECONDS) {
                uint16_t h = __atomic_load_n(&l->holder, __ATOMIC_RELAXED);
                panic("spinlock \"%s\" stuck for %d s on cpu %u, held by cpu %d", l->name,
                      STUCK_SECONDS, this_cpu()->index, (int)h - 1);
            }
        }
    }
}

static void lock_common(spinlock_t *l, unsigned subclass)
{
    preempt_disable();
    PATH_COUNT(PATH_LOCK);
    acquire_checks(l, subclass);   /* before spinning: report, don't hang */
    uint16_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    wait_turn(l, ticket);
    __atomic_store_n(&l->holder, (uint16_t)(this_cpu()->index + 1), __ATOMIC_RELAXED);
}

void spin_lock(spinlock_t *l)
{
    lock_common(l, 0);
}

void spin_lock_nested(spinlock_t *l, unsigned subclass)
{
    lock_common(l, subclass);
}

static void unlock_common(spinlock_t *l)
{
    release_checks(l);
    __atomic_store_n(&l->holder, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&l->owner, (uint16_t)(l->owner + 1), __ATOMIC_RELEASE);   /* ours to write */
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
    __atomic_store_n(&l->holder, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&l->owner, l->next, __ATOMIC_RELEASE);
}
