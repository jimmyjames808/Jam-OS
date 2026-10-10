/* Processes (docs/M13-PLAN.md section 5): our ids, process groups and
 * sessions, the table of our children and the waits for them.
 *
 * Ids. getpid is the kernel's small pid (process_info.pid), read once.
 * getppid, our process group and our session come from the hand-over
 * (the starter's pid, and 0 there meaning our own pid); without one the
 * parent is 1 and we lead a session and a group of our own. Groups and
 * sessions are libc's bookkeeping: setpgid and setsid change only what
 * this process and its children are told, never what the kernel knows.
 *
 * Children. posix_spawn puts each child here (lx_child_add): its process
 * handle, its signal channel (our end: kill's way to it), pid, group and
 * session, and a serial number that keys its binding on the signal
 * thread's port (sig.h's sig_port): SIG_TERMINATED, PORT_BIND_ONCE. The
 * signal thread is the port's only reader: it hands each child's packet
 * to lx_child_packet, which marks the child dead (notice), wakes the
 * waits and sends SIGCHLD (sig_child_ended, once per child, whoever
 * noticed it). A wait reports and reaps (closes) a dead child that matches
 * it. A child nobody waits for is just two handles, closed when we exit;
 * with SIGCHLD ignored (or SA_NOCLDWAIT) a child is reaped as it is
 * noticed, and waits never see it.
 *
 * Waiting. A wait looks for a dead child it matches: found, reported;
 * none it could ever match, ECHILD; WNOHANG, 0, after asking each live
 * child it matches whether it has ended (so an end the signal thread
 * hasn't taken yet is seen at once). Otherwise it sleeps on a futex word,
 * `gen`, bumped (and every sleeper woken) whenever an end is noticed, and
 * looks again. The sleep is interruptible: EINTR.
 *
 * Locks: `lock` guards everything below but `gen`, which is read and
 * bumped under it and waited on without it. No call that waits is made
 * with it held. */
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "proc.h"
#include "sig.h"

#define CHILD_MAX  PORT_MAX_BINDINGS   /* children alive or unreaped at once */
#define ONE_USER   1000                /* the one user's uid (M13 Q7) */

struct child {
    uint64_t key;     /* its port binding's key: a serial, never 0, never reused */
    handle_t proc;    /* its process handle; ours until it is reaped */
    handle_t sigch;   /* our end of its signal channel (`signal`), or 0 */
    int32_t  pid;     /* the kernel's pid */
    int32_t  pgid;    /* its process group */
    int32_t  sid;     /* its session */
    bool     dead;    /* its SIG_TERMINATED packet was taken */
};

static lock_t       lock;
static struct child kids[CHILD_MAX];   /* the first nkids are in use; lock */
static unsigned     nkids;             /* lock */
static uint64_t     next_key = 1;      /* lock; below SIG_KEY_OURS */
static uint32_t     gen;               /* bumped (under lock) on news; a futex */
static int32_t      exec_child;        /* execve's child, or 0; lock */

/* ---- our own ids ------------------------------------------------------------------ */

static bool    ids_known;              /* lock */
static int32_t self_pid, parent_pid, self_pgid, self_sid;   /* lock, once ids_known */

static void ids_init_locked(void)
{
    if (ids_known)
        return;
    struct process_info info;
    if (jam_process_get_info(lx_self_process(), &info) == OK)
        self_pid = (int32_t)info.pid;
    const char *cwd;
    const struct posixfds_state *s = lx_handover(&cwd);
    parent_pid = s && s->ppid ? (int32_t)s->ppid : 1;
    self_pgid = s && s->pgid ? (int32_t)s->pgid : self_pid;
    self_sid = s && s->sid ? (int32_t)s->sid : self_pid;
    ids_known = true;
}

/* One of our ids, read under the lock. */
static int32_t read_id(const int32_t *id)
{
    lock_take(&lock);
    ids_init_locked();
    int32_t v = *id;
    lock_give(&lock);
    return v;
}

int32_t lx_pid(void)
{
    return read_id(&self_pid);
}

int32_t lx_pgid(void)
{
    return read_id(&self_pgid);
}

int32_t lx_sid(void)
{
    return read_id(&self_sid);
}

long lx_getpid(long a, long b, long c, long d, long e, long f)
{
    return lx_pid();
}

long lx_getppid(long a, long b, long c, long d, long e, long f)
{
    return read_id(&parent_pid);
}

/* ---- the table -------------------------------------------------------------------- */

static struct child *find_locked(int32_t pid)
{
    for (unsigned i = 0; i < nkids; i++)
        if (kids[i].pid == pid)
            return &kids[i];
    return NULL;
}

bool lx_group_exists(int32_t pgid)
{
    lock_take(&lock);
    ids_init_locked();
    bool found = pgid == self_pgid;
    for (unsigned i = 0; i < nkids && !found; i++)
        found = kids[i].pgid == pgid && kids[i].sid == self_sid;
    lock_give(&lock);
    return found;
}

long lx_child_add(handle_t proc, handle_t sigch, int32_t pid, int32_t pgid, int32_t sid)
{
    lock_take(&lock);
    status_t st = nkids == CHILD_MAX ? ERR_NO_RESOURCES : OK;
    uint64_t key = next_key;
    if (st == OK)
        st = jam_port_bind(sig_port(), proc, key, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st == OK) {
        next_key++;
        kids[nkids++] = (struct child){ .key = key, .proc = proc, .sigch = sigch, .pid = pid,
                                        .pgid = pgid, .sid = sid };
    }
    lock_give(&lock);
    if (st != OK) {
        jam_process_kill(proc);   /* a child we could never wait for */
        jam_handle_close(proc);
        jam_handle_close(sigch);
        return st == ERR_NO_MEMORY ? -ENOMEM : -EAGAIN;
    }
    return 0;
}

/* Bump gen and wake everyone sleeping on it (lock held for the bump). */
static void news_locked(void)
{
    __atomic_add_fetch(&gen, 1, __ATOMIC_RELEASE);
}

static void wake_sleepers(void)
{
    (void)jam_futex_wake(&gen, UINT32_MAX, 0);   /* nobody asleep is fine */
}

static void reap_locked(unsigned i)
{
    jam_handle_close(kids[i].proc);
    jam_handle_close(kids[i].sigch);
    kids[i] = kids[--nkids];
}

static int wait_status(const struct process_info *info);

/* Child i's end noticed (it was alive): marked, the waits woken (lock
 * held); then, without it, SIGCHLD and, if SIGCHLD is ignored, its reaping. */
static void ended(unsigned i)
{
    struct child *c = &kids[i];
    c->dead = true;
    int32_t pid = c->pid;
    uint64_t key = c->key;
    struct process_info info;
    if (jam_process_get_info(c->proc, &info) != OK)
        info = (struct process_info){ .killed = 1, .exit_code = PROCESS_KILLED_CODE };
    news_locked();
    lock_give(&lock);
    wake_sleepers();
    bool reap = sig_child_ended(pid, wait_status(&info));
    lock_take(&lock);
    for (unsigned j = 0; reap && j < nkids; j++)
        if (kids[j].key == key) {   /* still there: no wait took it meanwhile */
            reap_locked(j);
            break;
        }
}

void lx_child_packet(const struct port_packet *pkt)
{
    lock_take(&lock);
    for (unsigned i = 0; i < nkids; i++)
        if (kids[i].key == pkt->key && !kids[i].dead) {
            ended(i);
            break;
        }
    lock_give(&lock);
}

long lx_child_signal(int32_t pid, int32_t pgid, int signo, int code, int32_t value)
{
    lock_take(&lock);
    unsigned n = 0, matched = 0;
    handle_t *hs = malloc(sizeof(handle_t) * (nkids ? nkids : 1));
    for (unsigned i = 0; hs && i < nkids; i++) {
        struct child *c = &kids[i];
        if ((pid > 0 && c->pid != pid) || (pgid > 0 && c->pgid != pgid))
            continue;
        matched++;
        handle_t h = signo == SIGKILL ? c->proc : c->sigch;
        if (!c->dead && signo && h && jam_handle_duplicate(h, RIGHT_SAME, &hs[n]) == OK)
            n++;
    }
    lock_give(&lock);
    if (!hs)
        return -ENOMEM;
    long r = 0;
    for (unsigned i = 0; i < n; i++) {
        long e = signo == SIGKILL ? lx_err(jam_process_kill(hs[i]))
                                  : sig_channel_send(hs[i], signo, code, value);
        if (e && !r)
            r = e;
        jam_handle_close(hs[i]);
    }
    free(hs);
    return r ? r : (long)matched;
}

/* ---- waits ------------------------------------------------------------------------ */

/* Which children a wait is for (waitpid's pid, or waitid's idtype and id). */
struct want {
    int32_t pid;       /* > 0: that child; else any (pgid says which group) */
    int32_t pgid;      /* > 0: only that group's children */
    bool    exited;    /* it reports ends at all (waitid without WEXITED doesn't) */
    bool    nohang;    /* WNOHANG */
    bool    keep;      /* WNOWAIT: report, don't reap */
};

/* What a wait found. */
struct found {
    int32_t             pid;
    struct process_info info;
};

static bool matches(const struct want *w, const struct child *c)
{
    return (w->pid <= 0 || c->pid == w->pid) && (w->pgid <= 0 || c->pgid == w->pgid);
}

/* Look for a dead child w matches: 1 (reported in *out, reaped unless
 * w->keep), 0 (some it matches live on), or -ECHILD. lock held. */
static long take_locked(const struct want *w, struct found *out)
{
    bool any = false;
    for (unsigned i = 0; i < nkids; i++) {
        struct child *c = &kids[i];
        if (!matches(w, c))
            continue;
        any = true;
        if (!c->dead || !w->exited)
            continue;
        out->pid = c->pid;
        if (jam_process_get_info(c->proc, &out->info) != OK)
            out->info = (struct process_info){ .killed = 1, .exit_code = PROCESS_KILLED_CODE };
        if (!w->keep)
            reap_locked(i);
        return 1;
    }
    return any ? 0 : -ECHILD;
}

/* Ask each live child w matches whether it has ended (a wait that won't
 * sleep). lock held (given up and taken again for each end found, so the
 * walk starts over). */
static void peek_locked(const struct want *w)
{
    for (unsigned i = 0; i < nkids; i++) {
        signals_t seen = 0;
        if (kids[i].dead || !matches(w, &kids[i]) ||
            jam_object_wait_one(kids[i].proc, SIG_TERMINATED, 0, &seen) != OK)
            continue;
        ended(i);   /* gives the lock up and takes it again: start over */
        i = (unsigned)-1;
    }
}

/* The wait itself: 1 with *out, 0 (WNOHANG, none yet), or -errno. */
static long wait_for(const struct want *w, struct found *out)
{
    for (;;) {
        lock_take(&lock);
        ids_init_locked();
        if (w->nohang)
            peek_locked(w);
        uint32_t g = __atomic_load_n(&gen, __ATOMIC_ACQUIRE);
        long r = take_locked(w, out);
        lock_give(&lock);
        if (r != 0 || w->nohang)
            return r;
        if (jam_futex_wait(&gen, g, DEADLINE_NEVER, 0) == ERR_INTERRUPTED)
            return -EINTR;
    }
}

/* A child's end as waitpid's status. */
static int wait_status(const struct process_info *info)
{
    int64_t code = info->exit_code;
    if (info->killed || code == PROCESS_KILLED_CODE)
        return SIGKILL;
    if (code > LX_EXIT_SIGNAL && code <= LX_EXIT_SIGNAL + 64)
        return (int)(code - LX_EXIT_SIGNAL) & 0x7f;
    return (int)(code & 0xff) << 8;
}

static void fill_rusage(struct rusage *ru, const struct process_info *info)
{
    memset(ru, 0, sizeof(*ru));
    ru->ru_utime.tv_sec = (time_t)(info->cpu_ns / NS_PER_S);   /* user and system together */
    ru->ru_utime.tv_usec = (suseconds_t)(info->cpu_ns % NS_PER_S / 1000);
}

long lx_wait4(long pid, long status, long options, long rusage, long e, long f)
{
    if (options & ~(long)(WNOHANG | WUNTRACED | WCONTINUED | __WNOTHREAD | __WCLONE | __WALL))
        return -EINVAL;
    if (pid > INT32_MAX || pid < -(long)INT32_MAX)
        return -ESRCH;   /* no such pid, nor group */
    struct want w = { .exited = true, .nohang = options & WNOHANG };
    if (pid > 0)
        w.pid = (int32_t)pid;
    else if (pid == 0)
        w.pgid = lx_pgid();
    else if (pid < -1)
        w.pgid = (int32_t)-pid;
    struct found got;
    long r = wait_for(&w, &got);
    if (r <= 0)
        return r;
    if (status)
        *(int *)(uintptr_t)status = wait_status(&got.info);
    if (rusage)
        fill_rusage((struct rusage *)(uintptr_t)rusage, &got.info);
    return got.pid;
}

/* waitid's siginfo for what it found. */
static void fill_siginfo(siginfo_t *si, const struct found *got)
{
    int s = wait_status(&got->info);
    si->si_signo = SIGCHLD;
    si->si_pid = got->pid;
    si->si_uid = ONE_USER;
    si->si_code = WIFEXITED(s) ? CLD_EXITED : CLD_KILLED;
    si->si_status = WIFEXITED(s) ? WEXITSTATUS(s) : WTERMSIG(s);
}

long lx_waitid(long idtype, long id, long info, long options, long rusage, long f)
{
    const long all = WNOHANG | WSTOPPED | WEXITED | WCONTINUED | WNOWAIT | __WNOTHREAD |
                     __WCLONE | __WALL;
    if ((options & ~all) || !(options & (WEXITED | WSTOPPED | WCONTINUED)))
        return -EINVAL;
    struct want w = { .exited = options & WEXITED, .nohang = options & WNOHANG,
                      .keep = options & WNOWAIT };
    if (idtype == P_PID && id > 0 && id <= INT32_MAX)
        w.pid = (int32_t)id;
    else if (idtype == P_PGID && id >= 0 && id <= INT32_MAX)
        w.pgid = id ? (int32_t)id : lx_pgid();
    else if (idtype != P_ALL)
        return -EINVAL;
    struct found got;
    long r = wait_for(&w, &got);
    if (r < 0)
        return r;
    siginfo_t *si = (siginfo_t *)(uintptr_t)info;
    if (si) {
        memset(si, 0, sizeof(*si));   /* WNOHANG with none: si_pid 0, as Linux */
        if (r)
            fill_siginfo(si, &got);
    }
    if (rusage) {
        struct process_info none = { 0 };
        fill_rusage((struct rusage *)(uintptr_t)rusage, r ? &got.info : &none);
    }
    return 0;
}

long lx_child_wait(int32_t pid, int64_t *code)
{
    lock_take(&lock);
    exec_child = pid;
    lock_give(&lock);
    struct want w = { .pid = pid, .exited = true };
    struct found got;
    long r;
    do
        r = wait_for(&w, &got);
    while (r == -EINTR);   /* execve's caller forwards signals: it never stops waiting */
    lock_take(&lock);
    exec_child = 0;
    lock_give(&lock);
    if (r > 0) {
        int s = wait_status(&got.info);
        *code = WIFEXITED(s) ? WEXITSTATUS(s) : LX_EXIT_SIGNAL + WTERMSIG(s);
    }
    return r > 0 ? 0 : r;
}

int32_t lx_exec_child(void)
{
    lock_take(&lock);
    int32_t pid = exec_child;
    lock_give(&lock);
    return pid;
}

/* ---- process groups and sessions --------------------------------------------------- */

long lx_getpgid(long pid, long b, long c, long d, long e, long f)
{
    lock_take(&lock);
    ids_init_locked();
    long r = -ESRCH;
    if (pid == 0 || pid == self_pid) {
        r = self_pgid;
    } else if (pid > 0 && pid <= INT32_MAX) {
        struct child *c = find_locked((int32_t)pid);
        if (c)
            r = c->pgid;
    }
    lock_give(&lock);
    return r;
}

long lx_getsid(long pid, long b, long c, long d, long e, long f)
{
    lock_take(&lock);
    ids_init_locked();
    long r = -ESRCH;
    if (pid == 0 || pid == self_pid) {
        r = self_sid;
    } else if (pid > 0 && pid <= INT32_MAX) {
        struct child *c = find_locked((int32_t)pid);
        if (c)
            r = c->sid;
    }
    lock_give(&lock);
    return r;
}

/* pgid may be joined by a process of our session: an existing group of
 * it (ours, or a child's in our session). lock held. */
static bool group_in_session_locked(int32_t pgid)
{
    if (pgid == self_pgid)
        return true;
    for (unsigned i = 0; i < nkids; i++)
        if (kids[i].pgid == pgid && kids[i].sid == self_sid)
            return true;
    return false;
}

long lx_setpgid(long pid, long pgid, long arg3, long d, long e, long f)
{
    if (pid < 0 || pgid < 0 || pid > INT32_MAX || pgid > INT32_MAX)
        return -EINVAL;
    lock_take(&lock);
    ids_init_locked();
    long r = 0;
    int32_t target = pid ? (int32_t)pid : self_pid;
    int32_t group = pgid ? (int32_t)pgid : target;
    struct child *c = target == self_pid ? NULL : find_locked(target);
    int32_t sid = c ? c->sid : self_sid;
    if (target != self_pid && !c)
        r = -ESRCH;
    else if (sid != self_sid || sid == target)
        r = -EPERM;   /* another session's, or a session's leader */
    else if (group != target && !group_in_session_locked(group))
        r = -EPERM;
    else if (c)
        c->pgid = group;
    else
        self_pgid = group;
    lock_give(&lock);
    return r;
}

long lx_setsid(long a, long b, long c, long d, long e, long f)
{
    lock_take(&lock);
    ids_init_locked();
    long r = -EPERM;   /* a group's leader can't start a session */
    if (self_pgid != self_pid) {
        self_pgid = self_sid = self_pid;
        r = self_pid;
    }
    lock_give(&lock);
    return r;
}
