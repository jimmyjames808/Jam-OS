/* The lock checker's held list (kernel/debug/lockdep.c), which its fast
 * path updates with interrupts on: what an interrupt handler finds halfway
 * through a push or a pop, releases out of order, and the run-time switch
 * with locks held. That the checker still refuses a bad order, a lock
 * taken twice and a class used in and out of interrupts is shown by the
 * crash tests (lockorder, lockself, locknest, lockirq: tools/crash-test.sh),
 * since each one panics. */
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>

/* Classes of their own: the handler test takes them "in an interrupt",
 * so no other code may take them with interrupts on. */
static spinlock_t irq_a = SPINLOCK_INIT("lockdep test irq A");
static spinlock_t irq_b = SPINLOCK_INIT("lockdep test irq B");
static spinlock_t lock_a = SPINLOCK_INIT("lockdep test A");
static spinlock_t lock_b = SPINLOCK_INIT("lockdep test B");

/* A pretend interrupt handler on this CPU (interrupts off): it takes two
 * locks and gives them back, as every handler does. */
static void handler_round(struct cpu *c)
{
    c->irq_depth++;
    spin_lock(&irq_a);
    spin_lock(&irq_b);
    spin_unlock(&irq_b);
    spin_unlock(&irq_a);
    c->irq_depth--;
}

/* The interrupted code is between "depth raised" and "slot filled" (a
 * push), or between "slot emptied" and "depth lowered" (a pop): either
 * way the top slot is empty. The handler skips it, stacks its own locks
 * above it, and leaves the slot and the depth as it found them. */
KTEST(lockdep_handler_sees_an_empty_top_slot)
{
    bool was = lockdep_is_on();
    lockdep_set(true);   /* also on a nolockdep boot */
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    unsigned d0 = c->held_depth;
    bool was_empty = d0 < MAX_HELD_LOCKS - 3 && !c->held[d0] && !c->held_cls[d0];
    unsigned depth_after = 0, cls_after = 1, above_cls = 1;
    const void *above = &irq_a;
    if (was_empty) {
        c->held_depth = d0 + 1;   /* the half-done push */
        handler_round(c);
        depth_after = c->held_depth;
        cls_after = c->held_cls[d0];
        above = c->held[d0 + 1];
        above_cls = c->held_cls[d0 + 1];
        c->held_depth = d0;       /* the push's caller gives up its slot */
    }
    irq_restore(f);
    lockdep_set(was);
    KT_ASSERT(was_empty);
    KT_EQ(depth_after, d0 + 1);
    KT_EQ(cls_after, 0);
    KT_ASSERT(above == NULL);
    KT_EQ(above_cls, 0);
}

/* A release that isn't of the top lock takes the slow way; the list
 * closes up and stays a stack. */
KTEST(lockdep_out_of_order_release)
{
    bool was = lockdep_is_on();
    lockdep_set(true);
    preempt_disable();   /* this CPU's list, read between the steps */
    struct cpu *c = this_cpu();
    unsigned d0 = c->held_depth;
    spin_lock(&lock_a);
    spin_lock(&lock_b);
    spin_unlock(&lock_a);
    unsigned depth_mid = c->held_depth;
    const void *top = c->held[d0], *gone = c->held[d0 + 1];
    unsigned gone_cls = c->held_cls[d0 + 1];
    spin_unlock(&lock_b);
    unsigned depth_end = c->held_depth;
    preempt_enable();
    lockdep_set(was);
    KT_EQ(depth_mid, d0 + 1);
    KT_EQ(depth_end, d0);
    KT_ASSERT(top == &lock_b);
    KT_ASSERT(gone == NULL);
    KT_EQ(gone_cls, 0);
}

/* The switch flips with locks held: a release is checked exactly when its
 * acquisition was, so nothing is left on the list and nothing is popped
 * that was never pushed. */
KTEST(lockdep_switch_with_locks_held)
{
    bool was = lockdep_is_on();
    lockdep_set(true);
    preempt_disable();            /* this CPU's list, read between the steps */
    struct cpu *c = this_cpu();
    unsigned d0 = c->held_depth;
    spin_lock(&lock_a);           /* checked: on the list */
    lockdep_set(false);
    spin_lock(&lock_b);           /* not checked: not on the list */
    unsigned depth_off = c->held_depth;
    spin_unlock(&lock_a);         /* checked when taken: comes off */
    unsigned depth_mid = c->held_depth;
    lockdep_set(true);
    spin_unlock(&lock_b);         /* not checked when taken: nothing to pop */
    unsigned depth_end = c->held_depth;
    preempt_enable();
    lockdep_set(was);
    KT_EQ(depth_off, d0 + 1);
    KT_EQ(depth_mid, d0);
    KT_EQ(depth_end, d0);
}
