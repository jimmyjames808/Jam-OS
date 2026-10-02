/* Path statistics (jam/pathstat.h, kernel/test/pathstat.c): the trace's
 * own rules (members only, the window, interrupt handlers left out, one
 * trace at a time), then the counts of today's call path, pinned per call
 * so a change to the path shows up here as a changed number.
 *
 * The per-call numbers are what the path does now (M11.5 stage 0). A stage
 * that removes a copy or a system call changes one of them on purpose:
 * update the number with it and say so in the commit. Counts that a
 * preemption can raise (locks, scheduler passes, switches, FPU and CR3
 * work) are checked only on an idle machine; the rest hold under load. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/syscall_nums.h>

#include "bench_internal.h"

/* count / calls, x100, rounded: 2.00 per call is 200. */
static uint64_t per100(const struct path_result *r, enum path_ev ev)
{
    return r->calls ? (r->count[ev] * 100 + r->calls / 2) / r->calls : 0;
}

static uint64_t sys100(const struct path_result *r, unsigned nr)
{
    return r->calls ? (r->sys[nr] * 100 + r->calls / 2) / r->calls : 0;
}

static spinlock_t test_lock = SPINLOCK_INIT("pathstat test");

static void alloc_one(void)
{
    kfree(kmalloc(32));
}

static bool other_stop;   /* atomic */

/* A thread that is not a member, allocating all the time. */
static void non_member(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&other_stop, __ATOMIC_ACQUIRE)) {
        alloc_one();
        thread_yield();
    }
}

KTEST(pathstat_counts_members_in_window)
{
    KT_ASSERT(path_begin(PATH_MK_YIELD, 1, 1, 1));
    KT_ASSERT(path_add_thread(current_thread()));
    __atomic_store_n(&other_stop, false, __ATOMIC_RELAXED);
    struct thread *other = thread_create("pathstat other", non_member, NULL, PRIO_DEFAULT);
    path_arm();
    alloc_one();                 /* before the window: not counted */
    PATH_MARK(PATH_MK_YIELD);    /* boundary 1: skipped */
    alloc_one();                 /* still skipped */
    PATH_MARK(PATH_MK_YIELD);    /* boundary 2: the window */
    alloc_one();
    alloc_one();
    uint64_t f = spin_lock_irqsave(&test_lock);
    spin_unlock_irqrestore(&test_lock, f);
    thread_sleep_ms(2);          /* the other thread allocates meanwhile */
    PATH_MARK(PATH_MK_YIELD);    /* boundary 3: the window has closed */
    alloc_one();
    KT_ASSERT(path_window_done());
    struct path_result r;
    path_end(&r);
    __atomic_store_n(&other_stop, true, __ATOMIC_RELEASE);
    thread_join(other);
    KT_EQ(r.calls, 1);
    KT_EQ(r.count[PATH_KMALLOC], 2);
    KT_EQ(r.count[PATH_KFREE], 2);
    KT_ASSERT(r.count[PATH_LOCK] >= 1);
    KT_EQ(r.count[PATH_SYSCALL], 0);
    /* The boundary that opened the window and the one that closed it. */
    KT_ASSERT(r.nstamps >= 2);
    KT_EQ(r.stamps[0].mark, PATH_MK_YIELD);
    KT_EQ(r.stamps[r.nstamps - 1].mark, PATH_MK_YIELD);
    KT_ASSERT(r.stamps[r.nstamps - 1].tsc >= r.stamps[0].tsc);
}

KTEST(pathstat_leaves_out_interrupt_handlers)
{
    KT_ASSERT(path_begin(PATH_MK_YIELD, 0, 1, 0));
    KT_ASSERT(path_add_thread(current_thread()));
    path_arm();
    PATH_MARK(PATH_MK_YIELD);   /* opens the window */
    /* Pretend to be inside an interrupt handler on this CPU: what the
     * handler does is not counted, the interrupt itself is. */
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    c->irq_depth++;
    PATH_COUNT(PATH_KMALLOC);
    PATH_COUNT(PATH_IRQ);
    c->irq_depth--;
    irq_restore(f);
    PATH_COUNT(PATH_KMALLOC);
    struct path_result r;
    path_end(&r);
    KT_EQ(r.count[PATH_KMALLOC], 1);
    KT_EQ(r.count[PATH_IRQ], 1);
}

KTEST(pathstat_one_trace_at_a_time)
{
    KT_ASSERT(path_begin(PATH_MK_CALL, 0, 1, 0));
    KT_ASSERT(!path_begin(PATH_MK_CALL, 0, 1, 0));
    KT_EQ(path_mark_cost(), 0);   /* it needs the trace too */
    for (unsigned i = 0; i < PATH_MEMBERS; i++)
        KT_ASSERT(path_add_thread(current_thread()));
    KT_ASSERT(!path_add_thread(current_thread()));
    struct path_result r;
    path_end(&r);
    KT_ASSERT(path_begin(PATH_MK_CALL, 0, 1, 0));
    path_end(&r);
    KT_EQ(r.calls, 0);
}

static int test_cpu(void)
{
    int p, p2;
    bench_cpus(&p, &p2);
    return p;
}

/* Two kernel threads yielding on one CPU: one round trip is two switches,
 * each one scheduler pass taking one lock (the run queue's), nothing else. */
KTEST(pathstat_switch_counts)
{
    struct path_result r;
    KT_ASSERT(bench_path_switch(test_cpu(), 16, &r));
    KT_ASSERT(r.calls > 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 0);
    KT_EQ(per100(&r, PATH_SYSCALL), 0);
    KT_EQ(per100(&r, PATH_FPU_SAVE), 0);
    KT_EQ(per100(&r, PATH_CR3), 0);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_SCHED), 200);
    KT_IDLE_EQ(per100(&r, PATH_LOCK), 200);
    struct path_shape *sh = kmalloc(sizeof(*sh));
    KT_ASSERT(sh);
    bool ok = path_shape_of(&r, PATH_MK_YIELD, sh);
    KT_ASSERT(ok);
    /* lead: yield, sched_in, locked, picked, arch_fpu, arch_done; then the
     * partner from its sched_done, the same six; then the lead's sched_done. */
    KT_IDLE_EQ(sh->nsteps, 14);
    KT_EQ(sh->first[0].mark, PATH_MK_YIELD);
    KT_EQ(sh->first[sh->nsteps].mark, PATH_MK_YIELD);
    kfree(sh);
}

/* channel_call between two kernel threads on one CPU: two messages, each
 * allocated once, copied in and out of it once; no job, no handles. */
KTEST(pathstat_kernel_call_counts)
{
    struct path_result r;
    KT_ASSERT(bench_path_kcall(test_cpu(), 16, &r));
    KT_ASSERT(r.calls > 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 200);
    KT_EQ(per100(&r, PATH_KFREE), 200);
    KT_EQ(per100(&r, PATH_KCOPY), 400);
    KT_EQ(per100(&r, PATH_KCOPY_B), 6400);
    KT_EQ(per100(&r, PATH_SYSCALL), 0);
    KT_EQ(per100(&r, PATH_HANDLE), 0);
    KT_EQ(per100(&r, PATH_JOB), 0);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_SCHED), 200);
    KT_IDLE_EQ(per100(&r, PATH_WAKE), 200);
    KT_IDLE_EQ(per100(&r, PATH_LOCK), 1400);
}

/* utest bench-call against bench-echo, both on one CPU: today's 5 system
 * calls (the call; the server's read, write, a read that finds nothing,
 * and its wait), 5 handle lookups, 2 messages, 2 job charges and 2
 * credits, and per switch one FPU save, one restore and one CR3 load. */
KTEST(pathstat_user_call_counts)
{
    struct path_result r;
    if (!bench_path_ucall("call", test_cpu(), test_cpu(), 16, &r)) {
        KT_SKIP_LIVE("no bin/utest or no trace free");
        KT_ASSERT(!"bench_path_ucall failed");
    }
    KT_ASSERT(r.calls > 0);
    KT_EQ(sys100(&r, SYS_channel_call), 100);
    KT_EQ(sys100(&r, SYS_channel_read), 200);
    KT_EQ(sys100(&r, SYS_channel_write), 100);
    KT_EQ(sys100(&r, SYS_object_wait_one), 100);
    KT_ASSERT(per100(&r, PATH_SYSCALL) >= 500 && per100(&r, PATH_SYSCALL) <= 502);
    KT_EQ(per100(&r, PATH_EMPTY_READ), 100);
    KT_EQ(per100(&r, PATH_KMALLOC), 200);
    KT_EQ(per100(&r, PATH_KFREE), 200);
    KT_EQ(per100(&r, PATH_KCOPY), 400);
    KT_EQ(per100(&r, PATH_HANDLE), 500);
    KT_EQ(per100(&r, PATH_JOB), 400);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_FPU_SAVE), 200);
    KT_IDLE_EQ(per100(&r, PATH_FPU_RESTORE), 200);
    KT_IDLE_EQ(per100(&r, PATH_CR3), 200);
}
