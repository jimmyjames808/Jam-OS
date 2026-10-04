/* Spinlock implementation and the lock-order checker.
 *
 * Classes are keyed by (name, subclass, sleeping). The dependency graph is
 * a 256x256 bit matrix: deps[a] has bit b set once class b was taken while
 * a was held. Taking B while holding A is an inversion if A is already
 * reachable from B.
 *
 * Shared state: dependency bits and a class's interrupt flags are only
 * ever SET, never cleared, so reading them without a lock is safe: a bit
 * seen set was validated when it was added. Only an acquisition that needs
 * a NEW edge (or a new class) takes the graph lock, rechecks, and runs the
 * cycle search. Once a workload's lock pairs have all been seen, taking a
 * spinlock writes nothing shared. The graph lock is a raw flag, not a
 * spinlock_t, so the checker never recurses into itself.
 *
 * The held list. Each CPU keeps the spinlocks it holds as a stack
 * (struct cpu's held[], held_cls[], held_depth), and the fast path updates
 * it with interrupts as the caller had them: no pushf/cli/sti pair per
 * lock. An interrupt handler on this CPU may run between any two steps
 * and take locks of its own; it always gives them back before it returns,
 * so it leaves the list as it found it. What it must never see is a slot
 * that holds another lock's leftovers, so:
 *   - every slot at or above the depth is empty (held_cls 0: a class is
 *     stored as class + 1, so class 255 doesn't exist);
 *   - a push raises the depth first and fills the slot after: a handler in
 *     between sees an empty slot, skips it, and pushes above it;
 *   - a pop empties the slot first and lowers the depth after.
 * The order matters: with the slot written before the depth, a handler in
 * between would push its own lock into that same slot and leave it behind
 * in ours ("releases a lock it does not hold", seen on the PC after 230 s
 * of stress on 28 CPUs). A release is almost always of the top lock (one
 * compare); any other release takes the slow way, with interrupts off,
 * and closes the gap.
 *
 * Taking a lock this CPU already holds is caught from the lock's own
 * holder field (no search of the list), whose HOLDER_CHECKED bit also
 * records whether the checker saw the acquisition: a release is checked
 * exactly when its acquisition was, so the checker can be switched off and
 * on at run time (lockdep_set, the benchmark) with locks held anywhere.
 *
 * Sleeping locks (mutexes) are tracked per thread, since a thread holds
 * them across sleeps and migrations. Only mutex -> mutex edges are
 * recorded: taking a mutex with a spinlock held is refused outright, so no
 * spinlock -> mutex edge can exist and no cycle can pass through both. */
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define TABLE          256                    /* rows and columns of the bit matrix */
#define WORDS          (TABLE / 64)
#define MAX_CLASSES    LOCKDEP_MAX_CLASSES    /* 255: class + 1 fits held_cls's byte */
#define NO_CLASS       MAX_CLASSES            /* "none": an empty slot, no bad class */
#define MAX_SUBCLASSES 8
#define STUCK_SECONDS  5

#define F_IN_IRQ  1u   /* acquired inside an interrupt handler */
#define F_IRQS_ON 2u   /* acquired with interrupts enabled */

/* spinlock_t.holder: the holding CPU's index + 1, and whether the checker
 * saw the acquisition (then the release is checked too). */
#define HOLDER_CPU     0x7fffu
#define HOLDER_CHECKED 0x8000u

struct lock_class {
    const char *name;                  /* the lock's class name */
    uint8_t     subclass;              /* spin_lock_nested's level, 0 = plain */
    bool        sleeping;              /* a mutex class (may sleep) */
    uint8_t     flags;                 /* F_*, set-only */
    uint16_t    sub[MAX_SUBCLASSES];   /* subclass -> class index + 1 */
};

static struct lock_class classes[MAX_CLASSES];
static uint64_t deps[TABLE][WORDS];
static unsigned class_count;
static bool graph_busy;          /* the graph lock: __atomic_test_and_set / __atomic_clear */
static bool dead;                /* lockdep_off: the panic path, nothing checked, not even a release */
static bool spin_checks = true;  /* spinlock acquisitions checked (lockdep_set, "nolockdep") */
static bool sleep_checks = true; /* mutexes checked ("nolockdep" turns them off for the boot) */

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
    __atomic_store_n(&dead, true, __ATOMIC_RELAXED);
    __atomic_store_n(&spin_checks, false, __ATOMIC_RELAXED);
    __atomic_store_n(&sleep_checks, false, __ATOMIC_RELAXED);
}

void lockdep_boot(void)
{
    if (!cmdline_has("nolockdep"))
        return;
    __atomic_store_n(&spin_checks, false, __ATOMIC_RELAXED);
    __atomic_store_n(&sleep_checks, false, __ATOMIC_RELAXED);
}

void lockdep_set(bool on)
{
    __atomic_store_n(&spin_checks, on && !__atomic_load_n(&dead, __ATOMIC_RELAXED),
                     __ATOMIC_RELAXED);
}

bool lockdep_is_on(void)
{
    return __atomic_load_n(&spin_checks, __ATOMIC_RELAXED);
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
        kprintf(" %s", c->held[i] ? c->held[i]->name : "(being taken)");
    struct thread *t = c->current;
    if (t && t->sleep_depth) {
        kprintf("; thread \"%s\" holds mutex(es):", t->name);
        for (unsigned i = 0; i < t->sleep_depth; i++)
            kprintf(" %s", cname(t->sleep_cls[i]));
    }
    kprintf("\n");
}

/* Entry i of a list of held classes, or NO_CLASS for an empty slot. A
 * CPU's spinlock list stores class + 1 (`bias` 1: 0 is empty); a thread's
 * mutex list stores the class itself (`bias` 0, never empty). */
static inline unsigned held_at(const uint8_t *held, unsigned i, unsigned bias)
{
    unsigned e = __atomic_load_n(&held[i], __ATOMIC_RELAXED);
    return bias && !e ? NO_CLASS : e - bias;
}

/* The first held class whose edge to cls is not known yet, or n. */
static inline unsigned first_unknown(const uint8_t *held, unsigned n, unsigned bias,
                                     unsigned cls)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned h = held_at(held, i, bias);
        if (h != NO_CLASS && !dep_test(h, cls))
            return i;
    }
    return n;
}

/* With held[i] -> cls not known yet: record it and every later missing
 * edge, refusing one that would close a cycle. Returns the held class
 * that would, or NO_CLASS. Out of line: the fast path never calls it. */
__attribute__((noinline)) static unsigned add_edges_from(const uint8_t *held, unsigned i,
                                                         unsigned n, unsigned bias, unsigned cls)
{
    unsigned bad = NO_CLASS;
    uint64_t f = graph_lock();
    PATH_COUNT(PATH_LOCK_SLOW);
    for (; i < n; i++) {
        unsigned h = held_at(held, i, bias);
        if (h == NO_CLASS || dep_test(h, cls))
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

/* Record held[i] -> cls for every held class: see add_edges_from. When
 * every edge is already known (the usual case) this writes nothing. */
static inline unsigned add_edges(const uint8_t *held, unsigned n, unsigned bias, unsigned cls)
{
    unsigned i = first_unknown(held, n, bias, cls);
    return i == n ? NO_CLASS : add_edges_from(held, i, n, bias, cls);
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

/* Interrupt-safety: a class taken inside an interrupt handler must never
 * be taken with interrupts enabled, or the handler can interrupt the
 * holder on the same CPU and spin forever. Whichever CPU sets the second
 * flag sees both in its atomic OR's result. Whether interrupts are on
 * (pushf) matters only until the class has been seen with them on. */
static void irq_rule(const struct cpu *c, unsigned cls)
{
    uint8_t flags = __atomic_load_n(&classes[cls].flags, __ATOMIC_RELAXED);
    uint8_t want = c->irq_depth ? F_IN_IRQ : 0;
    if (!(flags & F_IRQS_ON) && irqs_enabled())
        want |= F_IRQS_ON;
    if ((flags & want) != want)
        flags = __atomic_or_fetch(&classes[cls].flags, want, __ATOMIC_SEQ_CST);
    if ((flags & (F_IN_IRQ | F_IRQS_ON)) == (F_IN_IRQ | F_IRQS_ON))
        panic("lockdep: lock \"%s\" is taken both in interrupt handlers and with "
              "interrupts enabled; use spin_lock_irqsave", cname(cls));
}

/* Onto this CPU's held list: the depth first, then the slot (the header
 * says why). The signal fences keep the compiler from reordering the
 * steps; an interrupt is the only other reader. */
static void push(struct cpu *c, spinlock_t *l, unsigned cls)
{
    unsigned d = __atomic_load_n(&c->held_depth, __ATOMIC_RELAXED);
    if (d == MAX_HELD_LOCKS)
        panic("lockdep: cpu %u holds more than %d locks", c->index, MAX_HELD_LOCKS);
    __atomic_store_n(&c->held_depth, d + 1, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&c->held[d], l, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&c->held_cls[d], (uint8_t)(cls + 1), __ATOMIC_RELAXED);
}

/* The checks for one acquisition, before it spins (report, don't hang),
 * then the push. `me` is this CPU's index + 1. */
static void acquire_checks(struct cpu *c, spinlock_t *l, unsigned subclass, uint16_t me)
{
    if ((__atomic_load_n(&l->holder, __ATOMIC_RELAXED) & HOLDER_CPU) == me)
        panic("lockdep: cpu %u takes lock \"%s\" it already holds (self-deadlock)",
              c->index, l->name);
    unsigned cls = class_of(l, subclass);
    irq_rule(c, cls);
    unsigned bad = add_edges(c->held_cls, __atomic_load_n(&c->held_depth, __ATOMIC_RELAXED), 1,
                             cls);
    if (bad != NO_CLASS)
        inversion(cls, bad);
    push(c, l, cls);
}

/* Any release but the top one: take l out of the middle of the list, with
 * interrupts off while the entries above it move down. */
static void release_slow(struct cpu *c, const spinlock_t *l)
{
    uint64_t f = irq_save();
    PATH_COUNT(PATH_LOCK_SLOW);
    unsigned d = c->held_depth;
    for (unsigned i = d; i-- > 0;) {
        if (c->held[i] != l)
            continue;
        for (unsigned j = i; j + 1 < d; j++) {
            c->held[j] = c->held[j + 1];
            c->held_cls[j] = c->held_cls[j + 1];
        }
        c->held_cls[d - 1] = 0;
        c->held[d - 1] = NULL;
        __atomic_store_n(&c->held_depth, d - 1, __ATOMIC_RELAXED);
        irq_restore(f);
        return;
    }
    lockdep_print_held();
    panic("lockdep: cpu %u releases \"%s\" which it does not hold", c->index, l->name);
}

/* Off this CPU's held list: the slot is emptied before the depth drops. */
static void release_checks(struct cpu *c, const spinlock_t *l)
{
    unsigned d = __atomic_load_n(&c->held_depth, __ATOMIC_RELAXED);
    if (!d || __atomic_load_n(&c->held[d - 1], __ATOMIC_RELAXED) != l) {
        release_slow(c, l);
        return;
    }
    __atomic_store_n(&c->held_cls[d - 1], 0, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&c->held[d - 1], NULL, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&c->held_depth, d - 1, __ATOMIC_RELAXED);
}

/* ---- sleeping locks --------------------------------------------------------- */

void lockdep_sleep_acquire(const void *lock, const char *name, uint16_t *cache)
{
    if (!__atomic_load_n(&sleep_checks, __ATOMIC_RELAXED))
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
    unsigned bad = add_edges(t->sleep_cls, t->sleep_depth, 0, cls);
    if (bad != NO_CLASS)
        inversion(cls, bad);
    if (t->sleep_depth == MAX_HELD_MUTEXES)
        panic("lockdep: thread \"%s\" holds more than %d mutexes", t->name, MAX_HELD_MUTEXES);
    t->sleep_held[t->sleep_depth] = lock;
    t->sleep_cls[t->sleep_depth] = (uint8_t)cls;
    t->sleep_depth++;
}

void lockdep_sleep_release(const void *lock)
{
    if (!__atomic_load_n(&sleep_checks, __ATOMIC_RELAXED))
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
        if ((spins & 0xffff) == 0 && tsc_hz && !__atomic_load_n(&dead, __ATOMIC_RELAXED)) {
            if (!start)
                start = rdtsc();
            else if (rdtsc() - start > tsc_hz * STUCK_SECONDS) {
                uint16_t h = __atomic_load_n(&l->holder, __ATOMIC_RELAXED) & HOLDER_CPU;
                panic("spinlock \"%s\" stuck for %d s on cpu %u, held by cpu %d", l->name,
                      STUCK_SECONDS, this_cpu()->index, (int)h - 1);
            }
        }
    }
}

static void lock_common(spinlock_t *l, unsigned subclass)
{
    percpu_preempt_inc();   /* preempt_disable(), inline: this_cpu() is ours from here */
    PATH_COUNT(PATH_LOCK);
    struct cpu *c = this_cpu();
    uint16_t me = (uint16_t)(c->index + 1);
    if (__atomic_load_n(&spin_checks, __ATOMIC_RELAXED)) {
        acquire_checks(c, l, subclass, me);
        me |= HOLDER_CHECKED;
    }
    uint16_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    wait_turn(l, ticket);
    __atomic_store_n(&l->holder, me, __ATOMIC_RELAXED);
}

void spin_lock(spinlock_t *l)
{
    lock_common(l, 0);
}

void spin_lock_nested(spinlock_t *l, unsigned subclass)
{
    lock_common(l, subclass);
}

/* A release is checked when its acquisition was (HOLDER_CHECKED). One the
 * checker didn't see must still be this CPU's while the checker is on: a
 * release of a lock that is free, or another CPU's, is a bug either way. */
static void unlock_common(spinlock_t *l)
{
    struct cpu *c = this_cpu();   /* the lock holds preemption off */
    uint16_t h = __atomic_load_n(&l->holder, __ATOMIC_RELAXED);
    if (h & HOLDER_CHECKED) {
        if (!__atomic_load_n(&dead, __ATOMIC_RELAXED))
            release_checks(c, l);
    } else if ((h & HOLDER_CPU) != c->index + 1 &&
               __atomic_load_n(&spin_checks, __ATOMIC_RELAXED)) {
        lockdep_print_held();
        panic("lockdep: cpu %u releases \"%s\" which it does not hold", c->index, l->name);
    }
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
