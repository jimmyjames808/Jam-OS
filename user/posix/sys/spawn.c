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
 *   3. The program: a name looked up in PATH (posix_spawnp), then a path
 *      (relative ones from the copy's directory): one under /boot runs
 *      from the boot image, as do /bin and /usr/bin, which are the boot
 *      image's bin and usr/bin (there is no other); a file anywhere else
 *      runs only as the owner approved it, sealed and started by init's
 *      /svc/exec (file_program; EACCES without `svc exec` in our list),
 *      ENOENT if missing. Before step 4, which takes things out of ours.
 *   4. The hand-over: every slot without FD_CLOEXEC exported (new handles
 *      of the child's own), the directory, the umask, the signal mask and
 *      the ignored signals, our pid as its parent's, its process group and
 *      session (0 meaning its own pid, which nobody knows before it is
 *      made).
 *   5. spawn: our job, our namespace (or the grants list
 *      posix_spawnattr_setgrants_np gave), an 8 MiB stack, the hand-over
 *      written before the child runs. A program sealed by /svc/exec is
 *      started by init instead (start_sealed), in a job of its own inside
 *      ours, with its own approval's namespace and powers, never ours
 *      (the owner's rule, docs/CR-PLAN.md track CE1): the hand-over goes
 *      to init, which puts it on the child's startup channel. Then its
 *      pid, and a place in the table of children (proc.c).
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
#include <idl/exec.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include "proc.h"
#include "sig.h"
#include "spawn.h"

#define CHILD_STACK  (8ull << 20)   /* a POSIX child's stack: Linux's usual 8 MiB */
#define SIG_NEVER    ((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)))

/* ---- the program --------------------------------------------------------------------- */

/* Where the boot image's programs are, as paths: /boot itself, and /bin
 * and /usr/bin, its bin and usr/bin (there is no other /bin), so musl's
 * /bin/sh and its default PATH find them. */
static const struct { const char *path; const char *name; } boot_dirs[] = {
    { "/boot/", "" }, { "/bin/", "bin/" }, { "/usr/bin/", "usr/bin/" },
};

/* The program to start. */
struct program {
    char    *name;       /* PATH_MAX bytes: a bootfs name, or a file's absolute path */
    handle_t vmo;        /* a program of ours to run from (none yet: 0, the boot image's) */
    uint64_t size;       /* its bytes in vmo */
    handle_t sealed;     /* a file's program sealed by /svc/exec: its exec_prog channel
                          * (init starts it), or 0 */
    handle_t handover;   /* with sealed: our end of the channel its hand-over goes on */
};

/* /svc/exec's refusal as posix_spawn's errno value. */
static long exec_errno(status_t st)
{
    switch (st) {
    case ERR_ACCESS_DENIED:  return -EACCES;    /* not allowed, or changed since it was */
    case ERR_NOT_SUPPORTED:  return -EACCES;    /* (a server that doesn't seal) */
    case ERR_WRONG_TYPE:     return -ENOEXEC;   /* no program spawn can load */
    case ERR_BUSY:           return -ETXTBSY;   /* open for writing */
    case ERR_NOT_FOUND:      return -ENOENT;
    case ERR_NO_RESOURCES:   return -EAGAIN;
    case ERR_OUT_OF_RANGE:   return -E2BIG;
    }
    return lx_err(st);
}

/* A program on a file system (abs) sealed to run, the way the shell runs
 * one on /data (docs/M12.5-PLAN.md, "Programs from /data"): only as the
 * owner allowed it (`allow`'s line for exactly this path, and its page
 * table), from the file's own pages, each checked by the kernel against
 * the approval as it is fetched. Only a holder of RIGHT_ROOT_VMEX may seal
 * one, with a table from /data/etc, which no program's view has: init does
 * it for us (/svc/exec, abi/idl/exec.idl), given our own open of the file
 * (so our views decide what we can name). Its answer is the sealed program
 * (*sealed), which init also starts (start_sealed), and *handover, our end
 * of the channel the child's hand-over goes on. Without `svc exec` in our
 * list: EACCES, as before there was a sealer. Not allowed, or changed
 * since: EACCES; no program spawn can load: ENOEXEC; open for writing:
 * ETXTBSY. */
static long file_program(const char *abs, handle_t *sealed, handle_t *handover)
{
    handle_t svc = svc_get(SVC_EXEC);   /* libos's to keep */
    if (!svc)
        return -EACCES;   /* not ours to ask */
    struct jfile f;
    status_t st = file_open(abs, FS_READ, &f);
    if (st != OK)
        return st == ERR_IS_DIR ? -EACCES : lx_err(st);
    handle_t file, buf, mine, theirs, prog;
    file_give(&f, &file, &buf);   /* the file goes to init; its buffer is no use there */
    jam_handle_close(buf);
    st = jam_channel_create(&mine, &theirs);
    if (st != OK) {
        jam_handle_close(file);
        return lx_err(st);
    }
    uint8_t posix;
    char wants[1024];   /* unread: the list is init's to act on */
    uint32_t wl;
    st = exec_open(svc, abs, (uint32_t)strlen(abs), file, theirs, &prog, &posix, wants, &wl);
    if (st != OK) {
        jam_handle_close(mine);
        return exec_errno(st);
    }
    *sealed = prog;
    *handover = mine;
    return 0;
}

/* The program at abs (absolute, cleaned) into *p: a bootfs name, or a
 * file's sealed VMO (file_program). 0, or -ENOENT (missing), -ENOTDIR,
 * -EACCES (a directory, or a file we may not run). */
static long program(const char *abs, struct program *p)
{
    char *name = p->name;
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
    if (st != OK || dir)
        return -EACCES;
    long e = file_program(abs, &p->sealed, &p->handover);
    if (!e)
        strcpy(name, abs);   /* fits: both are PATH_MAX */
    return e;
}

/* The program for r into *prog: its path, or its name looked up in
 * r->search (a PATH: ':'-separated; an empty part is the directory dir). */
static long find_program(const struct jam_spawn_req *r, const char *dir, struct program *prog)
{
    char *abs = malloc(PATH_MAX), *part = malloc(PATH_MAX);
    long e = abs && part ? 0 : -ENOMEM;
    if (!e && (!r->search || strchr(r->path, '/'))) {
        e = spawn_absolute(r->path, dir, abs);
        if (!e)
            e = program(abs, prog);
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
                e = program(abs, prog);
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
    sig_spawn_masks(&mask, &ignored);
    if (r->flags & POSIX_SPAWN_SETSIGMASK)
        mask = r->sigmask;
    if (r->flags & POSIX_SPAWN_SETSIGDEF)
        ignored &= ~(uint64_t)r->sigdef;
    *s = (struct posixfds_state){
        .umask = lx_umask_now(),
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

/* argc strings of argv and then those of envp, each with its NUL, into
 * a new *out (*len bytes; the caller frees it). ERR_OUT_OF_RANGE: past
 * what /svc/exec takes. */
static status_t pack_strings(int argc, char *const *argv, char *const *envp, uint8_t **out,
                             uint32_t *len)
{
    uint8_t *b = malloc(EXEC_STRINGS_BYTES);
    uint32_t n = 0, count = 0;
    if (!b)
        return ERR_NO_MEMORY;
    for (int pass = 0; pass < 2; pass++) {
        char *const *v = pass ? envp : argv;
        for (int i = 0; v && v[i] && (pass || i < argc); i++, count++) {
            size_t l = strlen(v[i]) + 1;
            if (l > EXEC_STRINGS_BYTES - n || count == EXEC_STRINGS_MAX) {
                free(b);
                return ERR_OUT_OF_RANGE;
            }
            memcpy(b + n, v[i], l);
            n += (uint32_t)l;
        }
    }
    *out = b;
    *len = n;
    return OK;
}

/* Start the sealed program p through init (/svc/exec's exec_prog.start):
 * ho written on p's hand-over end first (consumed whatever happens), the
 * child's end of a new signal channel given. *proc (wait and inspect),
 * *job (its job: kill), *sigch (our end) on success. The child's
 * namespace and powers are its approval's (launch refuses
 * posix_spawnattr_setgrants_np for it: EPERM). */
static status_t start_sealed(const struct jam_spawn_req *r, const struct program *p,
                             const struct posixfds_out *ho, handle_t *proc, handle_t *job,
                             handle_t *sigch)
{
    uint8_t *strings = NULL;
    uint32_t len = 0;
    handle_t theirs = HANDLE_INVALID, ours_job = HANDLE_INVALID;
    status_t st = posixfds_write(p->handover, ho);   /* init moves it to the child */
    if (st == OK)
        st = pack_strings(count(r->argv), r->argv, r->envp, &strings, &len);
    if (st == OK)
        st = jam_handle_duplicate(startup_handle(SR_JOB), RIGHT_SAME, &ours_job);
    if (st == OK)
        st = jam_channel_create(sigch, &theirs);
    if (st != OK) {
        free(strings);
        if (ours_job)
            jam_handle_close(ours_job);
        return st;
    }
    st = exec_prog_start(p->sealed, (uint16_t)count(r->argv), strings, len, ours_job, theirs,
                         proc, job);   /* ours_job and theirs are init's now */
    free(strings);
    if (st != OK)
        jam_handle_close(*sigch);
    return st;
}

/* Start the program p with ho and a signal channel (its end as
 * SR_SIGNALS); *proc, *job (0 unless init started it: then its job, for
 * a kill) and our end of the channel (*sigch) on success. ho's handles
 * are consumed whatever happens; p's handles stay ours. */
static status_t start(const struct jam_spawn_req *r, const struct program *p,
                      const struct posixfds_out *ho, handle_t *proc, handle_t *job,
                      handle_t *sigch)
{
    static const char *const no_env[] = { NULL };
    *job = HANDLE_INVALID;
    if (p->sealed)
        return start_sealed(r, p, ho, proc, job, sigch);
    handle_t theirs;
    status_t st = jam_channel_create(sigch, &theirs);
    if (st != OK) {
        posixfds_discard(ho);
        return st;
    }
    struct spawn_handle x = { SR_SIGNALS, theirs };
    struct spawn_args a = {
        .path = p->name,
        .vmo = p->vmo,
        .size = p->size,
        .argc = count(r->argv),
        .argv = (const char *const *)r->argv,
        .job = startup_handle(SR_JOB),
        .envp = r->envp ? (const char *const *)r->envp : no_env,
        .ns = r->grants ? r->grants : NS_ALL,
        .stack_size = CHILD_STACK,
        .handover = ho,
        .extra = &x,
        .nextra = 1,
    };
    st = spawn(&a, proc);
    if (st != OK)
        jam_handle_close(*sigch);
    return st;
}

/* A started child: its pid, its process, its job (/svc/exec's children:
 * the kill's way; else 0) and our end of its signal channel. */
struct started {
    int      pid;
    handle_t proc;
    handle_t job;
    handle_t sigch;
};

/* Everything after the table: the state, the start, the pid. 0 with *c,
 * or an errno value. The table's handles in ho are consumed either way. */
static int launch(const struct jam_spawn_req *r, bool exec, struct spawnfd *t,
                  const struct program *p, struct posixfds_out *ho, struct started *c)
{
    long e = child_state(r, exec, &ho->state);
    if (e) {
        posixfds_discard(ho);
        return (int)-e;
    }
    ho->cwd = t->cwd;
    if (p->sealed && r->grants) {   /* its authority is its approval alone: none of ours */
        posixfds_discard(ho);
        return EPERM;
    }
    status_t st = start(r, p, ho, &c->proc, &c->job, &c->sigch);
    if (st != OK)
        return p->sealed ? (int)-exec_errno(st) : spawn_errno(st);
    struct process_info info;
    st = jam_process_get_info(c->proc, &info);
    if (st != OK) {
        if (c->job)
            jam_job_kill(c->job);
        else
            jam_process_kill(c->proc);
        jam_handle_close(c->proc);
        jam_handle_close(c->sigch);
        if (c->job)
            jam_handle_close(c->job);
        return (int)-lx_err(st);
    }
    c->pid = (int32_t)info.pid;
    return 0;
}

/* The spawn itself: the table, the program, then launch; the child into
 * the table of children. The program is found before the table is
 * exported: exporting moves things out of ours (a socket, a pipe's
 * leftover bytes, a writable file's open), so a spawn that fails for want
 * of its program (every PATH directory execvp tries before the right one)
 * must fail before it. */
static int spawn_child(const struct jam_spawn_req *r, bool exec, int *res)
{
    if (r->flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER))
        return ENOSYS;   /* no scheduling policies to give */
    struct spawnfd *t = NULL;
    struct posixfds_out ho = { 0 };
    struct program prog = { malloc(PATH_MAX), HANDLE_INVALID, 0, HANDLE_INVALID,
                            HANDLE_INVALID };
    long e = prog.name ? spawnfd_begin(&t) : -ENOMEM;
    if (!e)
        e = spawnfd_apply(t, r->ops, r->nops);
    if (!e)
        e = r->path ? find_program(r, t->cwd, &prog) : -EFAULT;
    if (!e)
        e = spawnfd_export(t, &ho);
    int ec = (int)-e;
    struct started c = { 0, HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    if (!e)
        ec = launch(r, exec, t, &prog, &ho, &c);
    struct posixfds_state s = ho.state;   /* the ids it was told */
    spawnfd_end(t, &ho);
    if (prog.vmo)
        jam_handle_close(prog.vmo);   /* the child maps what it needs of it */
    if (prog.sealed)
        jam_handle_close(prog.sealed);   /* started, or init drops it */
    if (prog.handover)
        jam_handle_close(prog.handover);
    free(prog.name);
    if (ec)
        return ec;
    e = lx_child_add(c.proc, c.sigch, c.job, c.pid, s.pgid ? (int32_t)s.pgid : c.pid,
                     s.sid ? (int32_t)s.sid : c.pid);
    if (e)
        return (int)-e;
    if (res)
        *res = c.pid;
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
