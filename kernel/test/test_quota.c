/* Job quotas: the fixes for the independent review of M5 phase 2 (its
 * regression tests started in test_review.c, failing; each moved here once
 * its finding was fixed). What they check: every piece of kernel memory a
 * process can make the kernel allocate is charged to a job, and a program
 * can't lift the limits its parent put on it. */
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

static __attribute__((unused)) uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

static struct job *quota_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);
    return j;
}

/* R4. A program gets its own job (SR_JOB) without RIGHT_MANAGE, so the
 * limit its parent set is binding: the child mode "raise-own-limit"
 * (user/utest/child.c) tries to lift JOB_LIMIT_PAGES through SR_JOB and
 * commit 1 MiB; it exits 50 when the kernel refuses the raise (0 would be
 * the bug: it escaped its limit). */
KTEST(quota_child_cannot_raise_own_job_limit)
{
    struct job *j = quota_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 64), OK);   /* a 256 KiB budget */
    const char *argv[] = { "utest", "raise-own-limit" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, NULL, 0, NULL, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    struct job_info ji;
    job_get_info(j, &ji);
    job_unref(j);
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, 50);                  /* the raise was refused */
    KT_EQ(ji.limit[JOB_LIMIT_PAGES], 64);
}
