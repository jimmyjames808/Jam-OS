/* Job quotas: every piece of kernel memory a process can make the kernel
 * allocate is charged to a job, and a program can't lift the limits its
 * parent put on it. Each test names the attack it stops. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/event.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>


/* A program gets its own job (SR_JOB) without RIGHT_MANAGE, so the
 * limit its parent set is binding: the child mode "raise-own-limit"
 * (user/utest/child.c) tries to lift JOB_LIMIT_PAGES through SR_JOB and
 * commit 1 MiB; it exits 50 when the kernel refuses the raise (0 would be
 * the bug: it escaped its limit). */
KTEST(quota_child_cannot_raise_own_job_limit)
{
    struct job *j = kt_fresh_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 64), OK);   /* a 256 KiB budget */
    const char *argv[] = { "utest", "raise-own-limit" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, NULL, 0, NULL, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20 * NS_PER_S, NULL),
          OK);
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

/* A job costs its parent a handle unit and jobs nest at most
 * JOB_MAX_DEPTH deep. The attack: job_create(cur) -> close cur ->
 * repeat (sysc_job_create + sysc_handle_close), holding one handle to the
 * bottom of an ever longer chain. Now the chain stops at the depth cap, the
 * whole chain is charged to the top job, and a charge at the bottom walks
 * at most JOB_MAX_DEPTH levels. */
KTEST(quota_job_chain_bounded)
{
    struct job *top = kt_fresh_job();   /* depth 1 (the root job is 0) */
    uint64_t before = kt_free_pages();
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
    uint64_t used = before - kt_free_pages();
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
    KT_GLOBAL_ASSERT(used <= 16);

    /* The handle unit is a real limit: a job allowed 2 units has room for
     * exactly two child jobs. */
    struct job *j = kt_fresh_job(), *a, *b, *c;
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, 2), OK);
    KT_EQ(job_create(j, &a), OK);
    KT_EQ(job_create(j, &b), OK);
    KT_EQ(job_create(j, &c), ERR_NO_RESOURCES);
    job_unref(a);
    job_unref(b);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 0);
    job_unref(j);
}

/* A VMO's own table pages (mid and leaf) are charged to its job like
 * its pages, and a commit the job refuses builds no table. The attack: a job allowed 0 pages asks for one page every 2 MiB of a 64 GiB
 * VMO; each refused commit used to leave a 4 KiB leaf behind (128 MiB of
 * kernel memory per VMO handle, nothing charged). */
KTEST(quota_vmo_tables_charged)
{
    struct job *j = kt_fresh_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 0), OK);
    struct vmo *v;
    KT_EQ(vmo_create(VMO_MAX_SIZE, 0, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);
    uint64_t before = kt_free_pages();
    unsigned refused = 0;
    for (uint64_t off = 0; off < VMO_MAX_SIZE; off += 2ull << 20)
        if (vmo_commit(v, off, PAGE_SIZE) == ERR_NO_MEMORY)
            refused++;
    uint64_t used = before - kt_free_pages();
    kprintf("quota: %u commits refused, job charged %lu pages, kernel spent %lu pages\n",
            refused, job_used(j, JOB_LIMIT_PAGES), used);
    KT_EQ(refused, VMO_MAX_SIZE / (2ull << 20));   /* the job refused every page... */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    kobject_unref(vmo_kobject(v));
    KT_GLOBAL_ASSERT(used <= 16);          /* ...so the kernel didn't spend them either */

    /* The first page of a VMO costs 3 (itself, a mid table, a leaf); the
     * next one in the same 2 MiB costs 1; one in a new 2 MiB costs 2. With
     * room for 4, that one is refused and leaves nothing behind. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 4), OK);
    KT_EQ(vmo_create(8ull << 20, 0, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);
    KT_EQ(vmo_commit(v, 0, PAGE_SIZE), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 3);
    KT_EQ(vmo_commit(v, PAGE_SIZE, PAGE_SIZE), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4);
    KT_EQ(vmo_commit(v, 4ull << 20, PAGE_SIZE), ERR_NO_MEMORY);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4);
    KT_EQ(vmo_decommit(v, 0, 2 * PAGE_SIZE), OK);   /* the pages go, the tables stay */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2);
    KT_EQ(vmo_commit(v, 4ull << 20, PAGE_SIZE), OK);   /* page + leaf: 4 again */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4);
    KT_EQ(vmo_set_size(v, 2ull << 20), OK);   /* the leaf past the end goes with its page */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2);
    kobject_unref(vmo_kobject(v));
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    /* Tables made before the job was set are charged when it is. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    KT_EQ(vmo_create(PAGE_SIZE, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, PAGE_SIZE), OK);
    KT_EQ(vmo_set_job(v, j), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 3);
    kobject_unref(vmo_kobject(v));
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    job_unref(j);
}

static void map_one(struct aspace *as, struct vmo *v, uint64_t addr, status_t want)
{
    KT_EQ(aspace_map(as, v, 0, PAGE_SIZE,
                     ASPACE_READ | ASPACE_WRITE | ASPACE_CAN_READ | ASPACE_CAN_WRITE |
                         ASPACE_FIXED,
                     &addr),
          want);
}

/* A process's address space charges its job for its PML4, every user
 * page-table page and the mapping structs. The attack: one
 * committed page mapped at 1 GiB strides costs a PD and a PT page per
 * mapping on the first touch (512 mappings = ~1024 table pages for one
 * charged page; MAX_MAPPINGS is 16384). Now the job pays for them, so a
 * job with room for 64 pages stops the attack at 64 pages of kernel
 * memory, and everything is credited back when the address space goes. */
KTEST(quota_aspace_tables_charged)
{
    enum { N = 512, LIMIT = 64 };
    struct job *j = kt_fresh_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, LIMIT), OK);
    struct process *p;
    KT_EQ(process_create(j, "quota-pt", &p), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1);   /* the PML4 */
    struct aspace *as = process_aspace(p);
    struct vmo *v;
    KT_EQ(vmo_create(PAGE_SIZE, 0, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);

    /* The exact costs: a mapping (a page per 16 of them), a first touch
     * (its page + mid + leaf in the VMO; PDPT + PD + PT here), and unmap
     * gives the tables back. */
    uint64_t base = 1ull << 32;
    map_one(as, v, base, OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2);
    KT_EQ(aspace_fault(as, base, ASPACE_WRITE), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2 + 3 + 3);
    KT_EQ(aspace_unmap(as, base, PAGE_SIZE), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1 + 3);   /* the VMO keeps its page */

    uint64_t before = kt_free_pages(), charged0 = job_used(j, JOB_LIMIT_PAGES);
    unsigned i;
    status_t st = OK;
    for (i = 0; i < N; i++) {
        uint64_t addr = base + i * (1ull << 30);   /* 1 GiB apart */
        st = aspace_map(as, v, 0, PAGE_SIZE,
                        ASPACE_READ | ASPACE_WRITE | ASPACE_CAN_READ | ASPACE_CAN_WRITE |
                            ASPACE_FIXED,
                        &addr);
        if (st == OK)
            st = aspace_fault(as, addr, ASPACE_WRITE);   /* what the user's touch does */
        if (st != OK)
            break;
    }
    uint64_t used = before - kt_free_pages();
    uint64_t charged = job_used(j, JOB_LIMIT_PAGES);
    kprintf("quota: stopped after %u of %u mappings (%s), job charged %lu page(s), kernel "
            "spent %lu pages (%lu page-table pages)\n", i, N, status_str(st), charged, used,
            aspace_pt_pages(as));
    KT_EQ(st, ERR_NO_MEMORY);
    KT_ASSERT(i < N / 8);
    KT_ASSERT(charged <= LIMIT);
    KT_GLOBAL_ASSERT(used <= charged - charged0 + 16);   /* nothing big left uncharged */

    /* A split the job can't pay for fails before changing anything. */
    uint64_t big = 1ull << 44;
    struct vmo *v2;
    KT_EQ(vmo_create(64 * PAGE_SIZE, 0, &v2), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    uint64_t addr = big;
    KT_EQ(aspace_map(as, v2, 0, 64 * PAGE_SIZE, ASPACE_READ | ASPACE_CAN_READ | ASPACE_FIXED,
                     &addr),
          OK);
    uint32_t n = aspace_mapping_count(as);
    for (big += 64 * PAGE_SIZE; n % ASPACE_MAPPINGS_PER_PAGE; n++, big += PAGE_SIZE)
        map_one(as, v2, big, OK);   /* fill the last charged page of mappings */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, job_used(j, JOB_LIMIT_PAGES)), OK);
    map_one(as, v2, big, ERR_NO_MEMORY);
    KT_EQ(aspace_unmap(as, addr + PAGE_SIZE, PAGE_SIZE), ERR_NO_MEMORY);      /* a hole */
    KT_EQ(aspace_protect(as, addr + PAGE_SIZE, PAGE_SIZE, 0), ERR_NO_MEMORY);  /* 2 splits */
    KT_EQ(aspace_mapping_count(as), n);   /* nothing changed */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, job_used(j, JOB_LIMIT_PAGES) + 1), OK);
    KT_EQ(aspace_protect(as, addr + PAGE_SIZE, PAGE_SIZE, 0), OK);
    KT_EQ(aspace_mapping_count(as), n + 2);
    KT_EQ(aspace_unmap(as, addr, 64 * PAGE_SIZE), OK);   /* back down: credited */
    KT_EQ(aspace_mapping_count(as), n - 1);
    kobject_unref(vmo_kobject(v2));

    kobject_unref(vmo_kobject(v));
    aspace_unref(as);
    process_kill(p, PROCESS_KILLED_CODE, true);   /* never started: torn down here */
    kobject_unref(process_kobject(p));
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    job_unref(j);
}

/* The rest of a process's kernel memory: the process object is a
 * handle unit, and a started thread costs UTHREAD_KMEM_PAGES (its kernel
 * stack and XSAVE area) on top of its THREADS unit. A start the job can't
 * pay for fails cleanly: the process is NEW again, the startup handle is
 * the caller's again, nothing stays charged. */
KTEST(quota_process_and_thread_charged)
{
    struct job *j = kt_fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "quota-thread", &p), OK);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);   /* the process */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1);     /* its PML4 */
    struct uthread *u;
    KT_EQ(uthread_create(p, "t", &u), OK);
    struct event *e;
    KT_EQ(event_create(&e), OK);
    struct khandle arg0 = khandle_from_new(&e->base, RIGHTS_BASIC);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 1 + UTHREAD_KMEM_PAGES - 1), OK);
    KT_EQ(process_start(p, u, 0x400000, 0x800000, &arg0, 0, NULL), ERR_NO_MEMORY);
    KT_ASSERT(arg0.obj == &e->base);   /* given back */
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_NEW);
    KT_EQ(info.threads, 0);
    KT_EQ(job_used(j, JOB_LIMIT_THREADS), 0);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);   /* the slot it had went back */
    khandle_release(&arg0);
    kobject_unref(uthread_kobject(u));
    process_kill(p, PROCESS_KILLED_CODE, true);   /* never started: torn down here */
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 0);   /* at teardown, not at the last reference */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    kobject_unref(process_kobject(p));
    job_unref(j);
}

/* The root job's handle and message budgets come out of its page
 * budget: everything a job can make the kernel hold fits in free memory
 * next to the kernel's reserve. */
KTEST(quota_root_job_budget)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    struct job *r;
    KT_EQ(userboot_root_job(&r), OK);
    struct job_info ji;
    job_get_info(r, &ji);
    job_unref(r);
    uint64_t worst = ji.limit[JOB_LIMIT_PAGES] +
                     ji.limit[JOB_LIMIT_HANDLES] * JOB_OBJECT_BYTES / PAGE_SIZE +
                     ji.limit[JOB_LIMIT_MSG_BYTES] / PAGE_SIZE;
    kprintf("quota: root job: %lu pages + %lu handle units + %lu message bytes = %lu of %lu "
            "free pages\n", ji.limit[JOB_LIMIT_PAGES], ji.limit[JOB_LIMIT_HANDLES],
            ji.limit[JOB_LIMIT_MSG_BYTES], worst, free);
    KT_ASSERT(ji.limit[JOB_LIMIT_HANDLES] >= 4096);
    KT_GLOBAL_ASSERT(worst + (free / 4 < 8192 ? free / 4 : 8192) <= free + 64);   /* +: pmm noise */
}
