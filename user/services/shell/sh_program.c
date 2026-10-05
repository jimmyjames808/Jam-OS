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
 * it, so its console channel never outlives it in the foreground.
 *
 * A program started in the background (`prog &`) gets no terminal: its
 * printf goes down an output channel the shell copies to the screen
 * (sh_jobs.c, which keeps it until it ends). */
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

static void tty_put(const char *s, size_t n)
{
    sh_tty("%.*s", (int)n, s);
}

/* A message printf wouldn't send (over 4096 bytes, or carrying handles)
 * is dropped, its handles closed: left at the head of the queue it would
 * fail every later read. */
bool sh_copy_output(handle_t out, bool past_ctrl_c, sh_put_fn put)
{
    char buf[4096];
    uint64_t t0 = now();
    for (unsigned i = 0; i < DRAIN_BUDGET && now() - t0 < DRAIN_BUDGET_NS; i++) {
        if (!past_ctrl_c && sh_interrupted())
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
            put(buf, n);
    }
    return true;
}

static bool drain(handle_t out, bool past_ctrl_c)
{
    return sh_copy_output(out, past_ctrl_c, sh_put);
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
 * gets in xr; *out_r: its stdout's read end when we are in a pipe or it
 * runs in the background (bg: no terminal then). The number of them. */
static unsigned program_handles(const struct wants *w, bool bg, struct spawn_handle *x,
                                rights_t *xr, handle_t *out_r)
{
    unsigned nx = 0;
    handle_t h, out_w;
    if (w->rights && sh_root() && jam_handle_duplicate(sh_root(), RIGHT_SAME, &h) == OK) {
        xr[nx] = root_rights(w->rights);
        x[nx++] = (struct spawn_handle){ SR_RESOURCE, h };
    }
    if (!bg && console_new_client_until(sh_console(), now() + 5 * NS_PER_S, 2, &h) == OK) {
        xr[nx] = RIGHT_SAME;
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, h };
    }
    /* In a pipe or the background: its printf goes down a channel to us
     * (libos printf.c). */
    if ((bg || sh_piped()) && jam_channel_create(out_r, &out_w) == OK) {
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
            drain(out_r, false);
        sh_jobs_poll(false);
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
        for (unsigned guard = 0; guard < 64 && drain(out_r, false); guard++)
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

/* A program spawn started: what start_program hands back. */
struct started {
    char     path[SH_PATH_MAX];   /* what was started (bin/x, /data/x) */
    handle_t proc, job;           /* its process and its job */
    handle_t out_r;               /* its stdout's read end, or HANDLE_INVALID */
    uint64_t t0;                  /* when it started */
};

/* argv[0] found, its list read, a job made, its handles and spawned (bg:
 * to run in the background), into *s. 0, or the status to give back
 * (said). */
static int start_program(int argc, char **argv, bool bg, struct started *s)
{
    if (!find_program(argv[0], s->path, sizeof(s->path)))
        return 127;
    handle_t vmo;
    uint64_t size;
    static struct wants w;   /* the shell starts one program at a time */
    if (!program_wants(s->path, &w, &vmo, &size))
        return 126;
    s->out_r = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &s->job);
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
    unsigned nx = program_handles(&w, bg, x, xr, &s->out_r);
    char **env = sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = s->path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct spawn_args a = {
        .path = s->path, .vmo = vmo, .size = size, .argc = n, .argv = args, .job = s->job,
        .extra = x, .nextra = nx, .extra_rights = xr, .envp = (const char *const *)env,
        .ns = w.n ? grants : NULL,
    };
    s->t0 = now();
    st = spawn(&a, &s->proc);
    sh_free_env(env);
    if (vmo)
        jam_handle_close(vmo);   /* the program maps what it runs: it keeps it */
    if (st == OK)
        return 0;
    sh_tty("run: can't start %s (%s)\n", s->path, status_str(st));
    if (st == ERR_ACCESS_DENIED && s->path[0] == '/')
        sh_tty("run: only programs in /boot can run, and those on /data the owner "
               "allowed (`allow`)\n");
    if (s->out_r)
        jam_handle_close(s->out_r);
    jam_handle_close(s->job);
    return 126;
}

/* argv[0] as a program. Its status. */
static int run_program(int argc, char **argv)
{
    struct started s;
    int code = start_program(argc, argv, false, &s);
    if (code)
        return code;
    /* What it prints goes to the log (unless piped): shown while it runs. */
    const char *base = strrchr(s.path, '/');
    sh_show_log(true, base ? base + 1 : s.path);
    sh_tty("run: %s started (Ctrl+C kills it)\n", s.path);
    sh_flush();
    struct process_info info;
    status_t st = wait_program(s.proc, s.job, s.out_r, s.path, &info);
    code = ended(st, &info, s.path, s.t0);
    clean_job(s.job, s.path);
    sh_show_log(false, NULL);
    jam_handle_close(s.proc);
    jam_handle_close(s.job);
    return code;
}

int sh_run_program(int argc, char **argv)
{
    return run_program(argc, argv);
}

int sh_start_background(int argc, char **argv)
{
    if (sh_jobs_full()) {
        sh_tty("sh: %d programs already run in the background (the most): end one first "
               "(jobs, kill %%n)\n", SH_MAX_JOBS);
        return 1;
    }
    if (sh_piped()) {
        sh_tty("sh: a program in the background can't write into a pipe\n");
        return 2;
    }
    struct started s;
    int code = start_program(argc, argv, true, &s);
    if (code)
        return code;
    unsigned n = sh_jobs_add(s.path, argc, argv, s.proc, s.job, s.out_r);
    const struct sh_job *j = sh_job_at(n - 1);
    sh_tty("[%u] %lu %s: in the background (jobs lists it, kill %%%u ends it)\n", n,
           (unsigned long)(j ? j->pid : 0), sh_basename(s.path), n);
    return 0;
}

/* ---- helpers: a program that does one job for a command ----------------------------- */

#define STOP_GRACE (3 * NS_PER_S)   /* a stopped helper's time to wind down */

/* Wait for the helper to end, copying its output (its lines, and what
 * comes on body_r when it has one: then its lines go to the screen); on
 * Ctrl+C ask it to stop (a byte on stop), and kill its job if it hasn't
 * ended in time. */
static status_t wait_helper(handle_t proc, handle_t job, handle_t out_r, handle_t stop,
                            handle_t body_r, struct process_info *info)
{
    status_t st;
    uint64_t kill_at = DEADLINE_NEVER;
    sh_put_fn lines = body_r ? tty_put : sh_put;
    while ((st = spawn_wait(proc, 50 * NS_PER_MS, info)) == ERR_TIMED_OUT) {
        sh_copy_output(out_r, true, lines);
        sh_jobs_poll(false);
        if (body_r)
            sh_copy_output(body_r, true, sh_put);
        if (sh_interrupted() && kill_at == DEADLINE_NEVER) {
            uint8_t go = 1;
            if (jam_channel_write(stop, &go, 1, NULL, 0) != OK)
                kill_at = 0;
            else
                kill_at = now() + STOP_GRACE;
        }
        if (now() >= kill_at) {
            jam_job_kill(job);
            kill_at = DEADLINE_NEVER - 1;   /* once */
        }
    }
    for (unsigned guard = 0; guard < 64 && sh_copy_output(out_r, true, lines); guard++)
        ;
    for (unsigned guard = 0; body_r && guard < 4096; guard++)
        if (!sh_copy_output(body_r, true, sh_put))
            break;
    return st;
}

/* Our ends of a helper's output and stop channels, and its job; with the
 * helper's ends added to x (2 more entries). */
struct helper {
    handle_t job, out_r, stop_w;
};

static status_t helper_setup(struct helper *h, struct spawn_handle *x, unsigned *nx)
{
    handle_t out_w, stop_r;
    *h = (struct helper){ 0 };
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &h->job);
    if (st == OK && (st = jam_channel_create(&h->out_r, &out_w)) == OK)
        x[(*nx)++] = (struct spawn_handle){ SR_STDOUT, out_w };
    if (st == OK && (st = jam_channel_create(&h->stop_w, &stop_r)) == OK)
        x[(*nx)++] = (struct spawn_handle){ SR_USER + 2, stop_r };
    return st;
}

static void helper_done(struct helper *h)
{
    handle_t *hs[] = { &h->out_r, &h->stop_w, &h->job };
    struct job_info ji;
    /* Anything it left running goes with it (an empty job is left alone:
     * a kill is a log line). */
    if (h->job && (jam_job_get_info(h->job, &ji) != OK || ji.used[JOB_LIMIT_THREADS]))
        jam_job_kill(h->job);
    for (unsigned i = 0; i < 3; i++)
        if (*hs[i])
            jam_handle_close(*hs[i]);
}

static int run_helper(const char *path, int argc, const char *const *argv,
                      struct spawn_handle *x, unsigned nx, handle_t body_r)
{
    static struct wants w;
    handle_t vmo, proc;
    uint64_t size;
    struct helper h;
    (void)program_wants(path, &w, &vmo, &size);   /* a bootfs name: never fails */
    const char *grants[WANTS_MAX + 1];
    for (unsigned i = 0; i < w.n; i++)
        grants[i] = w.grant[i];
    grants[w.n] = NULL;
    status_t st = helper_setup(&h, x, &nx);
    struct spawn_args a = {
        .path = path, .argc = argc, .argv = argv, .job = h.job, .extra = x, .nextra = nx,
        .ns = w.n ? grants : NULL,
    };
    if (st == OK) {
        st = spawn(&a, &proc);   /* the extras go, whatever happens */
    } else {
        for (unsigned i = 0; i < nx; i++)
            jam_handle_close(x[i].h);
    }
    if (st != OK) {
        helper_done(&h);
        sh_tty("%s: can't start %s (%s)\n", argv[0], path, status_str(st));
        return 126;
    }
    struct process_info info;
    st = wait_helper(proc, h.job, h.out_r, h.stop_w, body_r, &info);
    helper_done(&h);
    jam_handle_close(proc);
    if (st != OK || info.killed)
        return 137;
    return info.exit_code < 0 ? 1 : info.exit_code > 255 ? 255 : (int)info.exit_code;
}

int sh_run_helper(const char *path, int argc, const char *const *argv, struct spawn_handle *x,
                  unsigned nx)
{
    return run_helper(path, argc, argv, x, nx, HANDLE_INVALID);
}

int sh_run_helper_out(const char *path, int argc, const char *const *argv,
                      struct spawn_handle *x, unsigned nx, handle_t body_r)
{
    return run_helper(path, argc, argv, x, nx, body_r);
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
