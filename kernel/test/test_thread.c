/* Threads (kernel/sched/thread.c): kernel stacks and the stack cache, and
 * the priority ceiling. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sched.h>

/* ---- thread stacks ---------------------------------------------------------- */

/* A freed stack's pages go back, and its virtual range is reused. */
KTEST(kstack_free_reuses_range)
{
    KT_SKIP_LIVE("exact free pages and kernel stack ranges");
    uint64_t free0 = kt_free_pages();
    void *a = kstack_alloc_try(64 * 1024);
    KT_ASSERT(a);
    ((volatile uint64_t *)a)[-1] = 1;   /* mapped and writable */
    KT_ASSERT(kt_free_pages() <= free0 - 16);
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
    /* Reaps happen at each CPU's next switch; let them, then trim. */
    for (int i = 0; i < 4; i++) {
        thread_sleep_ms(2);
        thread_yield();
    }
    sched_stack_trim();

    KT_ASSERT(sched_stack_cache_pages() <= LIMIT * 16);
    KT_ASSERT(sched_stacks_freed() - freed0 >= THREADS - LIMIT);
    uint64_t accounted1 = kt_free_pages() + sched_stack_cache_pages();
    KT_GLOBAL_ASSERT(accounted1 + 2 >= accounted0);   /* slot bookkeeping: a page or two */
    sched_stack_cache_set_limit(old);
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
