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

/* R3. A job costs its parent a handle unit and jobs nest at most
 * JOB_MAX_DEPTH deep. The review's attack: job_create(cur) -> close cur ->
 * repeat (sysc_job_create + sysc_handle_close), holding one handle to the
 * bottom of an ever longer chain. Now the chain stops at the depth cap, the
 * whole chain is charged to the top job, and a charge at the bottom walks
 * at most JOB_MAX_DEPTH levels. */
KTEST(quota_job_chain_bounded)
{
    struct job *top = quota_job();   /* depth 1 (the root job is 0) */
    uint64_t before = free_now();
    struct job *cur = top;
    job_ref(cur);
    unsigned made = 0;
    status_t st;
    for (;;) {
        struct job *next;
        st = job_create(cur, &next);
        if (st != OK)
            break;
        job_unref(cur);   /* "close the handle": next keeps it alive */
        cur = next;
        made++;
        KT_ASSERT(made < 100000);
    }
    KT_EQ(st, ERR_OUT_OF_RANGE);
    KT_EQ(made, JOB_MAX_DEPTH - 2);   /* depths 2 .. JOB_MAX_DEPTH - 1 */
    KT_EQ(job_used(top, JOB_LIMIT_HANDLES), made);   /* every job below top */
    uint64_t used = before - free_now();
    uint64_t t0 = uptime_ns();
    KT_EQ(job_charge(cur, JOB_LIMIT_HANDLES, 1), OK);
    uint64_t charge_ns = uptime_ns() - t0;
    job_uncharge(cur, JOB_LIMIT_HANDLES, 1);
    kprintf("quota: %u-deep job chain behind one reference: %lu kernel pages, %lu handle "
            "units charged, one charge at the bottom took %lu ns\n", made, used,
            job_used(top, JOB_LIMIT_HANDLES), charge_ns);
    job_unref(cur);   /* the whole chain goes, crediting top */
    KT_EQ(job_used(top, JOB_LIMIT_HANDLES), 0);
    job_unref(top);
    KT_ASSERT(used <= 16);

    /* The handle unit is a real limit: a job allowed 2 units has room for
     * exactly two child jobs. */
    struct job *j = quota_job(), *a, *b, *c;
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, 2), OK);
    KT_EQ(job_create(j, &a), OK);
    KT_EQ(job_create(j, &b), OK);
    KT_EQ(job_create(j, &c), ERR_NO_RESOURCES);
    job_unref(a);
    job_unref(b);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 0);
    job_unref(j);
}
