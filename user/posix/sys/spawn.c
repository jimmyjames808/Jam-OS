/* posix_spawn and execve (docs/M13-PLAN.md section 5): a child started
 * with libjam's spawn, so the loader, the job and the namespace are the
 * native ones, and given what a Unix child keeps across exec through the
 * POSIX hand-over (<posixfds.h>).
 *
 * The steps of a spawn (__jam_posix_spawn, from musl's posix_spawn and
 * posix_spawnp through our override):
 *   1. A copy of our descriptor table, slots only (spawnfd.c): which of
 *      our descriptors becomes which of the child's, and which have
 *      FD_CLOEXEC; with our current directory.
 *   2. The file actions applied to the copy, in order (spawnfd.c): close,
 *      dup2, open (opened here, into a descriptor of ours kept until the
 *      spawn ends), chdir, fchdir.
 *   3. The hand-over: every slot without FD_CLOEXEC exported (new handles
 *      of the child's own), the directory, the umask, the signal mask and
 *      the ignored signals, our pid as its parent's, its process group and
 *      session (0 meaning its own pid, which nobody knows before it is
 *      made).
 *   4. The program: a name looked up in PATH (posix_spawnp), then a path
 *      (relative ones from the copy's directory): one under /boot runs
 *      from the boot image, as do /bin and /usr/bin, which are the boot
 *      image's bin and usr/bin (there is no other); anything else is
 *      EACCES until programs can be loaded from files, ENOENT if missing.
 *   5. spawn: our job, our namespace (or the grants list
 *      posix_spawnattr_setgrants_np gave), an 8 MiB stack, the hand-over
 *      written before the child runs. Then its pid, and a place in the
 *      table of children (proc.c).
 * A parent killed at any step leaves either no child (a process that
 * never started dies with its last handle, ours) or a child with the
 * whole hand-over: spawn writes all of it before process_start.
 *
 * execve is spawn, wait, exit (question 4's answer): the program starts
 * as our child with our descriptors, our mask and ignored signals, our
 * parent, group and session; we close our descriptors (they are the new
 * program's now), wait for it (the signal track forwards every signal to
 * it: lx_exec_child) and exit with its status. Its pid is not ours. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include "proc.h"
#include "spawn.h"

#define CHILD_STACK  (8ull << 20)   /* a POSIX child's stack: Linux's usual 8 MiB */
#define SIG_NEVER    ((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)))
#define UMASK_DEFAULT 022

/* ---- the program --------------------------------------------------------------------- */

/* Where the boot image's programs are, as paths: /boot itself, and /bin
 * and /usr/bin, its bin and usr/bin (there is no other /bin), so musl's
 * /bin/sh and its default PATH find them. */
static const struct { const char *path; const char *name; } boot_dirs[] = {
    { "/boot/", "" }, { "/bin/", "bin/" }, { "/usr/bin/", "usr/bin/" },
};

/* The program at abs (absolute, cleaned) as spawn_args.path: a bootfs
 * name into name (PATH_MAX). 0, or -ENOENT (missing), -EACCES (a file
 * spawn can't run yet: only the boot image's programs can be). */
static long program(const char *abs, char *name)
{
    for (unsigned i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); i++) {
        size_t l = strlen(boot_dirs[i].path);
        if (strncmp(abs, boot_dirs[i].path, l))
            continue;
        const struct bootfs_view *fs;
        const void *data;
        uint64_t size;
        strcpy(name, boot_dirs[i].name);
        strcat(name, abs + l);   /* fits: the prefix is never longer than the path's */
        if (bootfs_default(&fs) == OK && bootfs_lookup(fs, name, &data, &size) == OK)
            return 0;
        return -ENOENT;
    }
    bool dir;
    status_t st = fs_stat(abs, NULL, &dir, NULL);
    if (st == ERR_NOT_FOUND || st == ERR_NOT_DIR)
        return st == ERR_NOT_FOUND ? -ENOENT : -ENOTDIR;
    return -EACCES;   /* a program on a file system: not until M12.5's loader */
}

/* The program for r into name: its path, or its name looked up in
 * r->search (a PATH: ':'-separated; an empty part is the directory dir). */
static long find_program(const struct jam_spawn_req *r, const char *dir, char *name)
{
    char *abs = malloc(PATH_MAX), *part = malloc(PATH_MAX);
    long e = abs && part ? 0 : -ENOMEM;
    if (!e && (!r->search || strchr(r->path, '/'))) {
        e = spawn_absolute(r->path, dir, abs);
        if (!e)
            e = program(abs, name);
    } else if (!e) {
        bool denied = false;
        size_t ln = strnlen(r->path, PATH_MAX);
        e = -ENOENT;
        for (const char *p = r->search, *end = p; *end; p = end + 1) {
            end = strchrnul(p, ':');
            size_t l = (size_t)(end - p);
            e = -ENAMETOOLONG;
            if (l + 1 + ln < PATH_MAX) {
                snprintf(part, PATH_MAX, "%.*s%s%s", (int)l, p, l ? "/" : "", r->path);
                e = spawn_absolute(part, dir, abs);
            }
            if (!e)
                e = program(abs, name);
            denied |= e == -EACCES;
            if (!e || (e != -ENOENT && e != -ENOTDIR && e != -EACCES && e != -ENAMETOOLONG))
                break;
        }
        if (e && denied)
            e = -EACCES;
    }
    free(abs);
    free(part);
    return e;
}

/* ---- the hand-over's state ------------------------------------------------------------- */

/* The signals this thread blocks and the ones we ignore (bit n - 1). */
static void our_signals(uint64_t *mask, uint64_t *ignored)
{
    sigset_t s;
    *mask = 0;
    if (!pthread_sigmask(SIG_BLOCK, NULL, &s))
        memcpy(mask, &s, sizeof(*mask));
    *ignored = 0;
    for (int n = 1; n <= 64; n++) {
        struct sigaction sa;
        if (!sigaction(n, NULL, &sa) && sa.sa_handler == SIG_IGN)
            *ignored |= 1ull << (n - 1);
    }
}

/* The child's process group and session as r asks: 0 means its own pid.
 * 0, or -errno. */
static long child_ids(const struct jam_spawn_req *r, struct posixfds_state *s)
{
    if (r->flags & POSIX_SPAWN_SETSID) {
        s->sid = s->pgid = 0;
        return (r->flags & POSIX_SPAWN_SETPGROUP) && r->pgrp ? -EPERM : 0;
    }
    s->sid = (uint32_t)lx_sid();
    s->pgid = (uint32_t)lx_pgid();
    if (!(r->flags & POSIX_SPAWN_SETPGROUP))
        return 0;
    if (r->pgrp < 0)
        return -EINVAL;
    if (r->pgrp && !lx_group_exists(r->pgrp))
        return -EPERM;
    s->pgid = (uint32_t)r->pgrp;
    return 0;
}

/* The state a child of ours gets (exec: the state exec keeps, ours). */
static long child_state(const struct jam_spawn_req *r, bool exec, struct posixfds_state *s)
{
    uint64_t mask, ignored;
    our_signals(&mask, &ignored);
    if (r->flags & POSIX_SPAWN_SETSIGMASK)
        mask = r->sigmask;
    if (r->flags & POSIX_SPAWN_SETSIGDEF)
        ignored &= ~(uint64_t)r->sigdef;
    const char *cwd;
    const struct posixfds_state *ho = lx_handover(&cwd);
    *s = (struct posixfds_state){
        .umask = spawnfd_umask(ho ? ho->umask : UMASK_DEFAULT),
        .sigmask = mask & ~SIG_NEVER, .sigignore = ignored & ~SIG_NEVER,
    };
    if (exec) {
        s->ppid = (uint32_t)getppid();
        s->pgid = (uint32_t)lx_pgid();
        s->sid = (uint32_t)lx_sid();
        return 0;
    }
    s->ppid = (uint32_t)lx_pid();
    return child_ids(r, s);
}

/* ---- the spawn ------------------------------------------------------------------------- */

/* spawn's status as posix_spawn's errno value. */
static int spawn_errno(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:     return ENOENT;
    case ERR_ACCESS_DENIED: return EACCES;
    case ERR_INVALID_ARGS:  return ENOEXEC;   /* not a program spawn can load */
    case ERR_OUT_OF_RANGE:  return E2BIG;     /* argv and the environment don't fit */
    }
    return (int)-lx_err(st);
}

static int count(char *const *v)
{
    int n = 0;
    while (v && v[n])
        n++;
    return n;
}

/* Start the program name (a bootfs name) with ho; *proc on success.
 * ho's handles are consumed whatever happens. */
static status_t start(const struct jam_spawn_req *r, const char *name,
                      const struct posixfds_out *ho, handle_t *proc)
{
    static const char *const no_env[] = { NULL };
    struct spawn_args a = {
        .path = name,
        .argc = count(r->argv),
        .argv = (const char *const *)r->argv,
        .job = startup_handle(SR_JOB),
        .envp = r->envp ? (const char *const *)r->envp : no_env,
        .ns = r->grants ? r->grants : NS_ALL,
        .stack_size = CHILD_STACK,
        .handover = ho,
    };
    return spawn(&a, proc);
}

/* Everything but the table: the program, the state, the start, the pid.
 * 0 with *pid and *proc, or an errno value. The table's handles in ho are
 * consumed either way. */
static int launch(const struct jam_spawn_req *r, bool exec, struct spawnfd *t,
                  struct posixfds_out *ho, int *pid, handle_t *proc)
{
    char *name = malloc(PATH_MAX);
    long e = !name ? -ENOMEM : r->path ? find_program(r, t->cwd, name) : -EFAULT;
    if (!e)
        e = child_state(r, exec, &ho->state);
    if (e) {
        posixfds_discard(ho);
        free(name);
        return (int)-e;
    }
    ho->cwd = t->cwd;
    status_t st = start(r, name, ho, proc);
    free(name);
    if (st != OK)
        return spawn_errno(st);
    struct process_info info;
    st = jam_process_get_info(*proc, &info);
    if (st != OK) {   /* a child without a pid can't be waited for */
        jam_process_kill(*proc);
        jam_handle_close(*proc);
        return (int)-lx_err(st);
    }
    *pid = (int32_t)info.pid;
    return 0;
}

/* The spawn itself: the table, then launch; the child into the table of
 * children unless exec (whose waits are its own too). */
static int spawn_child(const struct jam_spawn_req *r, bool exec, int *res)
{
    if (r->flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER))
        return ENOSYS;   /* no scheduling policies to give */
    struct spawnfd *t = NULL;
    struct posixfds_out ho = { 0 };
    long e = spawnfd_begin(&t);
    if (!e)
        e = spawnfd_apply(t, r->ops, r->nops);
    if (!e)
        e = spawnfd_export(t, &ho);
    int ec = (int)-e, pid = 0;
    handle_t proc = HANDLE_INVALID;
    if (!e)
        ec = launch(r, exec, t, &ho, &pid, &proc);
    struct posixfds_state s = ho.state;   /* the ids it was told */
    spawnfd_end(t, &ho);
    if (ec)
        return ec;
    e = lx_child_add(proc, pid, s.pgid ? (int32_t)s.pgid : pid, s.sid ? (int32_t)s.sid : pid);
    if (e)
        return (int)-e;
    if (res)
        *res = pid;
    return 0;
}

int __jam_posix_spawn(int *pid, const struct jam_spawn_req *r)
{
    return spawn_child(r, false, pid);
}

long lx_execve(long path, long argv, long envp, long d, long e, long f)
{
    const struct jam_spawn_req r = {
        .path = (const char *)(uintptr_t)path,
        .argv = (char *const *)(uintptr_t)argv,
        .envp = (char *const *)(uintptr_t)envp,
    };
    int pid;
    int ec = spawn_child(&r, true, &pid);
    if (ec)
        return -ec;
    /* Our descriptors are the new program's now: we keep none open (a
     * pipe's end held here would hide its end of file). */
    struct fd_desc *dsc;
    bool cloexec;
    for (long fd = fd_next(0, &dsc, &cloexec); fd >= 0; fd = fd_next(fd + 1, &dsc, &cloexec)) {
        fd_put(dsc);
        (void)fd_close(fd);
    }
    int64_t code;
    if (lx_child_wait(pid, &code))
        code = LX_EXIT_SIGNAL + SIGKILL;   /* never: the child is ours, and only we reap it */
    jam_process_exit(code);
}
