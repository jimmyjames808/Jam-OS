#include <stdint.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/selftest.h>
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

/* Every CPU allocates and frees at once, checking each block keeps the
 * pattern it wrote: catches allocator races before the scheduler arrives. */
static volatile uint64_t smp_failures, smp_ops;

static void smp_alloc_worker(void *arg)
{
    (void)arg;
    uint32_t me = this_cpu()->index;
    uint64_t seed = 0x2545f4914f6cdd1dull * (me + 1);
    enum { SLOTS = 64 };
    uint8_t *ptrs[SLOTS] = { 0 };
    uint32_t sizes[SLOTS] = { 0 };
    struct page *pages[SLOTS] = { 0 };

    for (int round = 0; round < 3000; round++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        unsigned slot = seed % SLOTS;
        if (ptrs[slot]) {
            for (uint32_t b = 0; b < sizes[slot]; b += 61)
                if (ptrs[slot][b] != (uint8_t)(me * 7 + slot))
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
            memset(ptrs[slot], (uint8_t)(me * 7 + slot), sizes[slot]);
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

static void test_smp_alloc(void)
{
    uint64_t total, free_before, free_after;
    pmm_stats(&total, &free_before);
    smp_run_on_all(smp_alloc_worker, NULL);
    pmm_stats(&total, &free_after);
    CHECK(smp_failures == 0);
    kprintf("selftest: smp alloc ok (%u CPUs, %lu ops, %ld pages still in slabs)\n",
            cpu_count, smp_ops, (long)(free_before - free_after));
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
    test_smp_alloc();
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
    if (has_word(cmdline, "teststack"))
        kprintf("%lu\n", recurse_forever(0));
}
