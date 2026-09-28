#include <stdint.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/cmdline.h>
#include <jam/irq.h>
#include <jam/ipi.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/spinlock.h>
#include <jam/time.h>
#include <jam/x86.h>
#include <jam/smp.h>
#include <jam/string.h>

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond))                                                    \
            panic("selftest: %s failed (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)

/* xorshift, so the tests are repeatable. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_pmm(void)
{
    uint64_t total, free_before, free_now;
    pmm_stats(&total, &free_before);

    enum { N = 512 };
    static struct page *pages[N];
    static unsigned orders[N];
    for (int i = 0; i < N; i++) {
        orders[i] = rng() % 4;
        pages[i] = pmm_alloc_pages(orders[i], PMM_ZERO);
        CHECK(pages[i]);
        uint64_t *mem = page_to_virt(pages[i]);
        CHECK(mem[0] == 0 && mem[(PAGE_SIZE << orders[i]) / 8 - 1] == 0);
        mem[0] = page_to_phys(pages[i]);   /* detect overlapping blocks */
    }
    for (int i = 0; i < N; i++)
        CHECK(*(uint64_t *)page_to_virt(pages[i]) == page_to_phys(pages[i]));

    struct page *low = pmm_alloc_pages(0, PMM_DMA32);
    CHECK(low && page_to_phys(low) < (4ull << 30));
    pmm_free_pages(low, 0);

    for (int i = N; i-- > 0;)
        pmm_free_pages(pages[i], orders[i]);
    pmm_stats(&total, &free_now);
    CHECK(free_now == free_before);   /* everything merged back */
    kprintf("selftest: pmm ok (%d blocks, free pages restored)\n", N);
}

static void test_heap(void)
{
    enum { N = 2000 };
    static uint8_t *ptrs[N];
    static uint32_t sizes[N];
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < N; i++) {
            uint32_t sz = (rng() % 8 == 0) ? 1 + rng() % 20000 : 1 + rng() % 600;
            ptrs[i] = kmalloc(sz);
            CHECK(ptrs[i]);
            sizes[i] = sz;
            memset(ptrs[i], (uint8_t)i, sz);
        }
        for (int i = 0; i < N; i++)
            for (uint32_t b = 0; b < sizes[i]; b += 97)
                CHECK(ptrs[i][b] == (uint8_t)i);
        for (int i = 0; i < N; i++)
            kfree(ptrs[i]);
    }
    kprintf("selftest: heap ok (%d allocations x 4 rounds)\n", N);
}

static void test_vmm(void)
{
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    CHECK(pa);
    uint64_t va = VMAP_BASE + (1ull << 39) - PAGE_SIZE * 16;   /* unused vmap slot */
    vmm_map(pml4, va, pa, PAGE_SIZE, VM_WRITE | VM_SMALL);
    CHECK(vmm_translate(pml4, va + 123) == pa + 123);
    *(volatile uint64_t *)va = 0x4a414d4f53ull;   /* "JAMOS" */
    CHECK(*(uint64_t *)phys_to_virt(pa) == 0x4a414d4f53ull);
    vmm_unmap(pml4, va, PAGE_SIZE);
    CHECK(vmm_translate(pml4, va) == UINT64_MAX);
    pmm_free_page_phys(pa);

    /* The HHDM uses large pages; translate must see through them. */
    CHECK(vmm_translate(pml4, (uint64_t)phys_to_virt(pa)) == pa);
    kprintf("selftest: vmm ok\n");
}

/* ---- multi-CPU tests (need the scheduler) ------------------------------- */

/* One thread pinned to each CPU allocates and frees at once, checking each
 * block keeps the pattern it wrote: catches allocator races. */
static volatile uint64_t smp_failures, smp_ops;

static void smp_alloc_worker(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg;
    CHECK(this_cpu()->index == me);   /* affinity was honoured */
    uint64_t seed = 0x2545f4914f6cdd1dull * (me + 1);
    enum { SLOTS = 64 };
    uint8_t *ptrs[SLOTS] = { 0 };
    uint32_t sizes[SLOTS] = { 0 };
    struct page *pages[SLOTS] = { 0 };

    for (int round = 0; round < 3000; round++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        unsigned slot = seed % SLOTS;
        uint8_t tag = (uint8_t)(me * 7 + slot);
        if (ptrs[slot]) {
            for (uint32_t b = 0; b < sizes[slot]; b += 61)
                if (ptrs[slot][b] != tag)
                    __atomic_add_fetch(&smp_failures, 1, __ATOMIC_RELAXED);
            kfree(ptrs[slot]);
            ptrs[slot] = NULL;
        } else {
            sizes[slot] = 1 + (seed >> 20) % ((seed & 1) ? 3000 : 200);
            ptrs[slot] = kmalloc(sizes[slot]);
            if (!ptrs[slot]) {
                __atomic_add_fetch(&smp_failures, 1, __ATOMIC_RELAXED);
                continue;
            }
            memset(ptrs[slot], tag, sizes[slot]);
        }
        if (pages[slot]) {
            if (*(uint64_t *)page_to_virt(pages[slot]) != page_to_phys(pages[slot]))
                __atomic_add_fetch(&smp_failures, 1, __ATOMIC_RELAXED);
            pmm_free_pages(pages[slot], 1);
            pages[slot] = NULL;
        } else if ((pages[slot] = pmm_alloc_pages(1, 0))) {
            *(uint64_t *)page_to_virt(pages[slot]) = page_to_phys(pages[slot]);
        }
        __atomic_add_fetch(&smp_ops, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < SLOTS; i++) {
        kfree(ptrs[i]);
        if (pages[i])
            pmm_free_pages(pages[i], 1);
    }
}

static struct thread *spawn_pinned(const char *name, void (*fn)(void *), uint32_t cpu, int prio)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    return thread_create_on(name, fn, (void *)(uintptr_t)cpu, prio, &m);
}

static void test_smp_alloc(void)
{
    uint64_t total, free_before, free_after;
    pmm_stats(&total, &free_before);
    static struct thread *ts[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++)
        ts[i] = spawn_pinned("alloc-stress", smp_alloc_worker, i, PRIO_DEFAULT);
    for (uint32_t i = 0; i < cpu_count; i++)
        thread_join(ts[i]);
    pmm_stats(&total, &free_after);
    CHECK(smp_failures == 0);
    kprintf("selftest: smp alloc ok (%u CPUs, %lu ops, %ld pages still in slabs)\n",
            cpu_count, smp_ops, (long)(free_before - free_after));
}

/* Threads + mutex: many threads increment one counter under a mutex. */
static struct mutex count_lock;
static volatile uint64_t shared_count;

static void counter_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        mutex_lock(&count_lock);
        uint64_t v = shared_count;   /* non-atomic on purpose: the mutex protects it */
        if (i % 100 == 0)
            thread_yield();          /* yield while holding it: others must wait */
        shared_count = v + 1;
        mutex_unlock(&count_lock);
    }
}

static void test_threads_mutex(void)
{
    mutex_init(&count_lock, "test counter");
    shared_count = 0;
    uint32_t n = cpu_count * 3;
    struct thread **ts = kmalloc(sizeof(*ts) * n);
    for (uint32_t i = 0; i < n; i++)
        ts[i] = thread_create("counter", counter_worker, NULL, PRIO_DEFAULT - 2 + (int)(i % 5));
    for (uint32_t i = 0; i < n; i++)
        thread_join(ts[i]);
    kfree(ts);
    CHECK(shared_count == (uint64_t)n * 2000);
    kprintf("selftest: threads + mutex ok (%u threads, count %lu)\n", n, shared_count);
}

/* Sleep: asleep for at least what was asked, and not much more. */
static void test_sleep(void)
{
    for (uint64_t ms = 10; ms <= 50; ms += 20) {
        uint64_t t0 = uptime_ns();
        thread_sleep_ms(ms);
        uint64_t got = (uptime_ns() - t0) / 1000000;
        CHECK(got >= ms && got <= ms + 30);
    }
    kprintf("selftest: sleep ok\n");
}

/* Priority: on one CPU, a high-priority thread runs before queued lower
 * ones even if created last. */
static volatile uint32_t order_seq, order_of[3], order_cpu[3];
static volatile uint64_t order_t[3], hog_end;

static void order_worker(void *arg)
{
    order_of[(uintptr_t)arg] = __atomic_add_fetch(&order_seq, 1, __ATOMIC_RELAXED);
    order_cpu[(uintptr_t)arg] = this_cpu()->index;
    order_t[(uintptr_t)arg] = uptime_ns();
}

static volatile bool others_queued;

/* Keep the CPU busy until the test has queued the three threads behind it
 * (bounded, so a bug fails the check rather than hanging). */
static void hog_until_queued(void *arg)
{
    (void)arg;
    uint64_t start = uptime_ns();
    while (!others_queued && uptime_ns() - start < 2000000000ull)
        cpu_relax();
    hog_end = uptime_ns();
}

static void test_priority(void)
{
    uint32_t cpu = cpu_count > 1 ? cpu_count - 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    order_seq = 0;
    /* Hold the CPU with a busy thread so the others queue up. */
    others_queued = false;
    struct thread *hog = thread_create_on("prio-hog", hog_until_queued, NULL, PRIO_MAX, &m);
    struct thread *lo = thread_create_on("prio-low", order_worker, (void *)0, 4, &m);
    struct thread *mid = thread_create_on("prio-mid", order_worker, (void *)1, 12, &m);
    struct thread *hi = thread_create_on("prio-high", order_worker, (void *)2, 28, &m);
    others_queued = true;
    thread_join(hog);
    thread_join(lo);
    thread_join(mid);
    thread_join(hi);
    if (!(order_of[2] < order_of[1] && order_of[1] < order_of[0]))
        panic("priority: ran low #%u cpu %u t=%lu, mid #%u cpu %u t=%lu, high #%u cpu %u t=%lu; hog ended %lu (want high, mid, low on cpu %u)",
              order_of[0], order_cpu[0], order_t[0], order_of[1], order_cpu[1], order_t[1],
              order_of[2], order_cpu[2], order_t[2], hog_end, cpu);
    kprintf("selftest: priority ok\n");
}

/* TLB shootdown: every CPU caches a mapping, it is changed under them, and
 * every CPU must see the new page. Without the shootdown IPI, stale TLB
 * entries would keep showing the old value. */
static volatile uint64_t *shoot_va;
static volatile uint64_t shoot_expect, shoot_bad;

static void shoot_read(void *arg)
{
    (void)arg;
    if (*shoot_va != shoot_expect)
        __atomic_add_fetch(&shoot_bad, 1, __ATOMIC_RELAXED);
}

static void test_tlb_shootdown(void)
{
    enum { ROUNDS = 50 };
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t va = vmm_reserve(PAGE_SIZE);
    uint64_t pas[ROUNDS];
    shoot_va = (volatile uint64_t *)va;
    shoot_bad = 0;
    /* Old pages keep their old values until the end, so a stale TLB entry
     * would read a wrong value rather than whatever reused the page. */
    for (int round = 0; round < ROUNDS; round++) {
        pas[round] = pmm_alloc_page_phys(PMM_ZERO);
        CHECK(pas[round]);
        *(uint64_t *)phys_to_virt(pas[round]) = shoot_expect = 0x1000 + round;
        vmm_map(pml4, va, pas[round], PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
        smp_call_all(shoot_read, NULL);   /* every CPU caches the mapping */
        vmm_unmap(pml4, va, PAGE_SIZE);   /* ... and must drop it */
    }
    for (int round = 0; round < ROUNDS; round++)
        pmm_free_page_phys(pas[round]);
    CHECK(shoot_bad == 0);
    kprintf("selftest: TLB shootdown ok (50 remaps seen by all %u CPUs)\n", cpu_count);
}

void selftest_run(void)
{
    test_pmm();
    test_vmm();
    test_heap();
    kprintf("selftest: single-CPU tests passed\n");
}

void selftest_run_smp(void)
{
    test_sleep();
    test_priority();
    test_threads_mutex();
    test_smp_alloc();
    test_tlb_shootdown();
    kprintf("selftest: all passed\n");
}

static int has_word(const char *s, const char *w)
{
    size_t wl = strlen(w);
    for (const char *p = s; *p; p++)
        if ((p == s || p[-1] == ' ') && !memcmp(p, w, wl) && (p[wl] == ' ' || !p[wl]))
            return 1;
    return 0;
}

/* Deliberately unbounded: runs into the stack guard page. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
__attribute__((noinline)) static uint64_t recurse_forever(uint64_t n)
{
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)n;
    return recurse_forever(n + 1) + pad[0];
}
#pragma GCC diagnostic pop

/* Opaque to the optimiser so the NULL write isn't flagged or removed. */
static volatile uint64_t *volatile null_ptr;

/* ---- deliberate crashes that need several CPUs ---------------------------- */

static spinlock_t lock_a = SPINLOCK_INIT("test lock A");
static spinlock_t lock_b = SPINLOCK_INIT("test lock B");
static spinlock_t lock_c = SPINLOCK_INIT("test lock C");
static spinlock_t pair_1 = SPINLOCK_INIT("test pair");
static spinlock_t pair_2 = SPINLOCK_INIT("test pair");
static struct mutex mutex_a, mutex_b;

static void take_c_in_irq(void *arg)
{
    (void)arg;
    spin_lock(&lock_c);   /* runs in the IPI handler */
    spin_unlock(&lock_c);
}

static void hold_forever(void *arg)
{
    (void)arg;
    spin_lock(&lock_a);
    for (;;)
        cpu_relax();   /* interrupts stay on: only the lock is stuck */
}

static void irqs_off_forever(void *arg)
{
    (void)arg;
    irq_disable();
    for (;;)
        cpu_relax();
}

void selftest_crash_smp(void)
{
    if (cmdline_has("testlockorder")) {
        /* A then B once, then B then A: never deadlocks on this run, but
         * the checker must still refuse the second order. */
        spin_lock(&lock_a);
        spin_lock(&lock_b);
        spin_unlock(&lock_b);
        spin_unlock(&lock_a);
        spin_lock(&lock_b);
        spin_lock(&lock_a);
    }
    if (cmdline_has("testlocknest")) {
        /* Two locks of one class, nested without spin_lock_nested. */
        spin_lock(&pair_1);
        spin_lock(&pair_2);
    }
    if (cmdline_has("testlockirq") && cpu_count > 1) {
        /* Taken in an interrupt handler on CPU 1, then with interrupts on. */
        smp_call_on(1, take_c_in_irq, NULL);
        spin_lock(&lock_c);
    }
    if (cmdline_has("testmutexorder")) {
        mutex_init(&mutex_a, "test mutex A");
        mutex_init(&mutex_b, "test mutex B");
        mutex_lock(&mutex_a);
        mutex_lock(&mutex_b);
        mutex_unlock(&mutex_b);
        mutex_unlock(&mutex_a);
        mutex_lock(&mutex_b);
        mutex_lock(&mutex_a);
    }
    if (cmdline_has("testmutexspin")) {
        mutex_init(&mutex_a, "test mutex A");
        spin_lock(&lock_a);
        mutex_lock(&mutex_a);   /* may sleep with a spinlock held */
    }
    if (cmdline_has("teststuck") && cpu_count > 1) {
        spawn_pinned("lock-hog", hold_forever, 1, PRIO_DEFAULT);
        thread_sleep_ms(50);
        spin_lock(&lock_a);   /* spins until the 5 s stuck-lock panic */
    }
    if (cmdline_has("testwatchdog") && cpu_count > 1) {
        spawn_pinned("irqs-off", irqs_off_forever, 1, PRIO_DEFAULT);
        thread_sleep_ms(20000);   /* the watchdog should fire within ~6 s */
    }
}

void selftest_crash(const char *cmdline)
{
    if (has_word(cmdline, "testbp")) {
        __asm__ volatile("int3");
        kprintf("selftest: returned from breakpoint\n");
    }
    if (has_word(cmdline, "testpanic"))
        panic("test panic requested on the kernel command line");
    if (has_word(cmdline, "testpf"))
        null_ptr[3] = 1;
    if (has_word(cmdline, "testro"))   /* kernel text must be read-only */
        *(volatile uint8_t *)(uintptr_t)&selftest_run = 0xcc;
    if (has_word(cmdline, "testrohhdm")) {   /* ...through the HHDM alias too */
        uint64_t pa = vmm_translate(vmm_kernel_pml4(), (uint64_t)(uintptr_t)&selftest_run);
        *(volatile uint8_t *)phys_to_virt(pa) = 0xcc;
    }
    if (has_word(cmdline, "teststack"))
        kprintf("%lu\n", recurse_forever(0));
}
