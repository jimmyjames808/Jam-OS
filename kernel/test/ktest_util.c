/* Helpers shared by the kernel tests (declared in ktest.h), and the table
 * of debug hooks (jam/dbghook.h) the scheduler race tests install. */
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/userboot.h>

#include "../dev/vtd_internal.h"

void (*dbg_hooks[DBG_N])(void *arg);

uint64_t kt_free_pages(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

uint64_t kt_free_pages_settled(void)
{
    uint64_t prev = kt_free_pages();
    for (int i = 0; i < 100; i++) {
        thread_sleep_ms(5);
        uint64_t now = kt_free_pages();
        if (now == prev)
            break;
        prev = now;
    }
    return prev;
}

uint64_t kt_free_and_cached_pages(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);   /* drains the magazines first */
    return free + sched_stack_cache_pages();
}

uint32_t kt_cur_cpu(void)
{
    preempt_disable();
    uint32_t c = this_cpu()->index;
    preempt_enable_no_resched();
    return c;
}

uint32_t kt_pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    KT_EQ(kt_cur_cpu(), cpu);
    return cpu;
}

void kt_unpin_self(void)
{
    cpumask_t all;
    cpumask_all(&all);
    thread_set_affinity(current_thread(), &all);
}

uint64_t kt_user_peek(uint64_t va)
{
    bool smap = cpu_features.smap;
    if (smap)
        __asm__ volatile("stac" ::: "memory");
    uint64_t v = *(volatile uint64_t *)va;
    if (smap)
        __asm__ volatile("clac" ::: "memory");
    return v;
}

uint64_t kt_rng(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

struct job *kt_fresh_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);   /* j keeps it */
    return j;
}

void kt_job_is_empty(struct job *j)
{
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k))
            ktest_fail("job kind %u still has %lu units", k, job_used(j, k));
}

signals_t kt_signals_of(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    KT_EQ(handle_get(t, h, OBJ_NONE, 0, &obj, NULL), OK);
    signals_t s = kobject_signals(obj);
    kobject_unref(obj);
    return s;
}

struct kobject *kt_pci_dev_res(struct pci_dev *d)
{
    struct kobject *root = resource_root(), *pci, *dev;
    KT_ASSERT(root);
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, d->index, &dev), OK);
    kobject_unref(pci);
    kobject_unref(root);
    return dev;
}

/* ---- QEMU's edu --------------------------------------------------------------------- */

#define EDU_DMA_SRC 0x80   /* the DMA engine's registers (64-bit) */
#define EDU_DMA_DST 0x88
#define EDU_DMA_CNT 0x90
#define EDU_DMA_CMD 0x98
#define EDU_RUN     0x1u   /* command: start; reads 1 while running */
#define EDU_TO_RAM  0x2u   /* command: the buffer -> RAM */

struct pci_dev *kt_edu(void)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d)
        kprintf("ktest %s: no free edu (QEMU's -device edu; from the shell its driver has it), "
                "skipped\n", ktest_current);
    return d;
}

volatile uint8_t *kt_edu_regs(struct pci_dev *d)
{
    uint64_t f = pci_cmd_lock();
    status_t st = pci_enable_memory(d);
    pci_cmd_unlock(f);
    KT_EQ(st, OK);
    return vmm_map_mmio(d->info.bar[0].phys, PAGE_SIZE);
}

void kt_edu_dma_start(volatile uint8_t *r, uint64_t src, uint64_t dst, uint32_t len,
                      bool to_ram)
{
    *(volatile uint64_t *)(r + EDU_DMA_SRC) = src;
    *(volatile uint64_t *)(r + EDU_DMA_DST) = dst;
    *(volatile uint64_t *)(r + EDU_DMA_CNT) = len;
    *(volatile uint64_t *)(r + EDU_DMA_CMD) = EDU_RUN | (to_ram ? EDU_TO_RAM : 0);
}

bool kt_edu_idle(volatile uint8_t *r)
{
    uint64_t end = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (*(volatile uint64_t *)(r + EDU_DMA_CMD) & EDU_RUN) {
        if (uptime_ns() > end)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

bool kt_edu_dma(volatile uint8_t *r, uint64_t src, uint64_t dst, uint32_t len, bool to_ram)
{
    kt_edu_dma_start(r, src, dst, len, to_ram);
    return kt_edu_idle(r);
}

/* ---- the faults VT-d records -------------------------------------------------------- */

#define FAULTS_KEPT 16
static struct vtd_fault_rec kept[FAULTS_KEPT];
static uint32_t nkept;   /* the hook (the fault log thread, one) publishes with release */

static void keep_fault(void *arg)
{
    uint32_t n = __atomic_load_n(&nkept, __ATOMIC_RELAXED);
    if (n == FAULTS_KEPT)
        return;
    kept[n] = *(const struct vtd_fault_rec *)arg;
    __atomic_store_n(&nkept, n + 1, __ATOMIC_RELEASE);
}

void kt_vtd_faults_watch(bool on)
{
    __atomic_store_n(&nkept, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dbg_hooks[DBG_VTD_FAULT], on ? keep_fault : NULL, __ATOMIC_RELEASE);
}

uint32_t kt_vtd_faults_seen(void)
{
    return __atomic_load_n(&nkept, __ATOMIC_ACQUIRE);
}

bool kt_vtd_fault_wait(bool (*match)(const struct vtd_fault_rec *r, const void *arg),
                       const void *arg, struct vtd_fault_rec *out)
{
    uint64_t until = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    uint32_t from = 0;
    do {
        uint32_t n = kt_vtd_faults_seen();
        for (; from < n; from++)
            if (match(&kept[from], arg)) {
                if (out)
                    *out = kept[from];
                return true;
            }
        thread_sleep_ms(2);
    } while (uptime_ns() < until);
    return false;
}
