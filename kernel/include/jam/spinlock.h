/* Ticket spinlocks with lock-order checking.
 *
 * Every lock has a NAME, and the name is its lock class: all locks that
 * share a name are treated as one kind of lock. The checker (core/lockdep.c)
 * records "A was held while B was taken" for every pair of classes it sees,
 * and panics the first time an acquisition would complete a cycle (the
 * pattern behind every ABBA deadlock), even if the deadlock itself never
 * happened on this run. It also catches taking the same lock twice, two
 * locks of one class nested without spin_lock_nested, a class used both
 * from interrupt handlers and with interrupts enabled, and a lock that has
 * been spinning for 5 seconds. Mutexes are checked the same way.
 *
 * Holding a spinlock disables preemption on this CPU. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct spinlock {
    volatile uint16_t next;    /* next ticket to hand out */
    volatile uint16_t owner;   /* ticket now being served */
    volatile uint16_t holder;  /* CPU index + 1 of the holder, 0 if free */
    uint16_t          cls;     /* lock class index + 1, resolved lazily */
    const char       *name;
} spinlock_t;

#define SPINLOCK_INIT(n) { 0, 0, 0, 0, (n) }

static inline void spin_init(spinlock_t *l, const char *name)
{
    *l = (spinlock_t)SPINLOCK_INIT(name);
}

/* Take a pending reschedule if this is a safe point (defined in sched.c);
 * declared here because spin_unlock_irqrestore (inline below) calls it. */
void preempt_check(void);

void spin_lock(spinlock_t *l);
/* For taking two locks of the same class (e.g. two run queues): the
 * second one gets subclass 1 so the checker sees an ordered pair. */
void spin_lock_nested(spinlock_t *l, unsigned subclass);
bool spin_trylock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
/* Release without the preemption check; used by the scheduler itself. */
void spin_unlock_no_resched(spinlock_t *l);

static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f)
{
    if (f & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
}

static inline bool irqs_enabled(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    return f & (1u << 9);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
    /* Drop the lock and the preemption it held, but defer the reschedule
     * check until interrupts are actually restored: preempt_enable() runs
     * with interrupts still off here and would skip it. */
    spin_unlock_no_resched(l);
    irq_restore(f);
    preempt_check();
}

/* Panic only: make the lock free no matter who holds it. */
void spin_force_unlock(spinlock_t *l);

/* Preemption control (per CPU). */
void preempt_disable(void);
void preempt_enable(void);
void preempt_enable_no_resched(void);

/* Turn the checker off (the panic path, where rules no longer matter). */
void lockdep_off(void);
#define LOCKDEP_MAX_CLASSES 256
/* How many lock classes the checker has seen (at most LOCKDEP_MAX_CLASSES). */
unsigned lockdep_class_count(void);
/* Sleeping locks (mutexes): tracked per thread. `cache` holds the lock's
 * class index + 1 once known (0 before). */
void lockdep_sleep_acquire(const void *lock, const char *name, uint16_t *cache);
void lockdep_sleep_release(const void *lock);
/* Print the locks this CPU holds (for panics and assertions). */
void lockdep_print_held(void);
