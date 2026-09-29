/* M5 phase-2 performance pieces: per-CPU page stashes, the thread stack
 * cache limit, wake-affine channel_call placement, the priority ceiling. */
#include <jam/channel.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/time.h>

#define SECOND 1000000000ull

static uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

/* Pin the test thread ("main") to one CPU; returns that CPU. */
static uint32_t pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    return cpu;
}

static void unpin_self(void)
{
    cpumask_t m;
    cpumask_all(&m);
    thread_set_affinity(current_thread(), &m);
}

/* ---- per-CPU page stashes --------------------------------------------------- */

static struct page *stash_pages[80];

/* Single pages come from and go back to this CPU's stash, which refills and
 * drains in batches, and every page in it is still counted free. */
KTEST(pcp_refill_drain)
{
    uint32_t me = pin_self(cpu_count > 1 ? 1 : 0);
    pmm_drain_stashes();
    KT_EQ(pmm_stash_count(me), 0);
    uint64_t free0 = free_now();

    /* One page: the empty stash refills a batch and hands out its top. */
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    KT_ASSERT(p);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH - 1);
    KT_EQ(free_now(), free0 - 1);
    uint64_t *mem = page_to_virt(p);
    KT_ASSERT(mem[0] == 0 && mem[PAGE_SIZE / 8 - 1] == 0);   /* PMM_ZERO applied */
    pmm_free_pages(p, 0);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH);
    KT_EQ(free_now(), free0);

    /* LIFO: the page just freed is the next one handed out (cache-hot). */
    struct page *again = pmm_alloc_pages(0, 0);
    KT_ASSERT(again == p);
    pmm_free_pages(again, 0);

    /* 80 pages: 16 from the stash, then 4 refills of 16. Distinct pages. */
    for (int i = 0; i < 80; i++) {
        stash_pages[i] = pmm_alloc_pages(0, 0);
        KT_ASSERT(stash_pages[i]);
        KT_ASSERT(!(stash_pages[i]->flags & (PG_PCP | PG_FREE)));
        *(uint64_t *)page_to_virt(stash_pages[i]) = page_to_phys(stash_pages[i]);
    }
    KT_EQ(pmm_stash_count(me), 0);
    for (int i = 0; i < 80; i++)
        KT_EQ(*(uint64_t *)page_to_virt(stash_pages[i]), page_to_phys(stash_pages[i]));
    KT_EQ(free_now(), free0 - 80);

    /* Free them: the stash fills to PCP_MAX, drains a batch, fills again. */
    for (int i = 0; i < 80; i++)
        pmm_free_pages(stash_pages[i], 0);
    /* 64 at the 64th free; the 65th drains 16 (48) and adds one; 15 more. */
    KT_EQ(pmm_stash_count(me), PMM_PCP_MAX);
    KT_EQ(free_now(), free0);

    /* DMA32 requests bypass the stash. Freeing the page stashes it only if
     * DMA32 is the stash zone (no memory above 4 GiB, as in QEMU), and then
     * the full stash drains a batch first. */
    struct page *low = pmm_alloc_pages(0, PMM_DMA32);
    KT_ASSERT(low && page_to_phys(low) < (4ull << 30));
    KT_EQ(pmm_stash_count(me), PMM_PCP_MAX);
    pmm_free_pages(low, 0);
    uint32_t after = pmm_stash_count(me);
    KT_ASSERT(after == PMM_PCP_MAX || after == PMM_PCP_MAX - PMM_PCP_BATCH + 1);

    /* Draining puts everything back in the buddy lists; the count holds. */
    KT_ASSERT(pmm_drain_stashes() >= after);
    KT_EQ(pmm_stash_count(me), 0);
    KT_EQ(free_now(), free0);
    unpin_self();
}

/* A page freed on another CPU lands in THAT CPU's stash, still counted. */
static struct page *cross_page;

static void cross_free(void *arg)
{
    (void)arg;
    pmm_free_pages(cross_page, 0);
}

KTEST(pcp_cross_cpu_free)
{
    if (cpu_count < 2)
        return;
    uint32_t me = pin_self(0);
    uint32_t other = 1;
    pmm_drain_stashes();
    uint64_t free0 = free_now();
    cross_page = pmm_alloc_pages(0, 0);
    KT_ASSERT(cross_page);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH - 1);
    cpumask_t m;
    cpumask_one(&m, other);
    thread_join(thread_create_on("pcp-cross", cross_free, NULL, PRIO_DEFAULT, &m));
    /* thread_join returns once the thread has marked itself exited, but it
     * is reaped on its CPU's next switch, a moment later, and that can free
     * the slab page its struct lived in. Let that happen before counting:
     * on the real PC it landed between the two counts below and the free
     * count rose by one (a real free, not a drain bug). */
    thread_sleep_ms(20);
    KT_ASSERT(pmm_stash_count(other) >= 1);
    /* Creating the thread may have taken a slab page for its struct. */
    KT_ASSERT(free_now() + 1 >= free0);
    uint64_t f1 = free_now();
    pmm_drain_stashes();
    KT_EQ(free_now(), f1);
    unpin_self();
}

/* Out of memory: stashes on every CPU are drained before an allocation
 * fails, and every page is accounted for. */
static void stash_some(void *arg)
{
    (void)arg;
    struct page *p[20];
    for (int i = 0; i < 20; i++)
        p[i] = pmm_alloc_pages(0, 0);
    for (int i = 0; i < 20; i++)
        if (p[i])
            pmm_free_pages(p[i], 0);
}

KTEST(pcp_oom_drains_stashes)
{
    /* Park pages in every CPU's stash first. */
    for (uint32_t c = 0; c < cpu_count; c++) {
        cpumask_t m;
        cpumask_one(&m, c);
        thread_join(thread_create_on("pcp-stash", stash_some, NULL, PRIO_DEFAULT, &m));
    }
    thread_sleep_ms(20);   /* let the helpers be reaped first (see pcp_cross_cpu_free) */
    uint64_t parked = pmm_stash_pages();
    KT_ASSERT(parked >= cpu_count);
    uint64_t drains0 = pmm_stash_drains();
    uint64_t free0 = free_now();

    /* Take every page, chained through struct page (no memory touched).
     * Nothing else in the system may allocate meanwhile: the other CPUs
     * are idle between tests. */
    struct page *chain = NULL;
    uint64_t n = 0;
    struct page *p;
    while ((p = pmm_alloc_pages(0, 0))) {
        p->private = (uint64_t)chain;
        chain = p;
        n++;
    }
    uint64_t free_at_oom = free_now(), stash_at_oom = pmm_stash_pages();
    uint64_t drains = pmm_stash_drains() - drains0;
    while (chain) {
        struct page *next = (struct page *)chain->private;
        chain->private = 0;
        pmm_free_pages(chain, 0);
        chain = next;
    }
    kprintf("pcp: %lu pages taken (%lu were parked in stashes), %lu drains\n", n, parked,
            drains);
    KT_EQ(stash_at_oom, 0);
    KT_EQ(free_at_oom, 0);
    KT_EQ(n, free0);
    KT_ASSERT(drains >= 1);
    KT_EQ(free_now(), free0);
}

/* ---- thread stacks ---------------------------------------------------------- */

/* A freed stack's pages go back, and its virtual range is reused. */
KTEST(kstack_free_reuses_range)
{
    uint64_t free0 = free_now();
    void *a = kstack_alloc_try(64 * 1024);
    KT_ASSERT(a);
    ((volatile uint64_t *)a)[-1] = 1;   /* mapped and writable */
    KT_ASSERT(free_now() <= free0 - 16);
    kstack_free(a, 64 * 1024);
    KT_EQ(vmm_translate(vmm_kernel_pml4(), (uint64_t)a - 8), UINT64_MAX);
    void *b = kstack_alloc_try(64 * 1024);
    KT_ASSERT(b == a);   /* same range: no new vmap space, no new page tables */
    kstack_free(b, 64 * 1024);
    KT_ASSERT(free_now() + 1 >= free0);   /* a slab page for the slot at most */
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
    uint64_t accounted0 = free_now() + sched_stack_cache_pages();
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
    uint64_t accounted1 = free_now() + sched_stack_cache_pages();
    KT_ASSERT(accounted1 + 2 >= accounted0);   /* slot bookkeeping: a page or two */
    sched_stack_cache_set_limit(old);
}

/* ---- wake-affine channel_call ---------------------------------------------------- */

#define AFF_CALLS 200

static struct channel *aff_client_ep;
static volatile uint32_t aff_ran_on[MAX_CPUS];

static void aff_server(void *arg)
{
    struct channel *ep = arg;
    for (;;) {
        signals_t s = 0;
        object_wait_one((struct kobject *)ep, SIG_READABLE | SIG_PEER_CLOSED, uptime_ns() + 10 * SECOND,
                        &s);
        uint64_t m[2];
        uint32_t nb = 0;
        status_t st = channel_read(ep, m, sizeof(m), &nb, NULL, 0, NULL);
        if (st == OK) {
            preempt_disable();
            aff_ran_on[this_cpu()->index]++;
            preempt_enable();
            channel_write(ep, m, nb, NULL, 0);
        } else if (st != ERR_SHOULD_WAIT) {
            return;
        }
    }
}

static volatile uint32_t aff_bad;

static void aff_client(void *arg)
{
    (void)arg;
    for (int i = 0; i < AFF_CALLS; i++) {
        uint64_t req[2] = { 0, (uint64_t)i }, rep[2];
        uint32_t n = 0;
        if (channel_call(aff_client_ep, req, sizeof(req), NULL, 0, rep, sizeof(rep), &n, NULL, 0,
                         NULL, uptime_ns() + 10 * SECOND) != OK || rep[1] != (uint64_t)i)
            aff_bad++;
    }
}

/* One ping-pong: client pinned to `ccpu`, server allowed on `smask`.
 * Returns the server's affine wake count; aff_ran_on says where it ran. */
static uint64_t aff_round(uint32_t ccpu, const cpumask_t *smask, uint64_t *client_affine)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    aff_client_ep = a;
    aff_bad = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        aff_ran_on[i] = 0;
    struct thread *srv = thread_create_on("aff-server", aff_server, b, PRIO_DEFAULT, smask);
    cpumask_t cm;
    cpumask_one(&cm, ccpu);
    struct thread *cl = thread_create_on("aff-client", aff_client, NULL, PRIO_DEFAULT, &cm);
    /* Read the counters before join drops our references. */
    while (!cl->exited)
        thread_sleep_ms(1);
    *client_affine = cl->affine_wakes;
    thread_join(cl);
    kobject_unref((struct kobject *)a);   /* the server sees PEER_CLOSED */
    while (!srv->exited)
        thread_sleep_ms(1);
    uint64_t sa = srv->affine_wakes;
    thread_join(srv);
    kobject_unref((struct kobject *)b);
    KT_EQ(aff_bad, 0);
    return sa;
}

KTEST(wake_affine_channel_call)
{
    if (cpu_count < 2)
        return;
    pin_self(0);   /* keep the test thread off the client's CPU */
    /* The client's CPU: one with an HT sibling other than CPU 0 if any. */
    uint32_t c = 1;
    int sib = -1;
    for (uint32_t i = 1; i < cpu_count && sib < 0; i++)
        for (uint32_t j = 1; j < cpu_count; j++)
            if (j != i && cpus[j]->core_id == cpus[i]->core_id) {
                c = i;
                sib = (int)j;
                break;
            }
    cpumask_t any;
    cpumask_all(&any);
    uint64_t client_aff;
    uint64_t server_aff = aff_round(c, &any, &client_aff);
    kprintf("wake-affine: server placed affine %lu times, ran on cpu %u for %u of %d calls; "
            "client placed affine %lu times\n", server_aff, c, aff_ran_on[c], AFF_CALLS,
            client_aff);
    /* The request wakes the server onto the caller's CPU, and the reply
     * wakes the caller back onto it: nearly every call stays on one CPU. */
    KT_ASSERT(server_aff >= AFF_CALLS / 2);
    KT_ASSERT(aff_ran_on[c] >= AFF_CALLS / 2);
    KT_ASSERT(client_aff >= AFF_CALLS / 2);

    /* Server not allowed on the caller's CPU: it goes to the caller's idle
     * HT sibling when there is one (QEMU needs -smp N,threads=2). */
    cpumask_t not_c = any;
    not_c.bits[c / 64] &= ~(1ull << (c % 64));
    not_c.bits[0] &= ~1ull;   /* nor CPU 0, where this thread waits */
    server_aff = aff_round(c, &not_c, &client_aff);
    if (sib >= 0) {
        kprintf("wake-affine: server kept off cpu %u ran on its sibling cpu %d for %u of %d "
                "calls (%lu affine wakes)\n", c, sib, aff_ran_on[sib], AFF_CALLS, server_aff);
        KT_ASSERT(server_aff >= AFF_CALLS / 2);
        KT_ASSERT(aff_ran_on[sib] >= AFF_CALLS / 2);
    } else {
        kprintf("wake-affine: no HT sibling for cpu %u, sibling placement not tested\n", c);
        KT_EQ(server_aff, 0);
    }
    unpin_self();
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
