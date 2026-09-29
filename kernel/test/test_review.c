/* Independent review of M5 phase 2 (processes, syscalls, jobs): regression
 * tests for the bugs the review found. Each one FAILS (panics) on the code
 * it was written against; it passes once the bug is fixed.
 *
 * They are NOT part of the default "ktest" run (ktest_run skips names that
 * start with "review_" unless the prefix asks for them). Run them with
 *     ktest=review_            all of them
 *     ktest=review_vmo_tables  one of them (any prefix works)
 * e.g. make image && QEMU_SMP=4 QEMU_TIMEOUT=240 tools/qemu-test.sh build/r rev ktest=review_
 *
 * Every test cleans up after itself, so the harness's page-leak check stays
 * meaningful; the failure is the explicit assertion. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>

#define S 1000000000ull

static uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

static struct job *review_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);
    return j;
}

/* R1. A VMO's own page-table pages (root/mid/leaf, kernel memory) are not
 * charged to any job, and slot_locked() creates them BEFORE the page charge
 * is tried, so they stay even when the charge fails. A process whose job
 * may commit 0 pages makes a 64 GiB VMO and asks for one page every 2 MiB:
 * every commit fails with ERR_NO_MEMORY, yet each leaves a 4 KiB leaf table
 * behind: 128 MiB of kernel memory per VMO handle, with nothing charged.
 * (sysc_vmo_create -> vmo_set_job(t->job), sysc_vmo_commit -> vmo_commit do
 * exactly what this test does.) */
KTEST(review_vmo_tables_uncharged)
{
    struct job *j = review_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 0), OK);
    struct vmo *v;
    KT_EQ(vmo_create(VMO_MAX_SIZE, 0, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);
    uint64_t before = free_now();
    unsigned refused = 0;
    for (uint64_t off = 0; off < VMO_MAX_SIZE; off += 2ull << 20)
        if (vmo_commit(v, off, PAGE_SIZE) == ERR_NO_MEMORY)
            refused++;
    uint64_t used = before - free_now();
    kprintf("review: %u commits refused, job charged %lu pages, kernel spent %lu pages\n",
            refused, job_used(j, JOB_LIMIT_PAGES), used);
    KT_EQ(refused, VMO_MAX_SIZE / (2ull << 20));   /* the job refused every page... */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    uint64_t spent = used;
    kobject_unref(vmo_kobject(v));   /* frees the tables: no leak for the harness */
    job_unref(j);
    KT_ASSERT(spent <= 16);          /* ...so the kernel must not have spent them either */
}

/* R2. User page tables (aspace.c pt_prepare: PT/PD/PDPT pages) are not
 * charged to anyone either. One committed (charged) page, mapped at 1 GiB
 * strides, costs a PD and a PT page per mapping on the first touch: here
 * 512 mappings (MAX_MAPPINGS is 16384) = ~1024 uncharged kernel pages for
 * one charged page, per address space; every process_create makes one. */
KTEST(review_aspace_tables_uncharged)
{
    enum { N = 512 };
    struct job *j = review_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 1), OK);
    struct process *p;
    KT_EQ(process_create(j, "review-pt", &p), OK);
    struct aspace *as = process_aspace(p);
    struct vmo *v;
    KT_EQ(vmo_create(PAGE_SIZE, 0, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);
    uint64_t before = free_now();
    for (uint64_t i = 0; i < N; i++) {
        uint64_t addr = (1ull << 32) + i * (1ull << 30);   /* 1 GiB apart */
        KT_EQ(aspace_map(as, v, 0, PAGE_SIZE,
                         ASPACE_READ | ASPACE_WRITE | ASPACE_CAN_READ | ASPACE_CAN_WRITE |
                             ASPACE_FIXED,
                         &addr),
              OK);
        KT_EQ(aspace_fault(as, addr, ASPACE_WRITE), OK);   /* what the user's touch does */
    }
    uint64_t used = before - free_now();
    uint64_t charged = job_used(j, JOB_LIMIT_PAGES);
    kprintf("review: %u mappings, job charged %lu page(s), kernel spent %lu pages "
            "(%lu page-table pages)\n", N, charged, used, aspace_pt_pages(as));
    kobject_unref(vmo_kobject(v));
    aspace_unref(as);
    process_kill(p, PROCESS_KILLED_CODE, true);   /* never started: torn down here */
    kobject_unref(process_kobject(p));
    job_unref(j);
    KT_EQ(charged, 1);
    KT_ASSERT(used <= charged + 16);   /* fails: ~2 * N uncharged table pages */
}
