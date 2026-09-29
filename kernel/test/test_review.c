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

/* R3. Jobs cost nothing: job_create is charged to no resource, and a child
 * job keeps its parent alive, so a process that holds ONE handle can build
 * a chain of any length (job_create(cur) -> close cur -> repeat; exactly
 * what these calls do for sysc_job_create + sysc_handle_close). The chain
 * is unbounded kernel memory for a single handle unit, and every
 * job_charge/credit in the deepest job walks the whole chain (lock-free,
 * but under VMO / handle-table / channel spinlocks with interrupts off), so
 * a deep enough chain also stretches every allocation of that process into
 * a long interrupts-off section. */
KTEST(review_job_chain_uncharged)
{
    enum { DEPTH = 100000 };
    struct job *top = review_job();
    uint64_t before = free_now();
    struct job *cur = top;
    job_ref(cur);
    for (unsigned i = 0; i < DEPTH; i++) {
        struct job *next;
        KT_EQ(job_create(cur, &next), OK);
        job_unref(cur);   /* "close the handle": next keeps it alive */
        cur = next;
    }
    uint64_t used = before - free_now();
    uint64_t t0 = uptime_ns();
    KT_EQ(job_charge(cur, JOB_LIMIT_HANDLES, 1), OK);
    uint64_t charge_ns = uptime_ns() - t0;
    job_uncharge(cur, JOB_LIMIT_HANDLES, 1);
    kprintf("review: %u-deep job chain behind one reference: %lu kernel pages, "
            "one charge at the bottom took %lu us\n", DEPTH, used, charge_ns / 1000);
    job_unref(cur);   /* the whole chain goes (iteratively) */
    job_unref(top);
    /* One handle unit's worth of objects should not cost ~2000 pages. A
     * fix charges jobs (e.g. as handles/objects of the parent's job) and/or
     * bounds the depth. */
    KT_ASSERT(used <= 16);
}

/* R4. A process can raise its own job's limits. userboot (and libos spawn,
 * which duplicates the caller's job handle with RIGHT_SAME) hands every
 * program its own job as SR_JOB with RIGHT_WRITE, and job_set_limit needs
 * only RIGHT_WRITE, so the job limit a parent sets is advisory: the child
 * lifts it and allocates past it (up to the ancestors' limits; init holds
 * the ROOT job this way and can remove the kernel's reserve). The child
 * mode "review-raise" (user/utest/child.c) raises JOB_LIMIT_PAGES on
 * SR_JOB and commits 1 MiB; it exits 0 if both worked. */
KTEST(review_child_raises_own_job_limit)
{
    struct job *j = review_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 64), OK);   /* a 256 KiB budget */
    const char *argv[] = { "utest", "review-raise" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, NULL, 0, NULL, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    struct job_info ji;
    job_get_info(j, &ji);
    job_unref(j);
    kprintf("review: child exit %ld (0 = raised its own limit and used 256 pages), "
            "job limit now %lx\n", info.exit_code, ji.limit[JOB_LIMIT_PAGES]);
    KT_ASSERT(!info.killed);
    KT_ASSERT(info.exit_code != 0);             /* fails: the child escaped its limit */
    KT_EQ(ji.limit[JOB_LIMIT_PAGES], 64);
}
