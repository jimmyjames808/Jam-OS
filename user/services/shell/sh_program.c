/* Starting programs: `run`, a program's name typed as a command, demo,
 * and the test programs (utest, usbtest).
 *
 * Where a program is: a bare name is /boot/bin/<name>; anything with a '/'
 * is a bootfs name ("bin/x") or a path on any mount. A program on /boot is
 * started from the boot image itself (libos's spawn maps its code from the
 * image); one on another mount is read through the namespace, and the
 * kernel refuses its code for now (<os.h>, spawn_args.path).
 *
 * What a program gets: the shell's namespace as it is then (every mount
 * the shell has), a PROGRAM-level console channel of its own
 * (console.new_client: write, keys while it runs, the screen; no input
 * sources, no new channels), the mixer's `audio` channel (SR_AUDIO: it
 * can play sound), and nothing of devmgr's unless it is a test (the utest,
 * usbtest, hdatest and mixtest commands: test suites that kill and rebind
 * drivers get the query and control channels, and the mixer's control
 * channel; mixtest, which kills the mixer, also init's control channel as
 * SR_USER + 3, the shell's own number for it), and nothing of the music
 * player's unless it is jamjar, the player's window, which gets a
 * duplicate of the shell's client end as SR_USER + 4 (the `jamjar`
 * command; `run jamjar` doesn't). Ctrl+C reaches the shell even while the
 * program holds the keys (the console sees to that) and kills it. When it
 * ends its job is killed: anything it started goes with it, so its console
 * channel never outlives it in the foreground. */
#include <idl/console.h>
#include "sh.h"

/* One round of drain: a count and a time, so a program writing flat out
 * can't keep wait_program from Ctrl+C and from seeing the program end. */
#define DRAIN_BUDGET    64
#define DRAIN_BUDGET_NS (20 * NS_PER_MS)
#define DRAIN_HANDLES   4   /* slots to take a message's handles into, to close them */

/* Take the next message off out and throw it away: n bytes and nh handles
 * (what a read too small for it reported), its handles closed. */
static void drop(handle_t out, uint32_t n, uint32_t nh)
{
    char *big = malloc(n ? n : 1);
    handle_t *hs = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = out, .bytes_cap = n, .bytes = (uint64_t)(uintptr_t)big,
        .actual_bytes = (uint64_t)(uintptr_t)&n2, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = nh, .actual_handles = (uint64_t)(uintptr_t)&nh2,
    };
    if (big && hs && jam_channel_read(&a) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            jam_handle_close(hs[i]);
    free(big);
    free(hs);
}

/* Copy what the program wrote to its SR_STDOUT channel into our output,
 * one round of it. A message printf wouldn't send (over 4096 bytes, or
 * carrying handles) is dropped, its handles closed: left at the head of the
 * queue it would fail every later read. True if more may be queued; false
 * once the queue is empty or gone, or Ctrl+C was pressed. */
static bool drain(handle_t out)
{
    char buf[4096];
    uint64_t t0 = now();
    for (unsigned i = 0; i < DRAIN_BUDGET && now() - t0 < DRAIN_BUDGET_NS; i++) {
        if (sh_interrupted())
            return false;
        handle_t hs[DRAIN_HANDLES];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = out, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = DRAIN_HANDLES, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            drop(out, n, nh);
            continue;
        }
        if (st != OK)
            return false;
        for (uint32_t k = 0; k < nh; k++)
            jam_handle_close(hs[k]);
        if (!nh)
            sh_put(buf, n);
    }
    return true;
}

/* What spawn should be given for argv0, into path: "utest" -> bin/utest;
 * "bin/x" (a bootfs name) as it is; else a path through the cwd, as a
 * bootfs name if it is on /boot ("/boot/bin/x", "../drv/x") and absolute
 * otherwise ("/data/x"). false (said) if there is no such program. */
static bool find_program(const char *argv0, char *path, size_t cap)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    bool boot = bootfs_default(&fs) == OK, dir = false;
    if (!strchr(argv0, '/')) {
        snprintf(path, cap, "bin/%s", argv0);
    } else if (boot && bootfs_lookup(fs, argv0, &data, &size) == OK) {
        snprintf(path, cap, "%s", argv0);
    } else {
        char abs[SH_PATH_MAX];
        const char *name = sh_resolve(argv0, abs, sizeof(abs)) ? sh_bootfs_name(abs) : NULL;
        snprintf(path, cap, "%s", name ? name : abs);
    }
    if (path[0] != '/') {
        if (!boot)
            sh_tty("run: no bootfs\n");
        else if (bootfs_lookup(fs, path, &data, &size) != OK)
            sh_tty("run: no %s in bootfs\n", path);
        return boot && bootfs_lookup(fs, path, &data, &size) == OK;
    }
    status_t st = sh_stat(path, &dir, &size);
    if (st != OK || dir)
        sh_tty("run: %s: %s\n", path, st == OK ? "is a directory" : sh_why(st));
    return st == OK && !dir;
}

#define RUN_TEST    1u   /* devmgr's channels and the mixer's control channel */
#define RUN_INITCTL 2u   /* init's control channel too */
#define RUN_MUSIC   4u   /* the music player's channel (jamjar) */
#define RUN_HANDLES 8    /* the most program_handles gives */

/* A duplicate of h as `role` into x[*nx], if there is an h. */
static void give(struct spawn_handle *x, unsigned *nx, uint32_t role, handle_t h)
{
    handle_t d;
    if (h && jam_handle_duplicate(h, RIGHT_SAME, &d) == OK)
        x[(*nx)++] = (struct spawn_handle){ role, d };
}

/* The handles it starts with (see the top); *out_r: its stdout's read end
 * when we are in a pipe. The number of them. */
static unsigned program_handles(struct spawn_handle *x, unsigned how, handle_t *out_r)
{
    unsigned nx = 0;
    bool test = how & RUN_TEST;
    handle_t h, out_w;
    give(x, &nx, SR_AUDIO, sh_audio());
    if (test)
        give(x, &nx, SR_AUDIO_CTL, sh_audio_ctl());
    if (how & RUN_INITCTL)
        give(x, &nx, SR_USER + 3, sh_initctl());
    if (how & RUN_MUSIC)
        give(x, &nx, SR_USER + 4, sh_music());
    if (test && sh_devmgr() && jam_handle_duplicate(sh_devmgr(), RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_DEVMGR, h };
    if (test && sh_devmgr_ctl() && jam_handle_duplicate(sh_devmgr_ctl(), RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_DEVMGR_CTL, h };
    if (console_new_client_until(sh_console(), now() + 5 * NS_PER_S, 2, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, h };
    /* In a pipe: its printf goes down a channel to us (libos printf.c). */
    if (sh_piped() && jam_channel_create(out_r, &out_w) == OK)
        x[nx++] = (struct spawn_handle){ SR_STDOUT, out_w };
    return nx;
}

/* Wait for it to end, copying its output and killing its job on Ctrl+C. */
static status_t wait_program(handle_t proc, handle_t job, handle_t out_r, const char *path,
                             struct process_info *info)
{
    status_t st;
    bool killed = false;
    while ((st = spawn_wait(proc, 50 * NS_PER_MS, info)) == ERR_TIMED_OUT) {
        if (out_r)
            drain(out_r);
        if (sh_interrupted() && !killed) {
            sh_tty("^C: killing %s\n", path);
            sh_flush();
            jam_job_kill(job);
            killed = true;
        }
    }
    if (out_r) {
        /* What is left: the queue is capped (1024 messages), so this ends
         * unless something the program left running keeps writing, which
         * the guard (and Ctrl+C) cut short. */
        for (unsigned guard = 0; guard < 64 && drain(out_r); guard++)
            ;
        jam_handle_close(out_r);
    }
    return st;
}

/* How it ended, as a status ($?). */
static int ended(status_t st, const struct process_info *info, const char *path, uint64_t t0)
{
    uint64_t ms = (now() - t0) / NS_PER_MS;
    if (st != OK) {
        sh_tty("run: lost track of %s (%s)\n", path, status_str(st));
        return 126;
    }
    if (info->killed) {
        sh_tty("run: %s was killed after %lu ms\n", path, (unsigned long)ms);
        return 137;
    }
    sh_tty("run: %s exited with code %ld after %lu ms\n", path, (long)info->exit_code,
           (unsigned long)ms);
    return info->exit_code < 0 ? 1 : info->exit_code > 255 ? 255 : (int)info->exit_code;
}

/* Kill what it left running, and say if its job still holds anything. */
static void clean_job(handle_t job, const char *path)
{
    struct job_info ji;
    if (jam_job_get_info(job, &ji) != OK || ji.used[JOB_LIMIT_THREADS]) {
        sh_tty("run: killing what %s left running\n", path);
        jam_job_kill(job);
    }
    if (jam_job_get_info(job, &ji) == OK) {
        bool clean = true;
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            clean &= ji.used[k] == 0;
        if (!clean)
            sh_tty("run: its job still holds %lu pages, %lu handles, %lu threads\n",
                   (unsigned long)ji.used[JOB_LIMIT_PAGES],
                   (unsigned long)ji.used[JOB_LIMIT_HANDLES],
                   (unsigned long)ji.used[JOB_LIMIT_THREADS]);
    }
}

/* argv[0] as a program; `how`: RUN_* (0: a plain program). Its status. */
static int run_program(int argc, char **argv, unsigned how)
{
    char path[SH_PATH_MAX];
    if (!find_program(argv[0], path, sizeof(path)))
        return 127;
    handle_t job, proc, out_r = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        sh_tty("run: no job (%s)\n", status_str(st));
        return 126;
    }
    struct spawn_handle x[RUN_HANDLES];
    unsigned nx = program_handles(x, how, &out_r);
    char **env = sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct spawn_args a = {
        .path = path, .argc = n, .argv = args, .job = job, .extra = x, .nextra = nx,
        .envp = (const char *const *)env, .ns = NS_ALL,
    };
    uint64_t t0 = now();
    st = spawn(&a, &proc);
    sh_free_env(env);
    if (st != OK) {
        sh_tty("run: can't start %s (%s)\n", path, status_str(st));
        if (st == ERR_ACCESS_DENIED && path[0] == '/')
            sh_tty("run: only programs in /boot can run: the kernel makes no other memory "
                   "executable yet\n");
        if (out_r)
            jam_handle_close(out_r);
        jam_handle_close(job);
        return 126;
    }
    sh_tty("run: %s started (Ctrl+C kills it)\n", path);
    sh_flush();
    struct process_info info;
    st = wait_program(proc, job, out_r, path, &info);
    int code = ended(st, &info, path, t0);
    clean_job(job, path);
    jam_handle_close(proc);
    jam_handle_close(job);
    return code;
}

int sh_run_program(int argc, char **argv)
{
    return run_program(argc, argv, 0);
}

int sh_run_program_music(int argc, char **argv)
{
    return run_program(argc, argv, RUN_MUSIC);
}

/* The result line of test program `name` ("<name>: N passed ...", which
 * it also puts in the RESULTS box) in log[0..got); red unless ok. */
static bool show_result_line(const char *name, const char *log, size_t got, bool ok)
{
    char pat[40];
    int pl = snprintf(pat, sizeof(pat), "%s: ", name);
    bool shown = false;
    for (size_t i = 0; log && i < got;) {
        size_t e = i;
        while (e < got && log[e] != '\n')
            e++;
        for (size_t k = i; k + (size_t)pl < e; k++) {
            if (memcmp(log + k, pat, (size_t)pl) || log[k + pl] < '0' || log[k + pl] > '9')
                continue;
            bool passed_line = false;
            for (size_t j = k + (size_t)pl; j + 7 <= e && !passed_line; j++)
                passed_line = !memcmp(log + j, " passed", 7);
            if (!passed_line)
                break;
            sh_say("\033[1m%s%.*s\033[0m\n", ok ? "" : "\033[91m", (int)(e - k), log + k);
            shown = true;
            break;
        }
        i = e + 1;
    }
    return shown;
}

static int run_test(int argc, char **argv, unsigned how)
{
    uint64_t from = sh_klog_end();
    int code = run_program(argc, argv, how);
    if (argc > 1)
        return code;   /* a child mode (utest's own), not the suite: no result line */
    char *log;
    size_t got;
    uint64_t start;
    sh_klog_read(from, &log, &got, &start);
    if (!show_result_line(argv[0], log, got, code == 0))
        sh_say("%s: no result line in the log\n", argv[0]);
    free(log);
    return code;
}

int sh_run_test_program(int argc, char **argv)
{
    return run_test(argc, argv, RUN_TEST);
}

int sh_run_test_program_initctl(int argc, char **argv)
{
    return run_test(argc, argv, RUN_TEST | RUN_INITCTL);
}
