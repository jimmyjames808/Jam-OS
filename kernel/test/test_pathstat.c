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
#include <jam/channel.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/syscall_nums.h>
#include <jam/uentry.h>

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

/* Hand-offs per call x100 when only the request's wake is handed over
 * (the server answers with a plain write): 1, or 0 with the switch off. */
static uint64_t handoffs(void)
{
    return __atomic_load_n(&sched_handoff, __ATOMIC_RELAXED) ? 100 : 0;
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
    /* The pretend interrupt, and any real one (a tick) that came while
     * the window was open: its handler's work is left out all the same. */
    KT_ASSERT(r.count[PATH_IRQ] >= 1);
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
    KT_EQ(per100(&r, PATH_FPU_CALLED), 0);
    KT_EQ(per100(&r, PATH_CR3), 0);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_SCHED), 200);
    KT_EQ(per100(&r, PATH_HANDOFF), 0);
    KT_IDLE_EQ(per100(&r, PATH_LOCK), 200);
    KT_IDLE_EQ(per100(&r, PATH_LOCK_SLOW), 0);   /* the checker's fast path */
    KT_EQ(per100(&r, PATH_CLOCK), 0);
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
 * copied in and out once; no job, no handles. The request is allocated
 * (the server isn't waiting in a read: it waits on the endpoint, then
 * reads); the reply goes to the waiting caller in the server's slot, so
 * it allocates nothing (channel.c's "Slots"). With the slots off (the
 * benchmark's switch), both are allocated. */
KTEST(pathstat_kernel_call_counts)
{
    struct path_result r;
    bool slots = __atomic_load_n(&channel_slots, __ATOMIC_RELAXED);
    __atomic_store_n(&channel_slots, false, __ATOMIC_RELAXED);
    bool ok = bench_path_kcall(test_cpu(), 16, &r);
    __atomic_store_n(&channel_slots, slots, __ATOMIC_RELAXED);
    KT_ASSERT(ok && r.calls > 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 200);
    KT_EQ(per100(&r, PATH_KFREE), 200);
    if (!slots)
        return;   /* booted with them off: nothing more to compare */
    KT_ASSERT(bench_path_kcall(test_cpu(), 16, &r));
    KT_ASSERT(r.calls > 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 100);
    KT_EQ(per100(&r, PATH_KFREE), 100);
    KT_EQ(per100(&r, PATH_KCOPY), 400);
    KT_EQ(per100(&r, PATH_KCOPY_B), 6400);
    KT_EQ(per100(&r, PATH_SYSCALL), 0);
    KT_EQ(per100(&r, PATH_HANDLE), 0);
    KT_EQ(per100(&r, PATH_JOB), 0);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_SCHED), 200);
    KT_IDLE_EQ(per100(&r, PATH_WAKE), 200);
    /* The request's wake hands the server our CPU (the direct hand-off);
     * the reply's is queued: the server's plain write doesn't block. */
    KT_IDLE_EQ(per100(&r, PATH_HANDOFF), handoffs());
    /* 13 a round trip, less the run queue lock a handed wake never takes.
     * (The caller finds its reply handed over when it wakes, and doesn't
     * take its endpoint's lock again: chan_handed.) */
    KT_IDLE_EQ(per100(&r, PATH_LOCK), 1300 - handoffs());
    KT_IDLE_EQ(per100(&r, PATH_LOCK_SLOW), 0);   /* every release is the top lock */
    KT_EQ(per100(&r, PATH_CLOCK), 0);   /* no deadline: no clock read */
}

/* utest bench-call against bench-echo, both on one CPU: today's 5 system
 * calls (the call; the server's read, write, a read that finds nothing,
 * and its wait), 5 handle lookups, 2 messages, and per switch one restore
 * and one CR3 load. The bytes go straight from user memory into each
 * message and out of it again, so the kernel makes no copy of its own (4
 * user copies of the bytes a round trip, no stack buffers in between).
 * The request is queued: allocated, one job charge and one credit. The
 * reply is handed to the waiting caller in the server's slot: no
 * allocation, no charge. Both threads switch out inside a system call, so
 * no switch saves the FPU state: each keeps only its control words (the
 * system call rule, fpu.c). */
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
    KT_EQ(per100(&r, PATH_KMALLOC), 100);
    KT_EQ(per100(&r, PATH_KFREE), 100);
    KT_EQ(per100(&r, PATH_KCOPY), 0);
    KT_EQ(per100(&r, PATH_UCOPY_IN_B), 20800);
    KT_EQ(per100(&r, PATH_HANDLE), 500);
    KT_EQ(per100(&r, PATH_JOB), 200);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_HANDOFF), handoffs());   /* the request's, as kcall */
    KT_IDLE_EQ(per100(&r, PATH_FPU_SAVE), fpu_call_drop() ? 0 : 200);
    KT_IDLE_EQ(per100(&r, PATH_FPU_CALLED), fpu_call_drop() ? 200 : 0);
    KT_IDLE_EQ(per100(&r, PATH_FPU_RESTORE), 200);
    KT_IDLE_EQ(per100(&r, PATH_CR3), 200);
    /* The checker never turns interrupts off: each release is of the top
     * lock, and every lock pair has been seen before the window. */
    KT_IDLE_EQ(per100(&r, PATH_LOCK_SLOW), 0);
    KT_EQ(per100(&r, PATH_CLOCK), 0);   /* no deadline: no clock read */
}

/* The same call against a server on channel_reply_wait (utest
 * bench-rwecho): 2 system calls a round trip (the call; the server's one
 * reply-and-wait) where bench-echo makes 5, 2 handle lookups (the server's
 * reply and wait share one), and no read that finds nothing: the server's
 * call looks at the queue before it blocks. Each message goes to a thread
 * already waiting for it, in its writer's slot: no allocation, no job
 * charge. Each wake hands the CPU straight to the thread it wakes, which
 * then runs: 2 hand-offs, no queueing. */
KTEST(pathstat_user_reply_wait_counts)
{
    struct path_result r;
    if (!bench_path_ucall("rwcall", test_cpu(), test_cpu(), 16, &r)) {
        KT_SKIP_LIVE("no bin/utest or no trace free");
        KT_ASSERT(!"bench_path_ucall failed");
    }
    KT_ASSERT(r.calls > 0);
    KT_EQ(sys100(&r, SYS_channel_call), 100);
    KT_EQ(sys100(&r, SYS_channel_reply_wait), 100);
    KT_ASSERT(per100(&r, PATH_SYSCALL) >= 200 && per100(&r, PATH_SYSCALL) <= 202);
    KT_EQ(per100(&r, PATH_EMPTY_READ), 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 0);
    KT_EQ(per100(&r, PATH_KFREE), 0);
    KT_EQ(per100(&r, PATH_JOB), 0);
    KT_EQ(per100(&r, PATH_KCOPY), 0);
    KT_EQ(per100(&r, PATH_UCOPY_IN_B), 22400);   /* the two argument structs, 16 bytes each way */
    KT_EQ(per100(&r, PATH_HANDLE), 200);
    /* The pair runs on one time slice (the hand-off doesn't refresh it):
     * when it runs out, a tick preempts whichever runs, a pass that picks it
     * again. One every 20 ms: a few in a slow window. */
    KT_IDLE_ASSERT(per100(&r, PATH_SCHED) >= 200 && per100(&r, PATH_SCHED) <= 205);
    KT_IDLE_EQ(per100(&r, PATH_SWITCH), 200);
    KT_IDLE_EQ(per100(&r, PATH_HANDOFF), 2 * handoffs());
    KT_IDLE_EQ(per100(&r, PATH_FPU_RESTORE), 200);
    KT_IDLE_EQ(per100(&r, PATH_CR3), 200);
    /* 11 a round trip: per side the handle table, the pair and the peer's
     * endpoint for the send, its own endpoint before it blocks (not after:
     * it finds its message handed over, chan_handed) and the scheduler's;
     * the caller also lists itself on its endpoint first. A handed wake
     * takes no lock (with the hand-off off, each wake takes the run
     * queue's: 13). A rare extra one (a slice's end) in the trace's window:
     * not exact. */
    uint64_t locks = 1300 - 2 * handoffs();
    KT_IDLE_ASSERT(per100(&r, PATH_LOCK) >= locks && per100(&r, PATH_LOCK) <= locks + 10);
    KT_IDLE_EQ(per100(&r, PATH_LOCK_SLOW), 0);
    /* No deadline, no clock read, but the client's: its warm-up (calls
     * this quick may fill the whole window) reads the clock every 64. */
    KT_ASSERT(per100(&r, PATH_CLOCK) <= 2);
}

/* The same call with a deadline, as libos gives every file call: one more
 * system call (the clock) and one sleeper-queue entry per call. Clock
 * reads: the clock_get and the call's wait, which has a deadline (the
 * server's wait has none, and reads no clock). The 5 s
 * deadline is after the CPU's next tick, so it never re-arms the timer
 * (the tick looks after it: wait.c). */
KTEST(pathstat_user_deadline_call_counts)
{
    struct path_result r;
    if (!bench_path_ucall("dcall", test_cpu(), test_cpu(), 0, &r)) {
        KT_SKIP_LIVE("no bin/utest or no trace free");
        KT_ASSERT(!"bench_path_ucall failed");
    }
    KT_ASSERT(r.calls > 0);
    KT_EQ(sys100(&r, SYS_clock_get), 100);
    KT_EQ(sys100(&r, SYS_channel_call), 100);
    KT_EQ(per100(&r, PATH_SLEEPQ), 100);
    KT_EQ(per100(&r, PATH_TIMER_ARM), 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 100);
    KT_IDLE_EQ(per100(&r, PATH_CLOCK), 200);
    KT_IDLE_EQ(per100(&r, PATH_LOCK_SLOW), 0);
}

/* The call through generated code (tools/genidl.py), as a program makes
 * one: null.ping's client stub with a time limit, as libos's file calls
 * have, against null_serve (utest bench-gcall, bench-gecho). The time
 * limit goes to the kernel as a timeout (_within), so the client makes
 * no clock_get first; the generated server loop answers each request in
 * the call that takes the next one (channel_reply_wait). 2 system calls a
 * round trip (6 when the client read the clock for a deadline and the
 * loop read, wrote, read again to find nothing and waited), no read that
 * finds nothing (1). Still two clock reads, both the kernel's now: the
 * call turning its timeout into a deadline, and its wait (which has one). */
KTEST(pathstat_user_generated_call_counts)
{
    struct path_result r;
    if (!bench_path_ucall("gcall", test_cpu(), test_cpu(), 0, &r)) {
        KT_SKIP_LIVE("no bin/utest or no trace free");
        KT_ASSERT(!"bench_path_ucall failed");
    }
    KT_ASSERT(r.calls > 0);
    /* The warm-up reads the clock every 64 calls (calls this quick may
     * fill the whole window): at most 2 in 100. */
    KT_ASSERT(sys100(&r, SYS_clock_get) <= 2);
    KT_EQ(sys100(&r, SYS_channel_call), 100);
    KT_EQ(sys100(&r, SYS_channel_reply_wait), 100);
    KT_EQ(sys100(&r, SYS_channel_read), 0);
    KT_EQ(sys100(&r, SYS_channel_write), 0);
    KT_EQ(sys100(&r, SYS_object_wait_one), 0);
    KT_ASSERT(per100(&r, PATH_SYSCALL) >= 200 && per100(&r, PATH_SYSCALL) <= 202);
    KT_EQ(per100(&r, PATH_EMPTY_READ), 0);
    KT_EQ(per100(&r, PATH_KMALLOC), 0);
    KT_EQ(per100(&r, PATH_SLEEPQ), 100);
    /* Two a call, and now and then one of the warm-up's (above). */
    KT_IDLE_ASSERT(per100(&r, PATH_CLOCK) >= 200 && per100(&r, PATH_CLOCK) <= 202);
}
