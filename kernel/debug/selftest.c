/* Boot-time checks that ship in every kernel (KTESTS=0 too), unlike the
 * ktests in kernel/test/:
 *   - the self-test (boot word `selftest`): the page allocator, VMM and heap
 *     on one CPU, then sleeping, priorities, mutexes, allocation on every
 *     CPU and TLB shootdowns;
 *   - the crash tests (boot words test<name>, the shell's `crash <name>`),
 *     each of which must end on the panic screen with the right message
 *     (bp must come back instead). */
#include <stdint.h>
#include <jam/cmdline.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/smp.h>
#include <jam/spinlock.h>
#include <jam/status.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

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
static uint64_t smp_failures, smp_ops;   /* added to atomically by the workers */

/* p was filled with tag: check a sample of its bytes, then free it. */
static void check_free(uint8_t *p, uint32_t size, uint8_t tag)
{
    for (uint32_t b = 0; b < size; b += 61)
        if (p[b] != tag)
            __atomic_add_fetch(&smp_failures, 1, __ATOMIC_RELAXED);
    kfree(p);
}

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
            check_free(ptrs[slot], sizes[slot], tag);
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
    CHECK(__atomic_load_n(&smp_failures, __ATOMIC_RELAXED) == 0);
    report("selftest: smp alloc ok (%u CPUs, %lu ops, %ld pages still in slabs)",
            cpu_count, __atomic_load_n(&smp_ops, __ATOMIC_RELAXED),
            (long)(free_before - free_after));
}

/* Threads + mutex: many threads increment one counter under a mutex. */
static struct mutex count_lock;
static uint64_t shared_count;   /* count_lock */

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
/* Written by the threads, read once they are joined. */
static uint32_t order_seq, order_of[3], order_cpu[3];
static uint64_t order_t[3], hog_end;

static void order_worker(void *arg)
{
    order_of[(uintptr_t)arg] = __atomic_add_fetch(&order_seq, 1, __ATOMIC_RELAXED);
    order_cpu[(uintptr_t)arg] = this_cpu()->index;
    order_t[(uintptr_t)arg] = uptime_ns();
}

static bool others_queued;   /* the hog's cue to stop */

/* Keep the CPU busy until the test has queued the three threads behind it
 * (bounded, so a bug fails the check rather than hanging). */
static void hog_until_queued(void *arg)
{
    (void)arg;
    uint64_t start = uptime_ns();
    while (!__atomic_load_n(&others_queued, __ATOMIC_ACQUIRE) &&
           uptime_ns() - start < 2000000000ull)
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
    __atomic_store_n(&others_queued, false, __ATOMIC_RELEASE);
    struct thread *hog = thread_create_on("prio-hog", hog_until_queued, NULL, PRIO_MAX, &m);
    struct thread *lo = thread_create_on("prio-low", order_worker, (void *)0, 4, &m);
    struct thread *mid = thread_create_on("prio-mid", order_worker, (void *)1, 12, &m);
    struct thread *hi = thread_create_on("prio-high", order_worker, (void *)2, 28, &m);
    __atomic_store_n(&others_queued, true, __ATOMIC_RELEASE);
    thread_join(hog);
    thread_join(lo);
    thread_join(mid);
    thread_join(hi);
    if (!(order_of[2] < order_of[1] && order_of[1] < order_of[0]))
        panic("priority: ran low #%u cpu %u t=%lu, mid #%u cpu %u t=%lu, "
              "high #%u cpu %u t=%lu; hog ended %lu (want high, mid, low on cpu %u)",
              order_of[0], order_cpu[0], order_t[0], order_of[1], order_cpu[1], order_t[1],
              order_of[2], order_cpu[2], order_t[2], hog_end, cpu);
    kprintf("selftest: priority ok\n");
}

/* TLB shootdown: every CPU caches a mapping, it is changed under them, and
 * every CPU must see the new page. Without the shootdown IPI, stale TLB
 * entries would keep showing the old value. */
static volatile uint64_t *shoot_va;
static uint64_t shoot_expect, shoot_bad;   /* the value to see; CPUs that saw another */

static void shoot_read(void *arg)
{
    (void)arg;
    if (*shoot_va != __atomic_load_n(&shoot_expect, __ATOMIC_RELAXED))
        __atomic_add_fetch(&shoot_bad, 1, __ATOMIC_RELAXED);
}

static void test_tlb_shootdown(void)
{
    enum { ROUNDS = 50 };
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t va = vmm_reserve(PAGE_SIZE);
    uint64_t pas[ROUNDS];
    shoot_va = (volatile uint64_t *)va;
    __atomic_store_n(&shoot_bad, 0, __ATOMIC_RELAXED);
    /* Old pages keep their old values until the end, so a stale TLB entry
     * would read a wrong value rather than whatever reused the page. */
    for (int round = 0; round < ROUNDS; round++) {
        pas[round] = pmm_alloc_page_phys(PMM_ZERO);
        CHECK(pas[round]);
        __atomic_store_n(&shoot_expect, 0x1000 + round, __ATOMIC_RELAXED);
        *(uint64_t *)phys_to_virt(pas[round]) = 0x1000 + round;
        vmm_map(pml4, va, pas[round], PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
        smp_call_all(shoot_read, NULL);   /* every CPU caches the mapping */
        vmm_unmap(pml4, va, PAGE_SIZE);   /* ... and must drop it */
    }
    for (int round = 0; round < ROUNDS; round++)
        pmm_free_page_phys(pas[round]);
    CHECK(__atomic_load_n(&shoot_bad, __ATOMIC_RELAXED) == 0);
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
    report("selftest: all passed");
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

/* ---- the crash tests, one function each ------------------------------------ */

static void crash_lockorder(void)
{
    /* A then B once, then B then A: never deadlocks on this run, but the
     * checker must still refuse the second order. */
    spin_lock(&lock_a);
    spin_lock(&lock_b);
    spin_unlock(&lock_b);
    spin_unlock(&lock_a);
    spin_lock(&lock_b);
    spin_lock(&lock_a);
}

static void crash_locknest(void)
{
    /* Two locks of one class, nested without spin_lock_nested. */
    spin_lock(&pair_1);
    spin_lock(&pair_2);
}

static void crash_lockirq(void)
{
    /* Taken in an interrupt handler on CPU 1, then with interrupts on. */
    smp_call_on(1, take_c_in_irq, NULL);
    spin_lock(&lock_c);
}

static void crash_mutexorder(void)
{
    mutex_init(&mutex_a, "test mutex A");
    mutex_init(&mutex_b, "test mutex B");
    mutex_lock(&mutex_a);
    mutex_lock(&mutex_b);
    mutex_unlock(&mutex_b);
    mutex_unlock(&mutex_a);
    mutex_lock(&mutex_b);
    mutex_lock(&mutex_a);
}

static void crash_mutexspin(void)
{
    mutex_init(&mutex_a, "test mutex A");
    spin_lock(&lock_a);
    mutex_lock(&mutex_a);   /* may sleep with a spinlock held */
}

static void crash_stuck(void)
{
    spawn_pinned("lock-hog", hold_forever, 1, PRIO_DEFAULT);
    thread_sleep_ms(50);
    spin_lock(&lock_a);   /* spins until the 5 s stuck-lock panic */
}

static void crash_watchdog(void)
{
    spawn_pinned("irqs-off", irqs_off_forever, 1, PRIO_DEFAULT);
    thread_sleep_ms(20000);   /* the watchdog should fire within ~6 s */
}

/* SMEP/SMAP: map one page as user (present, DPL 3) in the kernel's tables
 * (kernel threads run on them), then have the kernel touch it without
 * stac/clac. Both must be enabled by now (cpu_init_local ran on every CPU). */
static void crash_user_page(bool exec)
{
    uint64_t va = 0x2000;   /* a user-range address, page 0 stays unmapped */
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    unsigned flags = VM_USER | VM_SMALL | (exec ? VM_EXEC : VM_WRITE);
    vmm_map(vmm_kernel_pml4(), va, pa, PAGE_SIZE, flags);
    if (exec) {
        *(volatile uint8_t *)phys_to_virt(pa) = 0xc3;   /* ret */
        ((void (*)(void))va)();   /* kernel fetch from a user page: #PF */
    } else {
        volatile uint8_t x = *(volatile uint8_t *)va;   /* kernel read: #PF */
        kprintf("selftest: SMAP did not fault, read %u\n", x);
    }
}

static void crash_smap(void) { crash_user_page(false); }
static void crash_smep(void) { crash_user_page(true); }

static void crash_bp(void)
{
    __asm__ volatile("int3");
    kprintf("selftest: returned from breakpoint\n");
}

static void crash_panic(void)
{
    panic("test panic requested (crash test)");
}

static void crash_pf(void)
{
    null_ptr[3] = 1;
}

static void crash_ro(void)   /* kernel text must be read-only */
{
    *(volatile uint8_t *)(uintptr_t)&selftest_run = 0xcc;
}

static void crash_rohhdm(void)   /* ...through the HHDM alias too */
{
    uint64_t pa = vmm_translate(vmm_kernel_pml4(), (uint64_t)(uintptr_t)&selftest_run);
    *(volatile uint8_t *)phys_to_virt(pa) = 0xcc;
}

static void crash_stack(void)
{
    kprintf("%lu\n", recurse_forever(0));
}

/* Each one's boot word is "test<name>" (hidden options: the QEMU tests use
 * them). `early` ones run at boot before the scheduler starts
 * (selftest_crash), the others once every CPU is up (selftest_crash_smp).
 * All of them also run on a running system: selftest_crash_run, the
 * shell's `crash <name>` (debug_command "crash <name>"). In the order they
 * run at boot. */
static const struct crash_test {
    const char *name;          /* the shell's `crash <name>` */
    void      (*fn)(void);     /* does the crash */
    bool        early;         /* runs before the scheduler starts */
    bool        needs_2cpus;   /* skipped on a single CPU */
    const char *what;          /* what it shows, for the list */
} crash_tests[] = {
    { "bp",         crash_bp,         true,  false, "breakpoint (int3): must CONTINUE, no panic" },
    { "panic",      crash_panic,      true,  false, "a plain panic()" },
    { "pf",         crash_pf,         true,  false, "page fault: a NULL write" },
    { "ro",         crash_ro,         true,  false, "write to kernel code (must fault)" },
    { "rohhdm",     crash_rohhdm,     true,  false, "write to kernel code through the HHDM alias" },
    { "stack",      crash_stack,      true,  false, "stack overflow into the guard page" },
    { "lockorder",  crash_lockorder,  false, false, "lock order inversion (must be caught)" },
    { "locknest",   crash_locknest,   false, false, "two locks of one class nested" },
    { "lockirq",    crash_lockirq,    false, true,  "lock used in and out of interrupts" },
    { "mutexorder", crash_mutexorder, false, false, "mutex order inversion" },
    { "mutexspin",  crash_mutexspin,  false, false, "mutex taken holding a spinlock" },
    { "stuck",      crash_stuck,      false, true,  "stuck spinlock (panics after 5 s)" },
    { "watchdog",   crash_watchdog,   false, true,  "a CPU stuck with interrupts off (watchdog)" },
    { "smap",       crash_smap,       false, false, "SMAP: kernel reads a user page without stac" },
    { "smep",       crash_smep,       false, false, "SMEP: kernel jumps to a user page" },
};
#define NCRASH (sizeof(crash_tests) / sizeof(crash_tests[0]))

static int has_test_word(const char *cmdline, const char *name)
{
    char w[24];
    ksnprintf(w, sizeof(w), "test%s", name);
    return has_word(cmdline, w);
}

void selftest_crash_smp(void)
{
    const char *cl = cmdline_get();
    for (size_t i = 0; i < NCRASH; i++)
        if (!crash_tests[i].early && has_test_word(cl, crash_tests[i].name) &&
            (!crash_tests[i].needs_2cpus || cpu_count > 1))
            crash_tests[i].fn();
}

void selftest_crash(const char *cmdline)
{
    for (size_t i = 0; i < NCRASH; i++)
        if (crash_tests[i].early && has_test_word(cmdline, crash_tests[i].name))
            crash_tests[i].fn();
}

bool selftest_crash_known(const char *name, size_t len)
{
    for (size_t i = 0; i < NCRASH; i++)
        if (strlen(crash_tests[i].name) == len && !memcmp(crash_tests[i].name, name, len))
            return true;
    return false;
}

int64_t selftest_crash_run(const char *name)
{
    for (size_t i = 0; i < NCRASH; i++) {
        const struct crash_test *c = &crash_tests[i];
        if (strcmp(c->name, name))
            continue;
        if (c->needs_2cpus && cpu_count < 2) {
            kprintf("crash %s: needs 2 CPUs\n", name);
            return ERR_NOT_SUPPORTED;
        }
        kprintf("crash %s: %s\n", name, c->what);
        c->fn();
        return 0;   /* only bp comes back (or a crash test that failed to crash) */
    }
    return ERR_NOT_FOUND;
}

int64_t selftest_crash_list(void)
{
    kprintf("crash tests (each panics the kernel on purpose, except bp):\n");
    for (size_t i = 0; i < NCRASH; i++)
        kprintf("  %-10s %s\n", crash_tests[i].name, crash_tests[i].what);
    return (int64_t)NCRASH;
}
