/* Helpers shared by the kernel tests (declared in ktest.h), and the table
 * of debug hooks (jam/dbghook.h) the scheduler race tests install. */
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/sched.h>
#include <jam/userboot.h>

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
