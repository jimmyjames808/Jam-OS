/* Threads (kernel/sched/thread.c): kernel stacks and the stack cache, and
 * the priority ceiling. */
#include <jam/irq.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sched.h>

#include "../sched/sched_internal.h"   /* thread_alloc, thread_reap */

/* ---- thread stacks ---------------------------------------------------------- */

/* A freed stack's pages go back, and its virtual range is reused. */
KTEST(kstack_free_reuses_range)
{
    KT_SKIP_LIVE("exact free pages and kernel stack ranges");
    /* One stack made and freed first, so this test starts from the same
     * state whatever ran before it: the stack space it uses is mapped (a
     * first stack beyond it takes a page-table page, kept for good) and
     * there is a free range to reuse. Then wait for the pages earlier
     * tests are still giving back (threads reaped at a CPU's next switch). */
    void *warm = kstack_alloc_try(64 * 1024);
    KT_ASSERT(warm);
    kstack_free(warm, 64 * 1024);
    uint64_t free0 = kt_free_pages_settled();
    void *a = kstack_alloc_try(64 * 1024);
    KT_ASSERT(a == warm);
    ((volatile uint64_t *)a)[-1] = 1;   /* mapped and writable */
    /* 16 pages taken. Taking a whole free range frees its list node, and
     * with it, if it was the only object there, the node's slab page. */
    KT_ASSERT(kt_free_pages() <= free0 - 15);
    kstack_free(a, 64 * 1024);
    KT_EQ(vmm_translate(vmm_kernel_pml4(), (uint64_t)a - 8), UINT64_MAX);
    void *b = kstack_alloc_try(64 * 1024);
    KT_ASSERT(b == a);   /* same range: no new vmap space, no new page tables */
    kstack_free(b, 64 * 1024);
    KT_ASSERT(kt_free_pages() + 1 >= free0);   /* a slab page for the slot at most */
}

static volatile bool stack_release;

static void stack_holder(void *arg)
{
    (void)arg;
    while (!stack_release)
        thread_sleep_ms(1);
}

/* Stacks over the cache limit are freed, not kept (nor leaked). */
KTEST(stack_cache_limit_frees)
{
    KT_NEEDS_IDLE("exact size of the thread stack cache, which every exiting thread changes");
    enum { LIMIT = 2, THREADS = 6 };
    unsigned old = sched_stack_cache_set_limit(LIMIT);
    KT_ASSERT(sched_stack_cache_pages() <= LIMIT * 16);
    uint64_t accounted0 = kt_free_pages() + sched_stack_cache_pages();
    uint64_t freed0 = sched_stacks_freed();

    stack_release = false;
    struct thread *t[THREADS];
    for (int i = 0; i < THREADS; i++)
        t[i] = thread_create("stack-holder", stack_holder, NULL, PRIO_DEFAULT);
    stack_release = true;
    for (int i = 0; i < THREADS; i++)
        thread_join(t[i]);
    /* Reaps happen at each CPU's next switch; let them, then trim. Not a fixed 8 ms: on a
     * loaded host a CPU may not switch for longer, so wait (at most 1 s) until the stacks
     * over the limit have been freed. */
    for (int i = 0; i < 200; i++) {
        thread_sleep_ms(5);
        thread_yield();
        sched_stack_trim();
        if (sched_stacks_freed() - freed0 >= THREADS - LIMIT)
            break;
    }

    KT_ASSERT(sched_stack_cache_pages() <= LIMIT * 16);
    KT_ASSERT(sched_stacks_freed() - freed0 >= THREADS - LIMIT);
    uint64_t accounted1 = kt_free_pages() + sched_stack_cache_pages();
    KT_GLOBAL_ASSERT(accounted1 + 2 >= accounted0);   /* slot bookkeeping: a page or two */
    sched_stack_cache_set_limit(old);
}

/* A thread with no stack of the cache's: thread "main" runs on the stack
 * kmain_stage2 was given (stack_top NULL) and exits at the end of every
 * run. Its reap must leave the cache alone, full (it wrote the doomed
 * list's link below address 0: the page fault in thread_reap that ended
 * M13 B3's init run at 2 CPUs, a killed spawner's hundreds of children
 * having filled the cache just before; proc_kill_spawner) or with room
 * (it cached NULL, counted as a stack's 16 pages). A thread made the way
 * main is, reaped once each way. */
KTEST(stack_cache_skips_stackless_reap)
{
    KT_NEEDS_IDLE("exact size of the thread stack cache, which every exiting thread changes");
    struct thread *t = thread_alloc("stackless", PRIO_DEFAULT);
    KT_ASSERT(t);
    KT_ASSERT(!t->stack_top);
    t->refs = 3;   /* ours, and one for each reap to drop */
    thread_set_state(t, T_DEAD);
    for (int i = 0; i < 4; i++) {   /* earlier tests' threads reaped at each CPU's next switch */
        thread_sleep_ms(2);
        thread_yield();
    }

    unsigned old = sched_stack_cache_set_limit(0);   /* full: every stack put is doomed */
    uint64_t f = irq_save();   /* as finish_switch reaps */
    thread_reap(t);
    uint64_t full = sched_stack_cache_pages();
    irq_restore(f);
    sched_stack_cache_set_limit(old);
    KT_EQ(full, 0);

    f = irq_save();
    uint64_t pages = sched_stack_cache_pages();
    thread_reap(t);
    uint64_t room = sched_stack_cache_pages();
    irq_restore(f);
    KT_EQ(room, pages);
    KT_EQ(t->refs, 1);
    thread_detach(t);
}

/* ---- priority ceiling -------------------------------------------------------- */

static volatile bool prio_release;

static void prio_holder(void *arg)
{
    (void)arg;
    while (!prio_release)
        thread_sleep_ms(1);
}

KTEST(priority_cap)
{
    prio_release = false;
    struct thread *t = thread_create_capped("prio-cap", prio_holder, NULL, PRIO_MAX, NULL,
                                            PRIO_USER_MAX);
    KT_EQ(t->prio_cap, PRIO_USER_MAX);
    KT_EQ(t->base_prio, PRIO_USER_MAX);   /* clamped at creation */
    thread_set_priority(t, PRIO_MAX);
    KT_EQ(t->base_prio, PRIO_USER_MAX);
    thread_set_priority(t, 5);
    KT_EQ(t->base_prio, 5);
    thread_set_priority(t, PRIO_MIN - 3);
    KT_EQ(t->base_prio, PRIO_MIN);
    thread_set_priority(t, 20);
    thread_set_priority_cap(t, 10);   /* lowering the ceiling lowers the priority */
    KT_EQ(t->base_prio, 10);
    thread_set_priority_cap(t, PRIO_MAX + 7);
    KT_EQ(t->prio_cap, PRIO_MAX);
    KT_EQ(t->base_prio, 10);
    prio_release = true;
    thread_join(t);

    /* Kernel threads default to no ceiling below PRIO_MAX. */
    prio_release = true;
    struct thread *k = thread_create("prio-kernel", prio_holder, NULL, PRIO_MAX);
    KT_EQ(k->prio_cap, PRIO_MAX);
    thread_join(k);
}
