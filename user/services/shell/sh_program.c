/* Starting programs from /boot/bin: `run`, a program's name typed as a
 * command, demo, and the test programs (utest, usbtest).
 *
 * What a program gets: a PROGRAM-level console channel of its own
 * (console.new_client: write, keys while it runs, the screen; no input
 * sources, no new channels), and nothing of devmgr's unless `test` (the
 * utest and usbtest commands: test suites that kill and rebind drivers get
 * the query and control channels). Ctrl+C reaches the shell even while the
 * program holds the keys (the console sees to that) and kills it. When it
 * ends its job is killed: anything it started goes with it, so its console
 * channel never outlives it in the foreground. */
#include <idl/console.h>
#include "sh.h"

/* Copy what the program wrote to its SR_STDOUT channel into our output. */
static void drain(handle_t out)
{
    char buf[4096];
    for (;;) {
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = out, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        if (jam_channel_read(&a) != OK)
            return;
        sh_put(buf, n);
    }
}

/* The bootfs path of argv0: "utest" -> bin/utest; "bin/x" as it is; else
 * a path through the cwd ("/boot/bin/x", "../drv/x"). */
static void bootfs_path(const struct bootfs_view *fs, const char *argv0, char *path, size_t cap)
{
    const void *data;
    uint64_t size;
    if (!strchr(argv0, '/')) {
        snprintf(path, cap, "bin/%s", argv0);
    } else if (bootfs_lookup(fs, argv0, &data, &size) == OK) {
        snprintf(path, cap, "%s", argv0);
    } else {
        char abs[SH_PATH_MAX];
        const char *name = sh_resolve(argv0, abs, sizeof(abs)) ? sh_bootfs_name(abs) : NULL;
        snprintf(path, cap, "%s", name ? name : argv0);
    }
}

/* The handles it starts with (see the top); *out_r: its stdout's read end
 * when we are in a pipe. The number of them. */
static unsigned program_handles(struct spawn_handle *x, bool test, handle_t *out_r)
{
    unsigned nx = 0;
    handle_t h, out_w;
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
        drain(out_r);
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

/* argv[0] as a program; `test`: with devmgr's channels. Its status. */
static int run_program(int argc, char **argv, bool test)
{
    char path[SH_PATH_MAX];
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK) {
        sh_tty("run: no bootfs\n");
        return 127;
    }
    bootfs_path(fs, argv[0], path, sizeof(path));
    if (bootfs_lookup(fs, path, &data, &size) != OK) {
        sh_tty("run: no %s in bootfs\n", path);
        return 127;
    }
    handle_t job, proc, out_r = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        sh_tty("run: no job (%s)\n", status_str(st));
        return 126;
    }
    struct spawn_handle x[4];
    unsigned nx = program_handles(x, test, &out_r);
    char **env = sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct spawn_args a = {
        .path = path, .argc = n, .argv = args, .job = job, .extra = x, .nextra = nx,
        .envp = (const char *const *)env,
    };
    uint64_t t0 = now();
    st = spawn(&a, &proc);
    sh_free_env(env);
    if (st != OK) {
        sh_tty("run: can't start %s (%s)\n", path, status_str(st));
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
    return run_program(argc, argv, false);
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

int sh_run_test_program(int argc, char **argv)
{
    uint64_t from = sh_klog_end();
    int code = run_program(argc, argv, true);
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
