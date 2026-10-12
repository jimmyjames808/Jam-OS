/* Starting programs: `run`, a program's name typed as a command, and the
 * test programs (utest, usbtest).
 *
 * Where a program is: a bare name is /boot/bin/<name>; anything with a '/'
 * is a bootfs name ("bin/x") or a path on any mount. A program on /boot is
 * started from the boot image itself (libos's spawn maps its code from the
 * image); one on /data runs only if the owner allowed exactly that file
 * (sh_allow.c), from its pages as /data's service supplies them, each
 * checked by the kernel against the approval (sh_dataprog.c), and two
 * instances of it share them; one on any
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
 * (sh_jobs.c, which keeps it until it ends).
 *
 * A POSIX program (the word `posix` in its list) gets more: an 8 MiB stack,
 * the environment a Unix program expects (sh_make_env_posix), and in the
 * foreground its descriptors 0 to 2 as the POSIX hand-over (sh_posix.c):
 * its terminal, the pipe it is in, or what a redirection names, and its
 * signal channel (SR_SIGNALS, the `signal` protocol): Ctrl+C is SIGINT to
 * it and Ctrl+\ SIGQUIT, and its job is killed if it has not ended 2 s
 * later. Only such a program takes `<` and `>`. The POSIX programs of a
 * pipeline run at once (sh_exec.c starts them with sh_stdio.defer and
 * waits in sh_pipe_settle); a native program, or a builtin, runs in turn.
 * The status of a program that a signal ended is 128 plus the signal. */
#include <idl/console.h>
#include <idl/signal.h>
#include <posixfds.h>
#include <wants.h>
#include "sh_core.h"

#define POSIX_STACK (8ull << 20)   /* a POSIX program's stack: address space, committed as used */

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
        .h = out, .rd_cap = n, .rd_bytes = (uint64_t)(uintptr_t)big,
        .rd_actual = (uint64_t)(uintptr_t)&n2, .rd_handles = (uint64_t)(uintptr_t)hs,
        .rd_hcap = nh, .rd_hactual = (uint64_t)(uintptr_t)&nh2,
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
static bool copy_round(handle_t out, bool past_ctrl_c, sh_put_fn put, char *buf, size_t cap)
{
    uint64_t t0 = now();
    for (unsigned i = 0; i < DRAIN_BUDGET && now() - t0 < DRAIN_BUDGET_NS; i++) {
        if (!past_ctrl_c && sh_interrupted())
            return false;
        handle_t hs[DRAIN_HANDLES];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = out, .rd_cap = (uint32_t)cap, .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = DRAIN_HANDLES, .rd_hactual = (uint64_t)(uintptr_t)&nh,
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

bool sh_copy_output(handle_t out, bool past_ctrl_c, sh_put_fn put)
{
    char buf[4096];
    return copy_round(out, past_ctrl_c, put, buf, sizeof(buf));
}

/* The same for a POSIX program's pipe, whose writes are messages of up to
 * 64 KiB (the layer's pipe): none is too big. */
static bool copy_pipe(handle_t out, sh_put_fn put)
{
    static char buf[CHANNEL_MAX_BYTES];
    return copy_round(out, true, put, buf, sizeof(buf));
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

#define RUN_HANDLES 4    /* the most program_handles gives, and a POSIX program's signal channel */

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
 * gets in xr; *out_r: its stdout's read end when stdout_pipe (we are in a
 * pipe, or it runs in the background: bg, no terminal then; a POSIX
 * program in the foreground has its pipe in the hand-over instead). The
 * number of them. */
static unsigned program_handles(const struct wants *w, bool bg, bool stdout_pipe,
                                struct spawn_handle *x, rights_t *xr, handle_t *out_r)
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
     * (libos stdio.c). */
    if (stdout_pipe && jam_channel_create(out_r, &out_w) == OK) {
        xr[nx] = RIGHT_SAME;
        x[nx++] = (struct spawn_handle){ SR_STDOUT, out_w };
    }
    return nx;
}

/* The program at path (a bootfs name, or a path on a mount) and its list
 * (*w). A program in the boot image has the list the build checked; one
 * on /data runs only as the owner allowed it (sh_allowed_program: *vmo
 * then holds its executable VMO, *size its bytes, given back with
 * sh_data_release); one anywhere else is given nothing, and the kernel
 * refuses its code. false: it can't run (said). */
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

/* A program spawn started: what start_program hands back. */
struct started {
    char     path[SH_PATH_MAX];   /* what was started (bin/x, /data/x) */
    handle_t proc, job;           /* its process and its job */
    handle_t out_r;               /* its stdout's read end, or HANDLE_INVALID */
    uint64_t t0;                  /* when it started */
    bool     posix;               /* a POSIX program (its list has `posix`) */
    handle_t sig;                 /* a POSIX program's signal channel (our end), or 0 */
};

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
    int sig = sh_exit_signal(info->exit_code);
    if (sig) {
        if (sh_run_lines || (sig != 2 && sig != 13))   /* SIGINT (Ctrl+C), SIGPIPE */
            sh_tty("run: %s killed by %s after %lu ms\n", path, sh_signal_name(sig),
                   (unsigned long)ms);
        return 128 + sig;
    }
    if (sh_run_lines)
        sh_tty("run: %s exited with code %ld after %lu ms\n", path, (long)info->exit_code,
               (unsigned long)ms);
    return info->exit_code < 0 ? 1 : info->exit_code > 255 ? 255 : (int)info->exit_code;
}

int sh_exit_signal(int64_t code)
{
    return code > EXIT_SIGNAL_BASE && code <= EXIT_SIGNAL_BASE + 64
               ? (int)(code - EXIT_SIGNAL_BASE) : 0;
}

const char *sh_signal_name(int signo)
{
    static const struct { int n; const char *name; } names[] = {
        { 1, "SIGHUP" }, { 2, "SIGINT" }, { 3, "SIGQUIT" }, { 6, "SIGABRT" },
        { 9, "SIGKILL" }, { 10, "SIGUSR1" }, { 12, "SIGUSR2" }, { 13, "SIGPIPE" },
        { 14, "SIGALRM" }, { 15, "SIGTERM" },
    };
    static char other[16];
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].n == signo)
            return names[i].name;
    snprintf(other, sizeof(other), "signal %d", signo);
    return other;
}

/* Kill what it left running, and say if its job still holds anything.
 * unread_ok: bytes it wrote that nobody has read yet are no leak (a POSIX
 * program's pipe to a program still reading). */
static void clean_job(handle_t job, const char *path, bool unread_ok)
{
    struct job_info ji;
    if (jam_job_get_info(job, &ji) != OK || ji.used[JOB_LIMIT_THREADS]) {
        sh_tty("run: killing what %s left running\n", path);
        jam_job_kill(job);
    }
    if (jam_job_get_info(job, &ji) == OK) {
        bool clean = true;
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            clean &= ji.used[k] == 0 || (unread_ok && k == JOB_LIMIT_MSG_BYTES);
        if (!clean)
            sh_tty("run: its job still holds %lu pages, %lu handles, %lu threads\n",
                   (unsigned long)ji.used[JOB_LIMIT_PAGES],
                   (unsigned long)ji.used[JOB_LIMIT_HANDLES],
                   (unsigned long)ji.used[JOB_LIMIT_THREADS]);
    }
}

/* ---- the programs of a line, and waiting for them --------------------------------------- */

/* One program the line started and has not finished with. A pipeline's
 * POSIX programs are all here at once (sh_stdio.defer); a program run on
 * its own is here for the length of its wait. */
struct running {
    struct started s;
    struct sh_buf *dest;   /* where its output goes (sh_put's target while it is copied) */
    bool     done;
    int      code;         /* once done: its status ($?) */
    bool     cancels;      /* once done: its end cancels the rest of the line */
    bool     int_sent;     /* it was sent SIGINT */
    bool     cut;          /* it was sent SIGPIPE: its pipe to a builtin is full */
    unsigned c_seen, q_seen;   /* the presses of Ctrl+C and Ctrl+\ it has been given */
    uint64_t kill_at;      /* its job is killed then if it is still there; never: not asked */
};

static struct running prog[SH_MAX_SEGS];
static unsigned nprog;

#define WAIT_SLICE_NS (20 * NS_PER_MS)   /* the longest settle sleeps before it looks again */

/* Signal signo to the POSIX program r, and start the clock that ends its
 * job if it takes longer than SH_SIGNAL_GRACE_NS. false if it can't be
 * sent one (not a POSIX program, or its channel is full). */
static bool signal_program(struct running *r, int signo)
{
    if (!r->s.posix || !r->s.sig ||
        signal_send(r->s.sig, (uint8_t)signo, SIGNAL_CODE_USER, 0) == ERR_SHOULD_WAIT)
        return false;
    if (r->kill_at == DEADLINE_NEVER)
        r->kill_at = now() + SH_SIGNAL_GRACE_NS;
    return true;
}

static void kill_now(struct running *r, const char *why)
{
    if (r->kill_at == DEADLINE_NEVER - 1)
        return;   /* its job is dead already */
    sh_tty("%s: killing %s\n", why, r->s.path);
    sh_flush();
    jam_job_kill(r->s.job);
    r->kill_at = DEADLINE_NEVER - 1;   /* once */
}

/* What the keys pressed since r last looked, and the clock, ask of it.
 * Ctrl+C is SIGINT to a POSIX program (every press: it may take the first
 * and go on) and its job killed at once for any other; Ctrl+\ is SIGQUIT,
 * for POSIX programs only. Either signal starts a clock: the job is killed
 * if the program has not ended SH_SIGNAL_GRACE_NS later. */
static void give_keys(struct running *r)
{
    if (r->c_seen != sh_ctrl_c_hits()) {
        r->c_seen = sh_ctrl_c_hits();
        if (signal_program(r, LINUX_SIGINT))
            r->int_sent = true;
        else
            kill_now(r, "^C");
    }
    if (r->q_seen != sh_quit_hits()) {
        r->q_seen = sh_quit_hits();
        (void)signal_program(r, LINUX_SIGQUIT);
    }
    if (now() >= r->kill_at) {
        sh_tty("sh: %s is still there %u s after the signal: killing it\n", r->s.path,
               (unsigned)(SH_SIGNAL_GRACE_NS / NS_PER_S));
        sh_flush();
        jam_job_kill(r->s.job);
        r->kill_at = DEADLINE_NEVER - 1;
    }
}

/* Copy what r wrote to where its output goes. true if more may be queued.
 * A builtin that takes its whole input can't be fed more than SH_PIPE_MAX:
 * when the buffer is full the program is sent SIGPIPE, as a reader that
 * went away would (`yes | head -3`). */
static bool drain_prog(struct running *r)
{
    if (!r->s.out_r)
        return false;
    struct sh_stdio saved = sh_stdio_get(), io = saved;
    io.out = r->dest;
    sh_stdio_set(io);
    bool more = r->s.posix ? copy_pipe(r->s.out_r, sh_put)
                           : sh_copy_output(r->s.out_r, true, sh_put);
    sh_stdio_set(saved);
    if (r->dest && r->dest->full && !r->cut) {
        r->cut = true;
        (void)signal_program(r, LINUX_SIGPIPE);
    }
    return more;
}

/* r ended (*info; st not OK: its process can't be asked): the rest of its
 * output, how it ended, and its handles. */
static void reap(struct running *r, status_t st, const struct process_info *info)
{
    if (r->s.out_r) {
        /* What is left: the queue is capped (1024 messages), so this ends
         * unless something the program left running keeps writing, which
         * the guard cuts short. */
        for (unsigned guard = 0; guard < 64 && drain_prog(r); guard++)
            ;
        jam_handle_close(r->s.out_r);
        r->s.out_r = HANDLE_INVALID;
    }
    r->code = ended(st, info, r->s.path, r->s.t0);
    r->cancels = !r->s.posix || st != OK || info->killed ||
                 sh_exit_signal(info->exit_code) == LINUX_SIGINT;
    clean_job(r->s.job, r->s.path, r->s.posix);
    sh_data_prune();   /* its job is empty: the shell's hold on its code may go */
    sh_show_log(false, NULL);
    if (r->s.posix)
        sh_io_posix_tty(-1);
    jam_handle_close(r->s.proc);
    if (r->s.sig)
        jam_handle_close(r->s.sig);
    jam_handle_close(r->s.job);
    r->done = true;
}

unsigned sh_pipe_running(void)
{
    return nprog;
}

int sh_pipe_settle(void)
{
    bool int_sent = false, cancels = false;
    for (;;) {
        sh_poll_keys();
        bool busy = false;
        struct running *wait_on = NULL;
        for (unsigned i = 0; i < nprog; i++) {
            struct running *r = &prog[i];
            if (r->done)
                continue;
            give_keys(r);
            busy |= drain_prog(r);
            struct process_info info = { 0 };
            status_t st = spawn_wait(r->s.proc, 0, &info);
            if (st != ERR_TIMED_OUT) {
                reap(r, st, &info);
                continue;
            }
            if (!wait_on || (!wait_on->s.out_r && r->s.out_r))
                wait_on = r;
        }
        if (!wait_on)
            break;
        sh_jobs_poll(false);
        if (busy)
            continue;
        /* Nothing to copy right now: sleep until its output comes or it
         * ends (one that is read from wakes us as its data arrives). */
        signals_t seen = 0;
        if (wait_on->s.out_r &&
            jam_object_wait_one(wait_on->s.out_r, SIG_READABLE | SIG_PEER_CLOSED,
                                now() + WAIT_SLICE_NS, &seen) == OK &&
            (seen & SIG_READABLE))
            continue;
        (void)spawn_wait(wait_on->s.proc, WAIT_SLICE_NS, NULL);
    }
    int status = nprog ? prog[nprog - 1].code : -1;
    for (unsigned i = 0; i < nprog; i++) {
        int_sent |= prog[i].int_sent;
        cancels |= prog[i].cancels;
    }
    nprog = 0;
    /* A POSIX program that took SIGINT and went on (or ended its own way)
     * has dealt with the key: the rest of the line runs, as in any shell. */
    if (int_sent && !cancels)
        sh_cancel_clear();
    return status;
}

/* s was started: it is waited for in sh_pipe_settle. Its stdout's read end
 * goes on to the next stage's program when the stage says so. */
static void add_running(const struct started *s)
{
    if (nprog == SH_MAX_SEGS)
        (void)sh_pipe_settle();   /* more than a line can hold: only nested lines get here */
    struct sh_stdio io = sh_stdio_get();
    struct running *r = &prog[nprog++];
    *r = (struct running){
        .s = *s, .dest = io.out, .kill_at = DEADLINE_NEVER, .q_seen = sh_quit_hits(),
        /* a Ctrl+C pressed while it started is its too */
        .c_seen = sh_ctrl_c_hits() - (sh_cancelled() ? 1 : 0),
    };
    if (s->posix) {
        sh_io_posix_tty(+1);
        if (io.direct_out && s->out_r) {
            io.direct_next = s->out_r;
            sh_stdio_set(io);
            r->s.out_r = HANDLE_INVALID;
        }
    }
}

bool sh_program_is_posix(const char *name)
{
    char path[SH_PATH_MAX];
    static struct wants w;
    handle_t vmo;
    uint64_t size;
    sh_quiet(true);
    bool ok = find_program(name, path, sizeof(path)) && program_wants(path, &w, &vmo, &size);
    sh_quiet(false);
    if (ok)
        sh_data_release(vmo, HANDLE_INVALID);   /* not started: only its list was wanted */
    return ok && w.posix;
}

/* This command's redirections, if the program can take them: only a POSIX
 * program in the foreground can (said). */
static bool redirections_ok(const struct started *s, bool bg)
{
    if (!sh_redir_count())
        return true;
    if (!s->posix)
        sh_tty("sh: %s isn't a POSIX program: < and > work for those only\n", s->path);
    else if (bg)
        sh_tty("sh: a program in the background can't take < and >\n");
    return s->posix && !bg;
}

/* The console channel among the extras, or HANDLE_INVALID. */
static handle_t console_of(const struct spawn_handle *x, unsigned nx)
{
    for (unsigned i = 0; i < nx; i++)
        if (x[i].role == SR_CONSOLE)
            return x[i].h;
    return HANDLE_INVALID;
}

/* What spawn is given only for a POSIX program: its stack and, in the
 * foreground, the hand-over (into ho). false (said): a redirection's file
 * won't open; the extras x are closed then, spawn will not run. */
static bool posix_setup(struct started *s, bool bg, struct spawn_handle *x, unsigned nx,
                        struct posixfds_out *ho, struct spawn_args *a)
{
    a->stack_size = POSIX_STACK;
    if (bg)
        return true;
    if (sh_posix_prepare(ho, console_of(x, nx), &s->out_r)) {
        a->handover = ho;
        return true;
    }
    for (unsigned i = 0; i < nx; i++)
        jam_handle_close(x[i].h);
    return false;
}

/* The program is not started: say why (unless it was said), and let go of
 * what start_program made. The status to give back. */
static int start_failed(struct started *s, status_t st, bool said)
{
    if (!said)
        sh_tty("run: can't start %s (%s)\n", s->path, status_str(st));
    if (!said && st == ERR_ACCESS_DENIED && s->path[0] == '/')
        sh_tty("run: only programs in /boot can run, and those on /data the owner "
               "allowed (`allow`)\n");
    if (!said && st == ERR_IO && sh_on_data(s->path))
        sh_tty("run: a page of %s doesn't match its approval, or /data can't read it: "
               "`allow %s` again if it changed\n", s->path, s->path);
    if (s->out_r)
        jam_handle_close(s->out_r);
    jam_handle_close(s->sig);
    jam_handle_close(s->job);
    return said ? 1 : 126;
}

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
    s->posix = w.posix;
    s->out_r = HANDLE_INVALID;
    s->sig = HANDLE_INVALID;
    status_t st = OK;
    if (!redirections_ok(s, bg))
        st = ERR_INVALID_ARGS;
    else if ((st = jam_job_create(startup_handle(SR_JOB), 0, &s->job)) != OK)
        sh_tty("run: no job (%s)\n", status_str(st));
    if (st != OK) {
        sh_data_release(vmo, HANDLE_INVALID);
        return st == ERR_INVALID_ARGS ? 2 : 126;
    }
    const char *grants[WANTS_MAX + 1];
    for (unsigned i = 0; i < w.n; i++)
        grants[i] = w.grant[i];
    grants[w.n] = NULL;
    struct spawn_handle x[RUN_HANDLES];
    rights_t xr[RUN_HANDLES];
    /* A POSIX program in the foreground has its pipe in the hand-over. */
    unsigned nx = program_handles(&w, bg, bg || (sh_piped() && !s->posix), x, xr, &s->out_r);
    handle_t theirs;
    if (s->posix && jam_channel_create(&s->sig, &theirs) == OK) {
        xr[nx] = RIGHT_SAME;
        x[nx++] = (struct spawn_handle){ SR_SIGNALS, theirs };   /* kill's way to it */
    }
    char **env = s->posix ? sh_make_env_posix() : sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = s->path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct posixfds_out ho;
    struct spawn_args a = {
        .path = s->path, .vmo = vmo, .size = size, .argc = n, .argv = args, .job = s->job,
        .extra = x, .nextra = nx, .extra_rights = xr, .envp = (const char *const *)env,
        .ns = w.n ? grants : NULL,
    };
    bool said = s->posix && !posix_setup(s, bg, x, nx, &ho, &a);
    s->t0 = now();
    if (!said)
        st = spawn(&a, &s->proc);
    sh_free_env(env);
    /* The program maps what it runs: it keeps it. A /data program's VMO
     * stays with the shell too while its job runs, for the next instance. */
    sh_data_release(vmo, said || st != OK ? HANDLE_INVALID : s->job);
    return said || st != OK ? start_failed(s, st, said) : 0;
}

/* argv[0] as a program. Its status. */
static int run_program(int argc, char **argv)
{
    struct started s;
    int code = start_program(argc, argv, false, &s);
    if (code)
        return code;
    bool defer = s.posix && sh_stdio_get().defer;
    /* What it prints goes to the log (unless piped): shown while it runs.
     * The name filter is for a program alone; a pipeline's have several. */
    const char *base = strrchr(s.path, '/');
    sh_show_log(true, defer ? NULL : base ? base + 1 : s.path);
    if (sh_run_lines)
        sh_tty("run: %s started (Ctrl+C kills it)\n", s.path);
    sh_flush();
    add_running(&s);
    if (defer)
        return 0;   /* the pipeline waits for it: sh_pipe_settle */
    return sh_pipe_settle();
}

int sh_run_program(int argc, char **argv)
{
    return run_program(argc, argv);
}

int sh_start_background(int argc, char **argv)
{
    if (sh_jobs_full())
        sh_jobs_report();   /* ended ones (repeat 9 'x &') free their numbers now */
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
    /* A POSIX program's signal channel is kept: kill %n sends SIGTERM. */
    unsigned n = sh_jobs_add(s.path, argc, argv, s.proc, s.job, s.out_r, s.sig);
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
        x[(*nx)++] = (struct spawn_handle){ PROGRAM_SR_STOP, stop_r };
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
