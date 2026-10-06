/* Stress test. Threads of seven kinds hammer the scheduler, locks and
 * allocators at mixed priorities for a set time while the main thread
 * checks TLB shootdowns once a second and prints progress every 10 s.
 * Every check failure is counted; any failure fails the run.
 *
 * The "process" kind starts user programs (bin/utest in a child mode) and
 * kills them at random moments (before they run, while they spin in user
 * mode, while they are blocked in channel_call), then checks that each
 * one's job ends with nothing charged. Without a bootfs holding bin/utest
 * those workers count instead.
 *
 * The same workers also run as a background load (stress_load_start and
 * stress_load_stop) while something else is tested: the kernel tests under
 * `ktest load` and the shell's `soak`. */
#include <jam/bootfs.h>
#include <jam/channel.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>

enum kind { K_COUNTER, K_ALLOC, K_SLEEPER, K_MIGRATOR, K_PINGPONG, K_SPAWNER, K_PROCESS,
            K_KINDS };
static const char *const kind_names[K_KINDS] = {
    "counter", "alloc", "sleeper", "migrator", "pingpong", "spawner", "process",
};

struct pingpong {
    spinlock_t       lock;   /* guards turn */
    struct waitqueue wq;     /* the two sides wait here */
    int              turn;   /* 0 or 1: whose move it is */
};

struct worker {
    enum kind         kind;              /* what this worker does */
    uint32_t          index;             /* its slot in the worker table */
    uint64_t          seed;              /* its random number state */
    uint64_t          local_count;       /* counter: increments this thread made */
    struct pingpong  *pp;                /* pingpong: the shared state */
    int               side;              /* pingpong: 0 or 1 */
    struct thread    *thread;            /* the worker thread */
    uint64_t          last_progress_ns;  /* last time it finished a step (the stuck check) */
};

static bool stop;                  /* the workers stop at their next step */
static uint64_t failures;          /* checks that failed */
static uint64_t ops[K_KINDS];      /* steps done, by kind */
static struct mutex counter_mutex;
static uint64_t counter;   /* protected by counter_mutex */

static uint64_t op_count(enum kind k)
{
    return __atomic_load_n(&ops[k], __ATOMIC_RELAXED);
}

static uint64_t rnd(struct worker *w)
{
    w->seed ^= w->seed << 13;
    w->seed ^= w->seed >> 7;
    w->seed ^= w->seed << 17;
    return w->seed;
}

static void fail(const char *what, const struct worker *w)
{
    if (__atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED) <= 10)
        report("stress: FAILED %s (thread %s#%u)", what, kind_names[w->kind], w->index);
}

static void do_counter(struct worker *w)
{
    mutex_lock(&counter_mutex);
    uint64_t v = counter;
    if (rnd(w) % 64 == 0)
        thread_yield();   /* get preempted inside the critical section */
    counter = v + 1;
    mutex_unlock(&counter_mutex);
    w->local_count++;
}

static void do_alloc(struct worker *w)
{
    enum { N = 16 };
    uint8_t *p[N];
    uint32_t sz[N];
    uint8_t tag = (uint8_t)(w->index * 13);
    for (int i = 0; i < N; i++) {
        sz[i] = 1 + rnd(w) % ((i & 3) ? 400 : 6000);
        p[i] = kmalloc(sz[i]);
        if (!p[i]) {
            fail("kmalloc returned NULL", w);
            sz[i] = 0;
            continue;
        }
        memset(p[i], tag + i, sz[i]);
    }
    if (rnd(w) % 8 == 0)
        thread_yield();
    for (int i = 0; i < N; i++) {
        if (!p[i])
            continue;
        for (uint32_t b = 0; b < sz[i]; b += 37)
            if (p[i][b] != (uint8_t)(tag + i)) {
                fail("heap block overwritten", w);
                break;
            }
        kfree(p[i]);
    }
}

static void do_sleeper(struct worker *w)
{
    uint64_t ms = rnd(w) % 30;
    uint64_t t0 = uptime_ns();
    thread_sleep_ms(ms);
    if (uptime_ns() - t0 < ms * 1000000)
        fail("woke before its sleep ended", w);
}

static void do_migrator(struct worker *w)
{
    uint32_t target = (uint32_t)(rnd(w) % cpu_count);
    cpumask_t m;
    cpumask_one(&m, target);
    thread_set_affinity(current_thread(), &m);
    preempt_disable();
    uint32_t now = this_cpu()->index;
    preempt_enable();
    if (now != target)
        fail("still on the wrong CPU after migrating", w);
    cpumask_all(&m);
    thread_set_affinity(current_thread(), &m);
}

static void do_pingpong(const struct worker *w)
{
    struct pingpong *pp = w->pp;
    uint64_t f = spin_lock_irqsave(&pp->lock);
    while (pp->turn != w->side && !__atomic_load_n(&stop, __ATOMIC_RELAXED))
        waitqueue_wait(&pp->wq, &pp->lock, &f);
    pp->turn = !w->side;
    spin_unlock_irqrestore(&pp->lock, f);
    waitqueue_wake_all(&pp->wq);
}

static void short_life(void *arg)
{
    uint64_t *x = arg;
    __atomic_add_fetch(x, 1, __ATOMIC_RELAXED);   /* several run at once */
}

static void do_spawner(struct worker *w)
{
    uint64_t hits = 0;
    struct thread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create("short-life", short_life, (void *)&hits, 8 + (int)(rnd(w) % 16));
    for (int i = 0; i < 4; i++)
        thread_join(t[i]);
    if (__atomic_load_n(&hits, __ATOMIC_RELAXED) != 4)
        fail("short-lived thread did not run exactly once", w);
}

static bool have_utest;
static struct job *stress_job;

/* One user program from start to death, killed at a random moment (or
 * left to exit), then its job must be empty. */
static void do_process(struct worker *w)
{
    static const char *const modes[] = { "exit7", "spin", "caller", "main-exits", "spin" };
    const char *mode = modes[rnd(w) % 5];
    struct job *j;
    if (job_create(stress_job, &j) != OK) {
        fail("job_create", w);
        return;
    }
    struct channel *mine = NULL, *theirs;
    struct userboot_handle x = { SR_USER, { NULL, 0 } };
    bool call = !strcmp(mode, "caller");
    if (call) {
        if (channel_create(&mine, &theirs) != OK) {
            fail("channel_create", w);
            job_unref(j);
            return;
        }
        x.kh = khandle_from_new((struct kobject *)theirs, RIGHTS_BASIC | RIGHTS_IO);
    }
    const char *argv[] = { "utest", mode };
    struct process *p;
    status_t st = userboot_spawn("bin/utest", argv, 2, j, call ? &x : NULL, call ? 1 : 0, NULL,
                                 &p);
    if (st != OK) {
        fail("userboot_spawn", w);
    } else {
        bool kill = !strcmp(mode, "spin") || call;
        if (call && rnd(w) % 2)   /* sometimes wait until it is blocked in the call */
            object_wait_one((struct kobject *)mine, SIG_READABLE, uptime_ns() + 5000000000ull,
                            NULL);
        else if (kill && rnd(w) % 4)
            thread_sleep_ns(rnd(w) % 3000000);
        if (kill)
            process_kill(p, PROCESS_KILLED_CODE, true);
        if (object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 30000000000ull,
                            NULL) != OK) {
            fail("user process did not die", w);
        } else {
            struct process_info info;
            process_get_info(p, &info);
            if (!kill && (info.killed || info.exit_code != (mode[0] == 'e' ? 7 : 11))) {
                report("stress: user process \"%s\" exited with code %ld%s (expected %d)",
                       mode, (long)info.exit_code, info.killed ? ", killed" : "",
                       mode[0] == 'e' ? 7 : 11);
                fail("user process exited with the wrong code", w);
            }
        }
        kobject_unref(process_kobject(p));
    }
    if (mine)
        kobject_unref((struct kobject *)mine);   /* drops any queued request too */
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k))
            fail("a dead process left something charged to its job", w);
    job_unref(j);
}

static void worker_main(void *arg)
{
    struct worker *w = arg;
    while (!__atomic_load_n(&stop, __ATOMIC_RELAXED)) {
        switch (w->kind) {
        case K_COUNTER:  do_counter(w);  break;
        case K_ALLOC:    do_alloc(w);    break;
        case K_SLEEPER:  do_sleeper(w);  break;
        case K_MIGRATOR: do_migrator(w); break;
        case K_PINGPONG: do_pingpong(w); break;
        case K_SPAWNER:  do_spawner(w);  break;
        case K_PROCESS:  do_process(w);  break;
        default: break;
        }
        __atomic_add_fetch(&ops[w->kind], 1, __ATOMIC_RELAXED);
        __atomic_store_n(&w->last_progress_ns, uptime_ns(), __ATOMIC_RELAXED);
    }
}

/* TLB shootdown under load: same check as the self-test, once per call. */
static volatile uint64_t *shoot_va;
static uint64_t shoot_expect, shoot_bad;   /* the value to see; CPUs that saw another */

static void shoot_read(void *arg)
{
    (void)arg;
    if (*shoot_va != __atomic_load_n(&shoot_expect, __ATOMIC_RELAXED))
        __atomic_add_fetch(&shoot_bad, 1, __ATOMIC_RELAXED);
}

static void shootdown_round(uint64_t va, uint64_t round)
{
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    if (!pa)
        return;
    __atomic_store_n(&shoot_expect, 0x5000 + round, __ATOMIC_RELAXED);
    *(uint64_t *)phys_to_virt(pa) = 0x5000 + round;
    vmm_map(vmm_kernel_pml4(), va, pa, PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
    smp_call_all(shoot_read, NULL);
    vmm_unmap(vmm_kernel_pml4(), va, PAGE_SIZE);
    pmm_free_page_phys(pa);
}

/* Give each of the n workers its kind, its pingpong partner and its seed. */
static void assign_kinds(struct worker *ws, struct pingpong *pps, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        struct worker *w = &ws[i];
        w->kind = (enum kind)(i % K_KINDS);
        w->index = i;
        w->seed = 0x9e3779b97f4a7c15ull ^ ((uint64_t)i * 0x2545f4914f6cdd1dull);
    }
    /* Pair up pingpong threads in order; an unpaired last one would wait
     * forever, so it becomes a counter instead. */
    struct worker *waiting = NULL;
    uint32_t pairs = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ws[i].kind != K_PINGPONG)
            continue;
        if (!waiting) {
            waiting = &ws[i];
            continue;
        }
        struct pingpong *pp = &pps[pairs++];
        spin_init(&pp->lock, "stress pingpong");
        waitqueue_init(&pp->wq, "stress pingpong wq");
        waiting->pp = ws[i].pp = pp;
        waiting->side = 0;
        ws[i].side = 1;
        waiting = NULL;
    }
    if (waiting)
        waiting->kind = K_COUNTER;
    const void *img;
    uint64_t isz;
    have_utest = bootfs_data("bin/utest", &img, &isz) == OK &&
                 userboot_root_job(&stress_job) == OK;
    for (uint32_t i = 0; i < n; i++)
        if (ws[i].kind == K_PROCESS && !have_utest)
            ws[i].kind = K_COUNTER;
}

static void start_workers(struct worker *ws, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        __atomic_store_n(&ws[i].last_progress_ns, uptime_ns(), __ATOMIC_RELAXED);
        char name[24];
        ksnprintf(name, sizeof(name), "%s#%u", kind_names[ws[i].kind], i);
        ws[i].thread = thread_create(name, worker_main, &ws[i], 8 + (int)(ws[i].seed % 17));
    }
}

/* Starvation: every thread must finish an operation every 10 s.
 * Low-priority threads behind CPU-bound higher ones only run when
 * boosted (about once a second), and a spawner waits on four such
 * children, so a few seconds is legitimate; forever is not. */
static void check_progress(const struct worker *ws, uint32_t n)
{
    uint64_t now = uptime_ns();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t last = __atomic_load_n(&ws[i].last_progress_ns, __ATOMIC_RELAXED);
        if (last >= now || now - last <= 10000000000ull)
            continue;
        report("stress: FAILED thread %s#%u (prio %d) made no progress for %lu s",
                kind_names[ws[i].kind], i, ws[i].thread->base_prio,
                (now - __atomic_load_n(&ws[i].last_progress_ns, __ATOMIC_RELAXED)) /
                    1000000000);
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    }
}

static void print_progress(uint64_t sec)
{
    uint64_t sw = 0, steals = 0, t, fr;
    for (uint32_t i = 0; i < cpu_count; i++) {
        sw += __atomic_load_n(&cpus[i]->switches, __ATOMIC_RELAXED);
        steals += __atomic_load_n(&cpus[i]->steals, __ATOMIC_RELAXED);
    }
    pmm_stats(&t, &fr);
    kprintf("stress: %4lu s  switches %lu  steals %lu  counter %lu  allocs %lu  "
            "sleeps %lu  migrations %lu  pingpongs %lu  spawns %lu  processes %lu  "
            "boosts %lu  free %lu MiB\n",
            sec, sw, steals, op_count(K_COUNTER), op_count(K_ALLOC),
            op_count(K_SLEEPER), op_count(K_MIGRATOR), op_count(K_PINGPONG),
            op_count(K_SPAWNER), op_count(K_PROCESS),
            sched_boost_count(), fr >> 8);
}

/* The run itself: once a second a TLB shootdown round and the checks,
 * every 10 s a progress line; stops early at the first failure. */
static void run_seconds(const struct worker *ws, uint32_t n, uint64_t seconds, uint64_t va,
                        uint64_t start)
{
    for (uint64_t sec = 1; sec <= seconds && !__atomic_load_n(&failures, __ATOMIC_RELAXED); sec++) {
        while (uptime_ns() - start < sec * 1000000000ull)
            thread_sleep_ms(50);
        shootdown_round(va, sec);
        DBG_HOOK(DBG_STRESS_SHOOTDOWN, &shoot_bad);
        if (__atomic_load_n(&shoot_bad, __ATOMIC_RELAXED)) {
            /* A hook set here is a test faking the stale read (ktest
             * stress_failure_does_not_stick): say so, so the RESULTS box
             * doesn't read as a real stale mapping. */
            bool faked = DBG_HOOK_SET(DBG_STRESS_SHOOTDOWN);
            report("stress: FAILED TLB shootdown: a CPU saw a stale mapping%s",
                   faked ? " (faked on purpose by a test: not a real failure)" : "");
            __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
        }
        if (sec >= 10)
            check_progress(ws, n);
        if (sec % 10 == 0 || sec == seconds)
            print_progress(sec);
    }
}

static void stop_workers(struct worker *ws, struct pingpong *pps, uint32_t n)
{
    __atomic_store_n(&stop, true, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n / 2 + 1; i++)
        if (pps[i].lock.name)
            waitqueue_wake_all(&pps[i].wq);
    for (uint32_t i = 0; i < n; i++)
        thread_join(ws[i].thread);
}

/* The mutex check: the shared counter equals what the threads counted. */
static void check_counter(const struct worker *ws, uint32_t n)
{
    uint64_t expect = 0;
    for (uint32_t i = 0; i < n; i++)
        expect += ws[i].local_count;
    if (counter != expect) {
        report("stress: FAILED mutex: counter %lu but threads counted %lu", counter, expect);
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    }
}

/* The user processes' job must be empty once they are all gone. */
static void check_job_empty(void)
{
    if (stress_job) {
        for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
            if (job_used(stress_job, k)) {
                report("stress: FAILED user processes left %lu units of job kind %u",
                       job_used(stress_job, k), k);
                __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
            }
        job_unref(stress_job);
        stress_job = NULL;
    }
}

/* The workers of the run in progress (one at a time: stress_run from the
 * boot's main thread or a debug command, which are never two at once). */
static struct worker *workers;
static struct pingpong *pingpongs;
static uint32_t nworkers;

/* Start n workers from a clean slate. */
static void workers_begin(uint32_t n)
{
    mutex_init(&counter_mutex, "stress counter");
    counter = 0;
    __atomic_store_n(&stop, false, __ATOMIC_RELAXED);
    /* Each run starts clean: the shell's `stress` can run many times in one
     * boot, and one failed run must not fail all the later ones. */
    __atomic_store_n(&failures, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&shoot_bad, 0, __ATOMIC_RELAXED);
    for (unsigned k = 0; k < K_KINDS; k++)
        __atomic_store_n(&ops[k], 0, __ATOMIC_RELAXED);
    nworkers = n;
    workers = kzalloc(sizeof(*workers) * n);
    pingpongs = kzalloc(sizeof(*pingpongs) * (n / 2 + 1));
    assign_kinds(workers, pingpongs, n);
    start_workers(workers, n);
    shoot_va = (volatile uint64_t *)vmm_reserve(PAGE_SIZE);
}

/* Stop them and make the end-of-run checks; how many checks failed in all. */
static uint64_t workers_end(void)
{
    stop_workers(workers, pingpongs, nworkers);
    check_counter(workers, nworkers);
    kfree(workers);
    kfree(pingpongs);
    workers = NULL;
    pingpongs = NULL;
    vmm_release((uint64_t)shoot_va, PAGE_SIZE);   /* unmapped after every round */
    shoot_va = NULL;
    check_job_empty();
    return __atomic_load_n(&failures, __ATOMIC_RELAXED);
}

bool stress_run(uint64_t seconds)
{
    uint32_t n = cpu_count * 4;
    kprintf("stress: %u threads on %u CPUs for %lu s, lock checking on\n", n, cpu_count,
            seconds);
    uint64_t total, free_before, free_after;
    pmm_stats(&total, &free_before);

    workers_begin(n);
    uint64_t start = uptime_ns();
    run_seconds(workers, n, seconds, (uint64_t)shoot_va, start);
    uint64_t failed = workers_end();

    pmm_stats(&total, &free_after);
    kprintf("stress: %lu KiB not returned (thread stacks are kept for reuse)\n",
            (free_before - free_after) * 4);
    bool faked = DBG_HOOK_SET(DBG_STRESS_SHOOTDOWN);
    report("stress: %s after %lu s (%lu failures)%s", failed ? "FAILED" : "PASSED",
           (uptime_ns() - start) / 1000000000, failed,
           failed && faked ? " (on purpose: a test's faked failure)" : "");
    return failed == 0;
}

/* ---- the background load ------------------------------------------------------- */

#define LOAD_ROUND_MS 250   /* a TLB shootdown round this often */

static struct thread *load_thread;
static bool load_stop;
static uint64_t load_start_ns;

/* What stress_run's own loop does once a second, four times as often:
 * shootdown rounds (IPIs to every CPU), and the starvation check. */
static void load_main(void *arg)
{
    (void)arg;
    uint64_t round = 0;
    while (!__atomic_load_n(&load_stop, __ATOMIC_ACQUIRE)) {
        thread_sleep_ms(LOAD_ROUND_MS);
        shootdown_round((uint64_t)shoot_va, ++round);
        if (__atomic_exchange_n(&shoot_bad, 0, __ATOMIC_RELAXED)) {
            report("stress: FAILED TLB shootdown: a CPU saw a stale mapping (load round %lu)",
                   round);
            __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
        }
        if (round % (1000 / LOAD_ROUND_MS) == 0 && uptime_ns() - load_start_ns > 10000000000ull)
            check_progress(workers, nworkers);
    }
}

bool stress_load_start(uint32_t n)
{
    if (load_thread || workers || n < 2)
        return false;
    kprintf("stress: background load: %u threads on %u CPUs (counters, allocations, sleeps, "
            "migrations, thread and process churn, TLB shootdowns)\n", n, cpu_count);
    workers_begin(n);
    load_start_ns = uptime_ns();
    __atomic_store_n(&load_stop, false, __ATOMIC_RELEASE);
    load_thread = thread_create("stress-load", load_main, NULL, PRIO_DEFAULT + 4);
    return true;
}

uint64_t stress_load_stop(void)
{
    if (!load_thread)
        return 0;
    __atomic_store_n(&load_stop, true, __ATOMIC_RELEASE);
    thread_join(load_thread);
    load_thread = NULL;
    print_progress((uptime_ns() - load_start_ns) / 1000000000);
    uint64_t failed = workers_end();
    kprintf("stress: background load stopped: %lu failures\n", failed);
    return failed;
}
