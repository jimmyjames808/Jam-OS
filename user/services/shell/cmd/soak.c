/* soak: the kernel tests over and over in a shuffled order on a busy
 * machine, until a time is up, then one summary.
 *
 *   soak [minutes] [loops=N] [seed=S] [load=N] [halt] [idle]
 *
 * Each loop is `ktest loops=1 seed=<the next seed> keep load` (the kernel's
 * own load: the stress test's threads and processes) and then `utest`. All
 * the while bin/soakload runs in the background: files written, read back
 * and deleted on /data and on every other writable stick, files of
 * read-only mounts read, memory mapped and unmapped, channel calls,
 * programs started. Sticks may be pulled and plugged while it runs. It
 * stops after `minutes` (default 3; the loop in progress finishes) or after
 * N loops if loops= is given, or on Ctrl+C between two steps.
 *
 * Before the load starts and again after it has stopped, one more shuffled
 * loop runs without any load: the tests that need an idle machine only run
 * there, and the last one sees whatever the soak left behind (it is the
 * plain live `ktest` after utest, stick pulls and load).
 *
 *   seed=S   the first loop's seed (default: from the clock); the kernel
 *            prints each loop's, and `ktest seed=<it>` replays that loop
 *   halt     the first failure stops the machine on the panic screen (what
 *            the boot menu's Soak entry does), instead of being recorded
 *   load=N   N kernel load workers instead of two per CPU (QEMU: fewer)
 *   idle     no load at all: only the repeated, shuffled tests and utest
 *
 * The summary (SOAK RESULTS) comes from the kernel: kernel/test/ktest_soak.c. */
#include <soakload.h>
#include "sh.h"

#define DEFAULT_MINUTES 3
#define STOP_WAIT_NS    (150 * NS_PER_S)   /* soakload's workers finish a file cycle first */

struct soak {
    uint64_t minutes, loops, seed;   /* loops 0: by the clock */
    uint64_t workers;                /* kernel load workers; 0: two per CPU */
    bool     halt, idle;
    handle_t load_job, load_proc, load_ctl, load_ns;   /* bin/soakload, or 0 */
    uint64_t utest_runs, utest_failed;
    struct soakload_result load;     /* its counts, once stopped */
    bool     load_lost;              /* soakload never answered */
};

static bool num_after(const char *arg, const char *key, uint64_t *v)
{
    size_t n = strlen(key);
    return !strncmp(arg, key, n) && sh_parse_u64(arg + n, v);
}

static bool parse(int argc, char **argv, struct soak *s)
{
    s->minutes = DEFAULT_MINUTES;
    s->seed = ((now() ^ (now() >> 29)) & 0x7fffffff) | 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "halt"))
            s->halt = true;
        else if (!strcmp(argv[i], "idle"))
            s->idle = true;
        else if (num_after(argv[i], "loops=", &s->loops) && s->loops >= 1 && s->loops <= 100000)
            ;
        else if (num_after(argv[i], "seed=", &s->seed) && s->seed >= 1 && s->seed <= 0x7fffffff)
            ;
        else if (num_after(argv[i], "load=", &s->workers) && s->workers >= 2 &&
                 s->workers <= 1024)
            ;
        else if (sh_parse_u64(argv[i], &s->minutes) && s->minutes >= 1 && s->minutes <= 600)
            ;
        else
            return false;
    }
    return true;
}

/* Start bin/soakload in a job of its own with a control channel and a
 * namespace channel (mounts that come back are sent to it between loops). */
static void start_load(struct soak *s)
{
    handle_t theirs;
    if (jam_job_create(startup_handle(SR_JOB), 0, &s->load_job) != OK)
        return;
    if (jam_channel_create(&s->load_ctl, &theirs) != OK) {
        jam_handle_close(s->load_job);
        s->load_job = s->load_ctl = HANDLE_INVALID;
        return;
    }
    struct spawn_handle x = { SR_USER, theirs };
    const char *args[] = { "soakload" };
    struct spawn_args a = {
        .path = "bin/soakload", .argc = 1, .argv = args, .job = s->load_job, .extra = &x,
        .nextra = 1, .ns = NS_ALL, .ns_out = &s->load_ns,
    };
    status_t st = spawn(&a, &s->load_proc);
    if (st != OK) {
        sh_tty("soak: can't start bin/soakload (%s): going on without the user-space load\n",
               status_str(st));
        jam_handle_close(s->load_ctl);
        jam_handle_close(s->load_job);
        s->load_job = s->load_ctl = s->load_proc = HANDLE_INVALID;
    }
}

/* Tell it to stop, take its counts, and end its job. */
static void stop_load(struct soak *s)
{
    if (!s->load_proc)
        return;
    uint32_t n = 0;
    uint8_t go = 1;
    struct channel_read_args a = {
        .h = s->load_ctl, .bytes_cap = sizeof(s->load), .bytes = (uint64_t)(uintptr_t)&s->load,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    jam_channel_write(s->load_ctl, &go, 1, NULL, 0);
    jam_object_wait_one(s->load_ctl, SIG_READABLE | SIG_PEER_CLOSED, now() + STOP_WAIT_NS, NULL);
    if (jam_channel_read(&a) != OK || n != sizeof(s->load)) {
        sh_tty("soak: bin/soakload did not answer: counted as a failure\n");
        memset(&s->load, 0, sizeof(s->load));
        s->load_lost = true;
    }
    spawn_wait(s->load_proc, 5 * NS_PER_S, NULL);
    jam_job_kill(s->load_job);
    jam_handle_close(s->load_proc);
    jam_handle_close(s->load_ctl);
    jam_handle_close(s->load_job);
    if (s->load_ns)
        jam_handle_close(s->load_ns);
}

/* The kernel tests once, in the next seed's order; busy: under the kernel's
 * load. false: the kernel refused, or Ctrl+C. */
static bool ktest_once(struct soak *s, bool busy)
{
    char cmd[64], load[16] = "";
    if (busy && s->workers)
        snprintf(load, sizeof(load), " load=%lu", (unsigned long)s->workers);
    else if (busy)
        snprintf(load, sizeof(load), " load");
    snprintf(cmd, sizeof(cmd), "ktest loops=1 seed=%lu%s%s", (unsigned long)s->seed++,
             s->halt ? "" : " keep", load);
    return sh_kcmd(cmd) >= 0 && !sh_interrupted();
}

/* One loop: the kernel tests, then utest. false: stop here. */
static bool one_loop(struct soak *s)
{
    if (!ktest_once(s, !s->idle))
        return false;
    if (s->load_ns)
        ns_send(s->load_ns, NS_ALL);   /* a stick plugged back in is a new mount */
    char name[] = "utest";
    char *argv[] = { name, NULL };
    s->utest_runs++;
    if (sh_run_test_program(1, argv) != 0) {
        s->utest_failed++;
        if (s->halt)
            return false;
    }
    return !sh_interrupted();
}

SH_CMD(soak)
{
    struct soak s = { 0 };
    if (!parse(argc, argv, &s)) {
        sh_tty("usage: soak [minutes] [loops=N] [seed=S] [load=N] [halt] [idle]\n");
        return 2;
    }
    if (sh_kcmd("soak begin") < 0)
        return 1;
    if (s.loops)
        sh_say("soak: %lu loop(s), first seed %lu%s%s\n", (unsigned long)s.loops,
               (unsigned long)s.seed, s.idle ? ", no load" : ", under load",
               s.halt ? ", halting on the first failure" : "");
    else
        sh_say("soak: %lu minute(s), first seed %lu%s%s (Ctrl+C ends it after the step in "
               "progress)\n", (unsigned long)s.minutes, (unsigned long)s.seed,
               s.idle ? ", no load" : ", under load",
               s.halt ? ", halting on the first failure" : "");
    sh_flush();
    /* An idle loop, the loops under load, an idle loop. */
    bool go = s.idle || ktest_once(&s, false);
    if (!s.idle)
        start_load(&s);
    uint64_t end = now() + s.minutes * 60 * NS_PER_S;
    for (uint64_t loop = 0; go && (s.loops ? loop < s.loops : now() < end); loop++)
        go = one_loop(&s);
    stop_load(&s);
    if (go && !s.idle)
        ktest_once(&s, false);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "soak end u=%lu,%lu io=%lu,%lu%s", (unsigned long)s.utest_runs,
             (unsigned long)s.utest_failed, (unsigned long)s.load.file_cycles,
             (unsigned long)(s.load.failed + s.load_lost), s.halt ? " halt" : "");
    int64_t failures = sh_kcmd(cmd);
    if (s.load_proc)
        sh_say("soak: user load: %lu file cycles, %lu read-only passes, %lu cut short, %lu "
               "memory rounds, %lu calls, %lu spawns\n", (unsigned long)s.load.file_cycles,
               (unsigned long)s.load.read_passes, (unsigned long)s.load.cut_short,
               (unsigned long)s.load.memory_rounds, (unsigned long)s.load.calls,
               (unsigned long)s.load.spawns);
    sh_say("shell: soak: %s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures != 0;
}
