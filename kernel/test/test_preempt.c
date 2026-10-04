/* A reschedule that becomes pending while preemption is off is taken at
 * the release that turns it back on (kernel/sched/sched.c, preempt_check:
 * its fast path looks at need_resched with one load and only then at the
 * interrupt flag). We wake a higher-priority thread pinned to our own CPU
 * inside a section (it can't run: preemption is off, and the wake marks
 * this CPU need_resched), then end the section: the thread must have run
 * before the next line after the release. The sections: a plain spinlock
 * (interrupts on), an irqsave one (interrupts come back on in the release)
 * and preempt_disable / preempt_enable. With interrupts off as well, the
 * release must not switch (it can't, safely); the reschedule is taken at
 * the next look once they are on. */
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/time.h>

static spinlock_t pr_outer = SPINLOCK_INIT("preempt test outer");
static spinlock_t pr_lock = SPINLOCK_INIT("preempt test");

/* The higher-priority thread: blocks until go, then notes that it ran. */
static struct {
    bool go;    /* pr_lock */
    bool ran;   /* atomic */
} pr;

static void pr_high(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&pr_lock);
    while (!pr.go)
        thread_block(&pr_lock, &f, DEADLINE_NEVER);
    __atomic_store_n(&pr.ran, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&pr_lock, f);
}

/* A thread above us on our CPU, blocked and switched out. */
static struct thread *pr_start(uint32_t cpu)
{
    pr.go = false;
    __atomic_store_n(&pr.ran, false, __ATOMIC_RELAXED);
    cpumask_t m;
    cpumask_one(&m, cpu);
    int prio = __atomic_load_n(&current_thread()->base_prio, __ATOMIC_RELAXED) + 2;
    struct thread *t = thread_create_on("preempt high", pr_high, NULL, prio, &m);
    uint64_t d = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (thread_state(t) != T_BLOCKED || __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);   /* it shares our CPU */
    }
    return t;
}

/* Inside a section: let it go and wake it. It is queued on our CPU, above
 * us: this CPU is marked need_resched, but nothing may switch yet. */
static void pr_wake(struct thread *t)
{
    uint64_t f = spin_lock_irqsave(&pr_lock);
    pr.go = true;
    thread_wake(t);
    spin_unlock_irqrestore(&pr_lock, f);   /* the outer section holds preemption off */
    KT_ASSERT(!__atomic_load_n(&pr.ran, __ATOMIC_ACQUIRE));
}

static bool pr_ran(void)
{
    return __atomic_load_n(&pr.ran, __ATOMIC_ACQUIRE);
}

KTEST(preempt_resched_at_unlock)
{
    uint32_t cpu = kt_pin_self(cpu_count > 1 ? 1 : 0);
    for (int kind = 0; kind < 3; kind++) {
        struct thread *t = pr_start(cpu);
        uint64_t f = 0;
        if (kind == 0)
            spin_lock(&pr_outer);   /* interrupts stay on */
        else if (kind == 1)
            f = spin_lock_irqsave(&pr_outer);
        else
            preempt_disable();
        pr_wake(t);
        if (kind == 0)
            spin_unlock(&pr_outer);
        else if (kind == 1)
            spin_unlock_irqrestore(&pr_outer, f);
        else
            preempt_enable();
        KT_ASSERT(pr_ran());   /* taken at the release itself */
        thread_join(t);
    }
    kt_unpin_self();
}

/* With interrupts off too, the release can't switch: the thread runs at
 * the first look after they are back on. */
KTEST(preempt_resched_waits_for_interrupts)
{
    uint32_t cpu = kt_pin_self(cpu_count > 1 ? 1 : 0);
    struct thread *t = pr_start(cpu);
    uint64_t f = irq_save();
    preempt_disable();
    pr_wake(t);
    preempt_enable();
    KT_ASSERT(!pr_ran());   /* not with interrupts off */
    irq_restore(f);
    preempt_check();
    KT_ASSERT(pr_ran());
    thread_join(t);
    kt_unpin_self();
}
