/* Processes, user threads and jobs (M5 phase 2), from the kernel side:
 * job accounting on its own, then real user programs started with
 * userboot_spawn (bin/utest in one of its child modes, see
 * user/utest/child.c): ELF loading, the startup message, exit codes,
 * fault kills, a kill in the middle of channel_call, job limits. Each
 * program runs in a job of its own, which must be back to zero on every
 * count once the program is dead (and the ktest harness checks no page
 * leaked). */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/bootfs.h>
#include <jam/channel.h>
#include <jam/elf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>

#define S 1000000000ull

extern volatile uint64_t user_faults;

static struct job *fresh_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);   /* j keeps it */
    return j;
}

static void job_is_empty(struct job *j)
{
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k))
            panic("ktest %s: job kind %u still has %lu units", ktest_current, k,
                  job_used(j, k));
}

/* Start "utest <mode> [arg]" in j with an optional extra handle. */
static struct process *start(struct job *j, const char *mode, const char *arg,
                             struct khandle *extra)
{
    const char *argv[] = { "utest", mode, arg };
    struct userboot_handle x = { SR_USER, { NULL, 0 } };
    if (extra) {
        x.kh = *extra;
        extra->obj = NULL;
    }
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, arg ? 3 : 2, j, extra ? &x : NULL, extra ? 1 : 0,
                         NULL, &p),
          OK);
    return p;
}

/* Wait for p to die; its info. Drops the caller's reference. */
static struct process_info finish(struct process *p)
{
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_DEAD);
    KT_EQ(info.threads, 0);
    kobject_unref(process_kobject(p));
    return info;
}

KTEST(proc_job_hierarchy_limits)
{
    struct job *parent, *child;
    KT_EQ(job_create(NULL, &parent), OK);
    KT_EQ(job_create(parent, &child), OK);
    KT_EQ(job_set_limit(parent, JOB_LIMIT_PAGES, 10), OK);
    KT_EQ(job_set_limit(child, 0, 1), ERR_INVALID_ARGS);
    KT_EQ(job_set_limit(child, JOB_LIMIT_COUNT, 1), ERR_INVALID_ARGS);
    /* The child has no limit of its own, but can't go past its parent's. */
    KT_EQ(job_charge(child, JOB_LIMIT_PAGES, 8), OK);
    KT_EQ(job_charge(child, JOB_LIMIT_PAGES, 3), ERR_NO_MEMORY);
    KT_EQ(job_used(child, JOB_LIMIT_PAGES), 8);    /* a failed charge leaves nothing */
    KT_EQ(job_used(parent, JOB_LIMIT_PAGES), 8);
    KT_EQ(job_charge(parent, JOB_LIMIT_PAGES, 2), OK);
    KT_EQ(job_charge(child, JOB_LIMIT_PAGES, 1), ERR_NO_MEMORY);
    /* Its own limit applies too, and handles/threads fail with NO_RESOURCES. */
    KT_EQ(job_set_limit(child, JOB_LIMIT_THREADS, 1), OK);
    KT_EQ(job_charge(child, JOB_LIMIT_THREADS, 1), OK);
    KT_EQ(job_charge(child, JOB_LIMIT_THREADS, 1), ERR_NO_RESOURCES);
    KT_EQ(job_used(parent, JOB_LIMIT_THREADS), 1);
    struct job_info ji;
    job_get_info(child, &ji);
    KT_EQ(ji.used[JOB_LIMIT_PAGES], 8);
    KT_EQ(ji.limit[JOB_LIMIT_THREADS], 1);
    KT_EQ(ji.limit[JOB_LIMIT_PAGES], JOB_NO_LIMIT);
    job_uncharge(child, JOB_LIMIT_THREADS, 1);
    job_uncharge(child, JOB_LIMIT_PAGES, 8);
    job_uncharge(parent, JOB_LIMIT_PAGES, 2);
    KT_EQ(job_used(parent, JOB_LIMIT_HANDLES), 1);   /* the child job itself */
    KT_EQ(job_charge(NULL, JOB_LIMIT_PAGES, 1000), OK);   /* kernel objects: never charged */
    job_unref(child);
    job_is_empty(parent);
    job_unref(parent);
}

/* The ELF went where the plan says: text straight from bootfs (the same
 * physical page, read-only and executable), data in a copy, the stack with
 * a guard below it. */
KTEST(proc_elf_load_layout)
{
    struct job *j = fresh_job();
    struct process *p = start(j, "spin", NULL, NULL);
    struct aspace *as = process_aspace(p);
    KT_ASSERT(as);
    KT_EQ(aspace_mapping_count(as), 5);   /* text, rodata, data, guard, stack */
    const void *img;
    uint64_t size;
    struct elf_plan plan;
    KT_EQ(bootfs_data("bin/utest", &img, &size), OK);
    KT_EQ(elf_parse(img, size, &plan), OK);
    const struct elf_segment *text = &plan.seg[0];
    KT_EQ(text->flags, ASPACE_READ | ASPACE_EXEC);
    /* Fault the first text page in from the kernel, as the CPU would: it
     * is the bootfs page itself. */
    uint64_t va = ALIGN_DOWN(text->vaddr, PAGE_SIZE);
    KT_EQ(aspace_fault(as, va, ASPACE_EXEC), OK);
    uint64_t pte = aspace_pte(as, va);
    KT_ASSERT(pte & 1);
    KT_ASSERT(!(pte & 2));                                 /* never writable */
    KT_EQ(pte & 0x000ffffffffff000ull,
          virt_to_phys((const uint8_t *)img + ALIGN_DOWN(text->file_off, PAGE_SIZE)));
    KT_EQ(aspace_protect(as, va, PAGE_SIZE, ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC),
          ERR_INVALID_ARGS);                                          /* W^X */
    KT_EQ(aspace_protect(as, va, PAGE_SIZE, ASPACE_READ | ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, va, PAGE_SIZE, ASPACE_READ), OK);   /* can drop exec... */
    KT_EQ(aspace_fault(as, va, ASPACE_WRITE), ERR_ACCESS_DENIED); /* ...never gain write */
    /* The data segment is a copy: a different page. */
    const struct elf_segment *data = &plan.seg[plan.nseg - 1];
    KT_ASSERT(data->flags & ASPACE_WRITE);
    uint64_t dva = ALIGN_DOWN(data->vaddr, PAGE_SIZE);
    KT_EQ(aspace_fault(as, dva, ASPACE_WRITE), OK);
    KT_ASSERT((aspace_pte(as, dva) & 0x000ffffffffff000ull) !=
              virt_to_phys((const uint8_t *)img + ALIGN_DOWN(data->file_off, PAGE_SIZE)));
    KT_EQ(aspace_fault(as, USERBOOT_STACK_TOP - (USERBOOT_STACK_PAGES + 1) * PAGE_SIZE,
                       ASPACE_READ),
          ERR_ACCESS_DENIED);                             /* the guard page */
    aspace_unref(as);
    process_kill(p, PROCESS_KILLED_CODE, true);
    struct process_info info = finish(p);
    KT_ASSERT(info.killed);
    job_is_empty(j);
    job_unref(j);
}

KTEST(proc_exit_code_and_startup_message)
{
    struct job *j = fresh_job();
    struct process_info info = finish(start(j, "exit7", NULL, NULL));
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, 7);
    info = finish(start(j, "startup", "hello", NULL));
    KT_EQ(info.exit_code, 0);   /* the child checked argv, handles, bootfs rights */
    info = finish(start(j, "main-exits", NULL, NULL));
    KT_EQ(info.exit_code, 11);
    job_is_empty(j);
    job_unref(j);
}

KTEST(proc_fault_kills_the_process)
{
    struct job *j = fresh_job();
    uint64_t faults = user_faults;
    struct process_info info = finish(start(j, "nullderef", NULL, NULL));
    KT_ASSERT(info.killed);
    KT_EQ(info.exit_code, PROCESS_KILLED_CODE);
    info = finish(start(j, "execdata", NULL, NULL));
    KT_ASSERT(info.killed);
    KT_EQ(user_faults, faults + 2);
    job_is_empty(j);
    job_unref(j);
}

/* The milestone check: a process blocked in channel_call on a server that
 * never answers is killed, and every page, handle and thread comes back. */
KTEST(proc_kill_in_channel_call_cleans_up)
{
    struct job *j = fresh_job();
    struct channel *mine, *theirs;
    KT_EQ(channel_create(&mine, &theirs), OK);
    struct khandle kh = khandle_from_new((struct kobject *)theirs, RIGHTS_BASIC | RIGHTS_IO);
    struct process *p = start(j, "caller", NULL, &kh);
    /* Its request arriving means it is (about to be) blocked in the call. */
    KT_EQ(object_wait_one((struct kobject *)mine, SIG_READABLE, uptime_ns() + 10 * S, NULL), OK);
    thread_sleep_ms(20);
    KT_EQ(job_used(j, JOB_LIMIT_THREADS), 2);
    KT_ASSERT(job_used(j, JOB_LIMIT_PAGES) >= 16);
    KT_ASSERT(job_used(j, JOB_LIMIT_HANDLES) >= 5);
    KT_ASSERT(job_used(j, JOB_LIMIT_MSG_BYTES) > 0);
    process_kill(p, PROCESS_KILLED_CODE, true);
    struct process_info info = finish(p);
    KT_ASSERT(info.killed);
    KT_EQ(job_used(j, JOB_LIMIT_THREADS), 0);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 0);
    /* Its request is still queued on our end, charged to it until read. */
    uint8_t buf[64];
    uint32_t nb;
    KT_EQ(channel_read(mine, buf, sizeof(buf), &nb, NULL, 0, NULL), OK);
    KT_EQ(nb, 16);
    KT_EQ(channel_read(mine, buf, sizeof(buf), &nb, NULL, 0, NULL), ERR_PEER_CLOSED);
    job_is_empty(j);
    kobject_unref((struct kobject *)mine);
    job_unref(j);
}

KTEST(proc_runaway_hits_its_job_limit)
{
    struct job *j = fresh_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 256), OK);
    struct process_info info = finish(start(j, "hog", NULL, NULL));
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, 42);   /* it got ERR_NO_MEMORY from vmo_commit */
    job_is_empty(j);
    info = finish(start(j, "hogfault", NULL, NULL));
    KT_ASSERT(info.killed);       /* a page fault it can't pay for: killed, no panic */
    job_is_empty(j);
    /* The program's own data and stack are charged to its job too (userboot
     * made them for it): with no pages at all it either can't be loaded
     * (its .data copy) or dies on its first stack touch. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 0), OK);
    const char *argv[] = { "utest", "exit7" };
    struct process *p;
    status_t st = userboot_spawn("bin/utest", argv, 2, j, NULL, 0, NULL, &p);
    if (st == OK)
        KT_ASSERT(finish(p).killed);
    else
        KT_EQ(st, ERR_NO_MEMORY);
    job_is_empty(j);
    job_unref(j);
}

/* Kill processes spinning in user mode on several CPUs at once: each is
 * stopped at its next timer tick and torn down. */
KTEST(proc_kill_spinning_processes)
{
    enum { N = 6 };
    struct job *j = fresh_job();
    struct process *ps[N];
    for (int i = 0; i < N; i++)
        ps[i] = start(j, "spin", NULL, NULL);
    thread_sleep_ms(30);
    for (int i = 0; i < N; i++)
        process_kill(ps[i], PROCESS_KILLED_CODE, true);
    for (int i = 0; i < N; i++)
        KT_ASSERT(finish(ps[i]).killed);
    job_is_empty(j);
    job_unref(j);
}

/* A process nobody starts: its last handle going makes it unstartable and
 * everything it had is freed with its last reference. */
KTEST(proc_never_started)
{
    struct job *j = fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "never", &p), OK);
    struct uthread *u;
    KT_EQ(uthread_create(p, "t", &u), OK);
    struct khandle kh = khandle_from_new(process_kobject(p), PROCESS_RIGHTS);
    khandle_release(&kh);   /* the last handle: now it can never start */
    struct khandle none = { NULL, 0 };
    KT_EQ(process_start(p, u, 0x400000, 0, &none, 0, NULL), ERR_BAD_STATE);
    kobject_unref(uthread_kobject(u));   /* its reference on p was the last */
    job_is_empty(j);
    job_unref(j);
}
