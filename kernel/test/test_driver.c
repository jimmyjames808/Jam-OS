/* <jam/driver.h> in the kernel build (M6, Track D): drivers as kernel
 * processes (kernel/drivers/driver_kernel.c). The drvtest driver checks the
 * whole driver.h surface from inside a kernel process and calls the null
 * driver (another kernel process) through the generated <idl/null.h>
 * client; the same two drivers run as processes in utest. Every test ends
 * with the drivers' job back at zero on every count: their handles, heap,
 * VMOs, mappings and threads are all gone once they are. */
#include <jam/channel.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/ktest.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <idl/null.h>

#define S            1000000000ull
#define MS           1000000ull
#define DRVTEST_NULL 0x40   /* drvtest's role for its channel to a null server */
#define CH_RIGHTS    (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)

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

/* Wait for p to die; its info. Drops the caller's reference. */
static struct process_info finish(struct process *p)
{
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 30 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_DEAD);
    KT_EQ(info.threads, 0);
    kobject_unref(process_kobject(p));
    return info;
}

static struct driver_kernel_handle role(uint32_t r, struct channel *c)
{
    struct driver_kernel_handle h = { r, khandle_from_new((struct kobject *)c, CH_RIGHTS) };
    return h;
}

static struct process *start(const char *name, struct driver_kernel_handle *h, unsigned n,
                             struct job *j)
{
    driver_main_fn fn = driver_kernel_find(name);
    KT_ASSERT(fn != NULL);
    struct process *p;
    KT_EQ(driver_kernel_start(name, fn, h, n, j, &p), OK);
    return p;
}

/* drvtest against null, both kernel processes; the client going away ends
 * the server. */
KTEST(driver_kernel_drvtest_and_null)
{
    struct job *j = fresh_job();
    uint64_t chans = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct driver_kernel_handle hs = role(DR_SERVE, b), hc = role(DRVTEST_NULL, a);
    struct process *srv = start("null", &hs, 1, j);
    struct process *cli = start("drvtest", &hc, 1, j);
    struct process_info ci = finish(cli), si = finish(srv);
    KT_EQ(ci.killed, 0);
    KT_EQ(ci.exit_code, 0);   /* every drvtest check passed */
    KT_EQ(si.killed, 0);
    KT_EQ(si.exit_code, 0);   /* null saw its client close and returned 0 */
    job_is_empty(j);
    KT_EQ(channel_live_count(), chans);
    job_unref(j);
}

/* Kernel code can speak a protocol with the generated structs directly,
 * and a driver blocked in a wait is killed at once. */
KTEST(driver_kernel_raw_call_and_kill)
{
    struct job *j = fresh_job();
    uint64_t chans = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct driver_kernel_handle hs = role(DR_SERVE, b);
    struct process *srv = start("null", &hs, 1, j);
    struct null_add_req q = { 0, NULL_ADD, 40, 2 };
    struct null_add_rep r;
    uint32_t n = 0;
    KT_EQ(channel_call(a, &q, sizeof(q), NULL, 0, &r, sizeof(r), &n, NULL, 0, NULL,
                       uptime_ns() + 10 * S),
          OK);
    KT_EQ(idl_rep_status(&r, n, sizeof(r)), OK);
    KT_EQ(r.sum, 42);
    thread_sleep_ms(10);   /* back in its wait */
    process_kill(srv, PROCESS_KILLED_CODE, true);
    struct process_info si = finish(srv);
    KT_EQ(si.killed, 1);
    KT_EQ(si.exit_code, PROCESS_KILLED_CODE);
    KT_EQ(object_wait_one((struct kobject *)a, SIG_PEER_CLOSED, uptime_ns() + S, NULL), OK);
    kobject_unref((struct kobject *)a);
    job_is_empty(j);
    KT_EQ(channel_live_count(), chans);
    job_unref(j);
}

/* A kernel driver is charged like a process: a job that can't pay for it
 * refuses it, and a refused start leaves nothing behind (its handles are
 * consumed either way). */
KTEST(driver_kernel_start_refused)
{
    struct job *j = fresh_job();
    uint64_t chans = channel_live_count();
    struct channel *a, *b;
    KT_ASSERT(driver_kernel_find("no-such-driver") == NULL);
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, 0), OK);
    KT_EQ(channel_create(&a, &b), OK);
    struct driver_kernel_handle hs = role(DR_SERVE, b);
    struct process *p = NULL;
    KT_EQ(driver_kernel_start("null", driver_kernel_find("null"), &hs, 1, j, &p),
          ERR_NO_RESOURCES);
    KT_ASSERT(hs.kh.obj == NULL);
    /* Room for the process but not its thread's kernel stack. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, JOB_NO_LIMIT), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 2), OK);
    struct channel *c, *d;
    KT_EQ(channel_create(&c, &d), OK);
    hs = role(DR_SERVE, d);
    KT_EQ(driver_kernel_start("null", driver_kernel_find("null"), &hs, 1, j, &p), ERR_NO_MEMORY);
    job_is_empty(j);
    KT_EQ(object_wait_one((struct kobject *)c, SIG_PEER_CLOSED, uptime_ns() + S, NULL), OK);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)c);
    KT_EQ(channel_live_count(), chans);
    job_unref(j);
}
