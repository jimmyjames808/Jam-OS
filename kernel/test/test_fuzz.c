/* Hostile programs against the kernel (M12.1, the kernel core's check):
 * bin/utest's fuzz modes (user/tests/utest/fuzz.c) run in a job of their
 * own, with limits, from twelve threads at once: random system calls with
 * random arguments, and channel ends raced through writes, calls, replies,
 * closes, marks and port bindings. Each run ends with the program exiting
 * while its threads are inside their calls, or killed from here at a
 * random moment. The kernel must survive it, and once the program is dead
 * its job must be back to zero on every count, and no channel end, port
 * or binding it made may be left (the harness checks the pages). The
 * seed is printed: `ktest=fuzz_` with the same build repeats a run only
 * as far as the threads' interleaving does. */
#include <jam/channel.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/port.h>
#include <jam/process.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/x86.h>

#define FZ_RUN_MS 1500   /* how long a run that ends itself goes on */

static struct job *fz_job(void)
{
    struct job *j = kt_fresh_job();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 8192), OK);       /* 32 MiB */
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, 4096), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_THREADS, 64), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_MSG_BYTES, 8u << 20), OK);   /* 8 MiB */
    return j;
}

static struct process *fz_start(struct job *j, const char *mode, uint64_t seed, uint64_t ms)
{
    static char s_seed[24], s_ms[24];
    ksnprintf(s_seed, sizeof(s_seed), "%lu", seed);
    ksnprintf(s_ms, sizeof(s_ms), "%lu", ms);
    const char *argv[] = { "utest", mode, s_seed, s_ms };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 4, j, NULL, 0, NULL, &p), OK);
    return p;
}

/* Wait for p's end (killing it if it outlives its own deadline), then
 * check that nothing it made is left. */
static void fz_finish(struct process *p, struct job *j, uint64_t live_ch, uint64_t live_ports,
                      uint64_t live_bindings)
{
    status_t st = object_wait_one(process_kobject(p), SIG_TERMINATED,
                                  uptime_ns() + kt_patience_ms(20000) * NS_PER_MS, NULL);
    if (st != OK) {
        kprintf("fuzz: the program outlived its run: killed\n");
        process_kill(p, PROCESS_KILLED_CODE, true);
        KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED,
                              uptime_ns() + kt_patience_ms(20000) * NS_PER_MS, NULL),
              OK);
    }
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_DEAD);
    KT_EQ(info.threads, 0);
    kobject_unref(process_kobject(p));
    /* Its processes in a job of its own (the pool's child job) die with it;
     * whatever is still being given back settles within a moment. Live,
     * the system-wide counts move with everything else running and are
     * not checked (KT_GLOBAL_EQ): waiting for them to come back would
     * only wait out the bound (2 s, a minute under load: most of the
     * soak's slowest tests), so only the job's own are waited for there. */
    job_kill(j, NULL);
    uint64_t end = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    struct port_stats ps;
    for (;;) {
        port_get_stats(&ps);
        bool settled = ktest_live || (channel_live_count() == live_ch &&
                                      ps.ports == live_ports && ps.bindings == live_bindings);
        for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
            settled &= job_used(j, k) == 0;
        if (settled || uptime_ns() >= end)
            break;
        thread_sleep_ms(5);
    }
    kt_job_is_empty(j);
    KT_GLOBAL_EQ(channel_live_count(), live_ch);
    KT_GLOBAL_EQ(ps.ports, live_ports);
    KT_GLOBAL_EQ(ps.bindings, live_bindings);
}

/* One run of `mode`: it ends itself after FZ_RUN_MS (kill_ms 0), or is
 * killed from here after kill_ms. */
static void fz_run(const char *mode, uint64_t seed, uint64_t kill_ms)
{
    uint64_t live_ch = channel_live_count();
    struct port_stats ps;
    port_get_stats(&ps);
    struct job *j = fz_job();
    kprintf("fuzz: %s seed %lu, %s\n", mode, seed, kill_ms ? "killed" : "exits by itself");
    struct process *p = fz_start(j, mode, seed, kill_ms ? 60000 : FZ_RUN_MS);
    if (kill_ms) {
        thread_sleep_ms(kill_ms);
        process_kill(p, PROCESS_KILLED_CODE, true);
    }
    fz_finish(p, j, live_ch, ps.ports, ps.bindings);
    job_unref(j);
}

static uint64_t fz_seed(void)
{
    uint64_t s = rdtsc() | 1;
    return kt_rng(&s) % 1000000000u + 1;
}

KTEST(fuzz_syscalls_many_threads)
{
    fz_run("fuzz-calls", fz_seed(), 0);
}

KTEST(fuzz_syscalls_killed_mid_call)
{
    uint64_t s = fz_seed();
    fz_run("fuzz-calls", s, 200 + kt_rng(&s) % 1000);
}

KTEST(fuzz_channels_race_closes)
{
    fz_run("fuzz-chans", fz_seed(), 0);
}

KTEST(fuzz_channels_killed_mid_call)
{
    uint64_t s = fz_seed();
    fz_run("fuzz-chans", s, 200 + kt_rng(&s) % 1000);
}
