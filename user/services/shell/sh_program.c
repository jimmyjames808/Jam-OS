/* Starting programs: `run`, a program's name typed as a command, demo,
 * and the test programs (utest, usbtest).
 *
 * Where a program is: a bare name is /boot/bin/<name>; anything with a '/'
 * is a bootfs name ("bin/x") or a path on any mount. A program on /boot is
 * started from the boot image itself (libos's spawn maps its code from the
 * image); one on /data runs only if the owner allowed exactly that file
 * (sh_allow.c), from a VMO the kernel made executable for us; one on any
 * other mount is read through the namespace and the kernel refuses its
 * code (<os.h>, spawn_args.path).
 *
 * What a program gets is its list (<wants.h>), which the build checked
 * for every program in the boot image: the services and mounts it names,
 * each mount a view (read-only, or writable with the top-level etc left
 * alone: <fsview.h>), and the root resource with only the powers it names
 * (without RIGHT_TRANSFER: it can't pass them on).
 * Every program also gets its terminal: a PROGRAM-level console channel of
 * its own (console.new_client: write, keys while it runs, the screen; no
 * input sources, no new channels), and its output channel in a pipe. A
 * program with no list gets the terminal only. Ctrl+C reaches the shell
 * even while the program holds the keys (the console sees to that) and
 * kills it. When it ends its job is killed: anything it started goes with
 * it, so its console channel never outlives it in the foreground. */
#include <idl/console.h>
#include <wants.h>
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

#define RUN_HANDLES 3    /* the most program_handles gives */

/* The root resource's rights for a list's WANT_RIGHT_*: what the
 * program's copy gets. Never RIGHT_TRANSFER: it can narrow its copy
 * (RIGHT_DUPLICATE), never pass it on. */
static rights_t root_rights(uint32_t want)
{
    rights_t r = RIGHT_DUPLICATE | RIGHT_WAIT | RIGHT_INSPECT;
    r |= want & WANT_RIGHT_KLOG ? RIGHT_ROOT_KLOG : 0;
    r |= want & WANT_RIGHT_SYSINFO ? RIGHT_ROOT_SYSINFO : 0;
    r |= want & WANT_RIGHT_CLOCK ? RIGHT_ROOT_CLOCK : 0;
    r |= want & WANT_RIGHT_DEBUG ? RIGHT_ROOT_DEBUG : 0;
    return r;
}

/* The handles it starts with (see the top), each with the rights its copy
 * gets in xr; *out_r: its stdout's read end when we are in a pipe. The
 * number of them. */
static unsigned program_handles(const struct wants *w, struct spawn_handle *x, rights_t *xr,
                                handle_t *out_r)
{
    unsigned nx = 0;
    handle_t h, out_w;
    if (w->rights && sh_root() && jam_handle_duplicate(sh_root(), RIGHT_SAME, &h) == OK) {
        xr[nx] = root_rights(w->rights);
        x[nx++] = (struct spawn_handle){ SR_RESOURCE, h };
    }
    if (console_new_client_until(sh_console(), now() + 5 * NS_PER_S, 2, &h) == OK) {
        xr[nx] = RIGHT_SAME;
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, h };
    }
    /* In a pipe: its printf goes down a channel to us (libos printf.c). */
    if (sh_piped() && jam_channel_create(out_r, &out_w) == OK) {
        xr[nx] = RIGHT_SAME;
        x[nx++] = (struct spawn_handle){ SR_STDOUT, out_w };
    }
    return nx;
}

/* The program at path (a bootfs name, or a path on a mount) and its list
 * (*w). A program in the boot image has the list the build checked; one
 * on /data runs only as the owner allowed it (sh_allow.c: *vmo then holds
 * its bytes, made executable, *size of them); one anywhere else is given
 * nothing, and the kernel refuses its code. false: it can't run (said). */
static bool program_wants(const char *path, struct wants *w, handle_t *vmo, uint64_t *size)
{
    const struct bootfs_view *fs;
    const void *data;
    memset(w, 0, sizeof(*w));
    *vmo = HANDLE_INVALID;
    *size = 0;
    if (path[0] == '/')
        return !sh_on_data(path) || sh_allowed_program(path, vmo, size, w);
    if (bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, size) == OK)
        (void)wants_read(data, *size, w);   /* checked at build time; a bad one gives nothing */
    *size = 0;
    return true;
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

/* argv[0] as a program. Its status. */
static int run_program(int argc, char **argv)
{
    char path[SH_PATH_MAX];
    if (!find_program(argv[0], path, sizeof(path)))
        return 127;
    handle_t job, proc, out_r = HANDLE_INVALID, vmo;
    uint64_t size;
    static struct wants w;   /* the shell runs one program at a time */
    if (!program_wants(path, &w, &vmo, &size))
        return 126;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        sh_tty("run: no job (%s)\n", status_str(st));
        if (vmo)
            jam_handle_close(vmo);
        return 126;
    }
    const char *grants[WANTS_MAX + 1];
    for (unsigned i = 0; i < w.n; i++)
        grants[i] = w.grant[i];
    grants[w.n] = NULL;
    struct spawn_handle x[RUN_HANDLES];
    rights_t xr[RUN_HANDLES];
    unsigned nx = program_handles(&w, x, xr, &out_r);
    char **env = sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct spawn_args a = {
        .path = path, .vmo = vmo, .size = size, .argc = n, .argv = args, .job = job,
        .extra = x, .nextra = nx, .extra_rights = xr, .envp = (const char *const *)env,
        .ns = w.n ? grants : NULL,
    };
    uint64_t t0 = now();
    st = spawn(&a, &proc);
    sh_free_env(env);
    if (vmo)
        jam_handle_close(vmo);   /* the program maps what it runs: it keeps it */
    if (st != OK) {
        sh_tty("run: can't start %s (%s)\n", path, status_str(st));
        if (st == ERR_ACCESS_DENIED && path[0] == '/')
            sh_tty("run: only programs in /boot can run, and those on /data the owner "
                   "allowed (`allow`)\n");
        if (out_r)
            jam_handle_close(out_r);
        jam_handle_close(job);
        return 126;
    }
    /* What it prints goes to the log (unless piped): shown while it runs. */
    const char *base = strrchr(path, '/');
    sh_show_log(true, base ? base + 1 : path);
    sh_tty("run: %s started (Ctrl+C kills it)\n", path);
    sh_flush();
    struct process_info info;
    st = wait_program(proc, job, out_r, path, &info);
    int code = ended(st, &info, path, t0);
    clean_job(job, path);
    sh_show_log(false, NULL);
    jam_handle_close(proc);
    jam_handle_close(job);
    return code;
}

int sh_run_program(int argc, char **argv)
{
    return run_program(argc, argv);
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
    sh_show_log(true, NULL);   /* a test's whole story: drivers, devmgr, the kernel */
    int code = run_program(argc, argv);
    sh_show_log(false, NULL);
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
