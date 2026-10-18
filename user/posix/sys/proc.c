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
 * Children. posix_spawn and fork put each child here (lx_child_add): its process
 * handle, its signal channel (our end: kill's way to it), pid, group and
 * session (and for a program from /data, which init's /svc/exec starts,
 * its job: that process handle can't kill it, the job can), and a serial number that keys its binding on the signal
 * thread's port (sig.h's sig_port): SIG_TERMINATED, PORT_BIND_ONCE. A
 * session of init's (pty.c's jamos_session_open) is a child too
 * (lx_session_add): its shell's process, which we may wait on and inspect
 * but not kill, and its session_ctl channel in place of a signal channel
 * (kill's signals go there as session_ctl.signal: SIGHUP, SIGTERM and
 * SIGKILL hang it up). The
 * signal thread is the port's only reader: it hands each child's packet
 * to lx_child_packet, which marks the child dead (notice), wakes the
 * waits and sends SIGCHLD (sig_child_ended, once per child, whoever
 * noticed it). A wait reports and reaps (closes) a dead child that matches
 * it. A child nobody waits for is just two handles, closed when we exit;
 * with SIGCHLD ignored (or SA_NOCLDWAIT) a child is reaped as it is
 * noticed, and waits never see it.
 *
 * Notices (docs/FORK-PLAN.md section 1.7, signal.idl's events to the
 * client). A child tells us what it changes of itself that our table
 * keeps: its process group (setpgid on itself), a session of its own
 * (setsid), and a vfork child its exec (vfork's wait, below). Each child's
 * end of its signal channel is bound to the signal thread's port too (its
 * key with KEY_NOTICE), and marked to take no handles and queue at most
 * CHANNEL_BOUND_BYTES; the signal thread reads a few notices a turn. A
 * look at the groups (kill and wait of a group, getpgid and getsid of a
 * child, setpgid, /proc) first reads what is queued, so a notice sent
 * before the child did anything else we can see is never missed. Nothing
 * in a notice is trusted: one written by any process but the child (its
 * server end handed on), one that breaks the format, a group outside the
 * child's session that isn't its own pid: dropped. A message too big for
 * any notice ends our listening to that child (it stays bound in the
 * channel, charged to its writer, until the child is reaped). A child
 * that lies can only change which of our group signals and group waits
 * reach it.
 *
 * vfork's wait (section 3, Q3 (A)): vfork is fork, and the parent's call
 * returns once its child has exec'd (the child's exec notice, sent the
 * last thing before the replace, or once its execve's program is started
 * when it isn't exec'd in place) or has ended. Signals that come meanwhile
 * wait until it returns, as Linux's vfork holds them; a default action
 * that ends the process doesn't wait.
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
 * with it held, and no malloc: it is the first of the locks fork holds
 * across its clone (fork.c's list), which musl's malloc lock comes
 * before. */
#include <stdlib.h>
#include <errno.h>
#include <idl/session.h>
#include <idl/signal.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "proc.h"
#include "sig.h"

#define CHILD_MAX      (PORT_MAX_BINDINGS / 2)   /* children alive or unreaped at once: two
                                                  * bindings each, its end and its notices */
#define ONE_USER       1000                      /* the one user's uid (M13 Q7) */
#define KEY_NOTICE     (1ull << 62)              /* | a child's key: its notices' binding */
#define NOTICE_TURN    16                        /* notices read from one child at a time */
#define NOTICE_BYTES   64                        /* a notice and some junk; bigger: deaf */
#define NOTICE_WAIT_NS NS_PER_S                  /* how long our notice waits for room */

struct child {
    uint64_t key;     /* its port bindings' key: a serial, never 0, never reused, below
                       * KEY_NOTICE */
    handle_t proc;    /* its process handle; ours until it is reaped */
    handle_t sigch;   /* our end of its signal channel (`signal`), or 0; a session's
                       * session_ctl */
    handle_t job;     /* its job (RIGHT_MANAGE) when its process can't be killed through
                       * proc (/svc/exec's children), or 0 */
    int32_t  pid;     /* the kernel's pid */
    int32_t  pgid;    /* its process group */
    int32_t  sid;     /* its session */
    bool     dead;    /* its SIG_TERMINATED packet was taken */
    bool     session; /* one of init's sessions (sigch is its session_ctl) */
    bool     execed;  /* its exec notice came (a vfork child's) */
    bool     deaf;    /* it sent what is no notice: its notices aren't read any more */
};

static lock_t       lock;
static struct child kids[CHILD_MAX];   /* the first nkids are in use; lock */
static unsigned     nkids;             /* lock */
static uint64_t     next_key = 1;      /* lock; below SIG_KEY_OURS */
static uint32_t     gen;               /* bumped (under lock) on news; a futex */
static int32_t      exec_child;        /* execve's child, or 0; lock */
static uint64_t     reaped_cpu_ns;     /* CPU time of the children a wait reaped; lock */
static bool         vforked;           /* we are a vfork child that hasn't exec'd (atomic) */
static bool         unheard;           /* a notice of ours found no room in a second: our
                                        * starter doesn't read them (atomic) */

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

/* fork's step (fork.c): the ids and the table whole across the clone; in
 * the child, its pid the kernel's, its parent us, our group and session,
 * and its table of children empty (cleared, not closed: those handles
 * were the parent's; docs/FORK-PLAN.md section 1.8). */
void proc_fork(int step)
{
    if (step == LX_FORK_TAKE) {
        lock_take(&lock);
        ids_init_locked();   /* our pid: the child's parent's */
        return;
    }
    if (step == LX_FORK_GIVE) {
        lock_give(&lock);
        return;
    }
    lock = (lock_t){ 0 };
    struct process_info info;
    parent_pid = self_pid;
    self_pid = jam_process_get_info(lx_self_process(), &info) == OK ? (int32_t)info.pid : 0;
    nkids = 0;
    exec_child = 0;
    reaped_cpu_ns = 0;
    vforked = false;   /* vfork sets it in its child after this */
    unheard = false;   /* our starter is the parent now, which reads them */
}

/* ---- the table -------------------------------------------------------------------- */

static struct child *find_locked(int32_t pid)
{
    for (unsigned i = 0; i < nkids; i++)
        if (kids[i].pid == pid)
            return &kids[i];
    return NULL;
}

/* The notices (below): what the children have sent, read before a look at
 * their groups. */
static bool group_in_locked(int32_t pgid, int32_t sid);
static bool hear_locked(struct child *c, bool *news);
static void hear_all_locked(bool *news);
static void give_heard(bool news);

bool lx_group_exists(int32_t pgid)
{
    bool news = false;
    lock_take(&lock);
    ids_init_locked();
    hear_all_locked(&news);
    bool found = group_in_locked(pgid, self_sid);
    give_heard(news);
    return found;
}

/* The three below ask the kernel with the table's lock held: its calls are
 * short (a process_get_maps waits only for the region lock, which a fault's
 * wait for a pager gives up), and the handle can't be reaped meanwhile. */
long lx_child_facts(int32_t pid, struct lx_child_facts *out)
{
    bool news = false;
    lock_take(&lock);
    struct child *c = find_locked(pid);
    long r = c ? 0 : -ESRCH;
    if (c) {
        (void)hear_locked(c, &news);
        out->pgid = c->pgid;
        out->sid = c->sid;
        out->dead = c->dead;
        if (jam_process_get_info(c->proc, &out->info) != OK)
            out->info = (struct process_info){ .state = PROCESS_DEAD, .killed = 1,
                                               .exit_code = PROCESS_KILLED_CODE, .pid = (uint32_t)pid };
    }
    give_heard(news);
    return r;
}

bool lx_child_exists(int32_t pid)
{
    lock_take(&lock);
    bool yes = find_locked(pid) != NULL;
    lock_give(&lock);
    return yes;
}

long lx_child_maps(int32_t pid, uint64_t from, struct map_info *out, uint32_t cap)
{
    lock_take(&lock);
    const struct child *c = find_locked(pid);
    long r = c ? kp_process_maps(c->proc, from, out, cap) : -ESRCH;
    lock_give(&lock);
    return r;
}

unsigned lx_child_pids(int32_t *out, unsigned cap)
{
    lock_take(&lock);
    for (unsigned i = 0; i < nkids && i < cap; i++)
        out[i] = kids[i].pid;
    unsigned n = nkids;
    lock_give(&lock);
    return n;
}

/* Does c have notices to read (a signal channel of its own, listened to)? */
static bool hears(const struct child *c)
{
    return c->sigch && !c->session && !c->deaf;
}

/* c into the table, its end bound to the signal thread's port, and its
 * signal channel for its notices (marked first: no handles, and a bound on
 * what a child that floods it keeps queued; a mark that fails, on an end
 * not ours alone, only leaves that out). */
static status_t add(struct child c)
{
    lock_take(&lock);
    status_t st = nkids == CHILD_MAX ? ERR_NO_RESOURCES : OK;
    c.key = next_key;
    if (st == OK)
        st = jam_port_bind(sig_port(), c.proc, c.key, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st == OK && hears(&c)) {
        (void)jam_channel_set_flags(c.sigch, CHANNEL_NO_HANDLES | CHANNEL_BYTE_BOUND);
        st = jam_port_bind(sig_port(), c.sigch, c.key | KEY_NOTICE, SIG_READABLE,
                           PORT_BIND_PERSISTENT);   /* what is queued already: a packet at once */
        if (st != OK)
            (void)jam_port_unbind(sig_port(), c.proc, c.key);
    }
    if (st == OK) {
        next_key++;
        kids[nkids++] = c;
    }
    lock_give(&lock);
    return st;
}

long lx_child_add(handle_t proc, handle_t sigch, handle_t job, int32_t pid, int32_t pgid,
                  int32_t sid)
{
    status_t st = add((struct child){ .proc = proc, .sigch = sigch, .job = job, .pid = pid,
                                      .pgid = pgid, .sid = sid });
    if (st != OK) {
        if (job)   /* a child we could never wait for */
            jam_job_kill(job);
        else
            jam_process_kill(proc);
        jam_handle_close(proc);
        jam_handle_close(sigch);
        if (job)
            jam_handle_close(job);
        return st == ERR_NO_MEMORY ? -ENOMEM : -EAGAIN;
    }
    return 0;
}

long lx_session_add(handle_t proc, handle_t ctl, int32_t pid)
{
    status_t st = add((struct child){ .proc = proc, .sigch = ctl, .pid = pid, .pgid = pid,
                                      .sid = pid, .session = true });
    if (st != OK) {
        (void)session_ctl_hangup(ctl);   /* a session we could never wait for */
        jam_handle_close(proc);
        jam_handle_close(ctl);
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
    if (hears(&kids[i]))   /* gone with the handle anyway, unless a kill holds a duplicate */
        (void)jam_port_unbind(sig_port(), kids[i].sigch, kids[i].key | KEY_NOTICE);
    jam_handle_close(kids[i].proc);
    jam_handle_close(kids[i].sigch);
    if (kids[i].job)
        jam_handle_close(kids[i].job);
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

/* ---- notices ---------------------------------------------------------------------- */

/* Is pgid a group of session sid we know of: ours, or one of our
 * children's? lock held. */
static bool group_in_locked(int32_t pgid, int32_t sid)
{
    ids_init_locked();
    if (pgid == self_pgid && sid == self_sid)
        return true;
    for (unsigned i = 0; i < nkids; i++)
        if (kids[i].pgid == pgid && kids[i].sid == sid)
            return true;
    return false;
}

struct heard {
    struct child *c;
    bool          news;   /* something a wait looks at changed: gen bumped */
};

static void on_exec(void *ctx)
{
    struct heard *h = ctx;
    h->c->execed = true;
    h->news = true;
}

/* A group c may say it is in: its own pid, or one of its session we know
 * of (one of its own children's we don't: dropped). */
static void on_group(void *ctx, int32_t pgid)
{
    struct child *c = ((struct heard *)ctx)->c;
    if (pgid == c->pid || (pgid > 0 && group_in_locked(pgid, c->sid)))
        c->pgid = pgid;
}

static void on_session(void *ctx)
{
    struct child *c = ((struct heard *)ctx)->c;
    c->pgid = c->sid = c->pid;   /* nothing claimed: a session's id is its leader's pid */
}

static const struct signal_events notices = {
    .exec = on_exec, .group = on_group, .session = on_session,
};

/* Read what c has sent on its signal channel, NOTICE_TURN at most, and act
 * on each (in the order sent: every reader holds the lock, and no read
 * waits). True if more may be queued; *news if a wait should look again
 * (gen bumped: wake_sleepers once the lock is given up). lock held. */
static bool hear_locked(struct child *c, bool *news)
{
    for (unsigned n = 0; hears(c) && n < NOTICE_TURN; n++) {
        _Static_assert(NOTICE_BYTES >= SIGNAL_REP_MAX, "room for every notice");
        _Alignas(8) uint8_t b[NOTICE_BYTES];
        uint32_t sizes[3] = { 0 };   /* its bytes, its handles, its writer */
        struct idl_msg m = { 0 };
        struct channel_read_args a = {
            .h = c->sigch, .rd_cap = sizeof(b), .rd_bytes = (uint64_t)(uintptr_t)b,
            .rd_handles = (uint64_t)(uintptr_t)m.hs, .rd_hcap = IDL_HANDLES_MAX,
            .rd_flags = CHANNEL_READ_PID, .rd_actual = (uint64_t)(uintptr_t)&sizes[0],
            .rd_hactual = (uint64_t)(uintptr_t)&sizes[1],
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {   /* far bigger than any notice: junk, stuck at the head */
            c->deaf = true;
            (void)jam_port_unbind(sig_port(), c->sigch, c->key | KEY_NOTICE);
            return false;
        }
        if (st != OK)
            return false;   /* none queued, or the child has gone */
        m.n = sizes[0];
        m.nh = sizes[1];
        if (sizes[2] != (uint32_t)c->pid) {   /* another process holding the child's end */
            idl_msg_drop(&m);
            continue;
        }
        struct heard h = { .c = c };
        (void)signal_event_dispatch(&notices, &h, b, &m);   /* junk: dropped, handles closed */
        if (h.news) {
            news_locked();
            *news = true;
        }
    }
    return hears(c);
}

/* What every child has sent, before a look at the groups. lock held. */
static void hear_all_locked(bool *news)
{
    for (unsigned i = 0; i < nkids; i++)
        (void)hear_locked(&kids[i], news);
}

/* The lock given up, and the waits woken if a notice changed what they
 * look at. */
static void give_heard(bool news)
{
    lock_give(&lock);
    if (news)
        wake_sleepers();
}

void lx_child_packet(const struct port_packet *pkt)
{
    bool news = false;
    lock_take(&lock);
    for (unsigned i = 0; i < nkids; i++) {
        struct child *c = &kids[i];
        if (c->key != (pkt->key & ~KEY_NOTICE))
            continue;
        if (!(pkt->key & KEY_NOTICE)) {
            if (!c->dead)
                ended(i);
        } else if (hear_locked(c, &news)) {   /* the rest on another turn: others' come first */
            (void)port_again(sig_port(), c->sigch, pkt->key, SIG_READABLE);
        }
        break;
    }
    give_heard(news);
}

/* Our notice to our starter on our end of the signal channel, waiting up
 * to NOTICE_WAIT_NS for room. A starter that doesn't read them (the
 * native shell) fills its end at last: once a notice has waited that
 * long, the rest are dropped at once. */
static void notify(uint32_t what, int32_t pgid)
{
    handle_t ch = startup_handle(SR_SIGNALS);
    uint64_t deadline = now() + NOTICE_WAIT_NS;
    while (ch && !__atomic_load_n(&unheard, __ATOMIC_RELAXED)) {
        status_t st = what == SIGNAL_EXEC    ? signal_exec(ch)
                      : what == SIGNAL_GROUP ? signal_group(ch, pgid)
                                             : signal_session(ch);
        if (st != ERR_SHOULD_WAIT)
            return;   /* sent, or our starter has gone */
        signals_t seen;
        if (jam_object_wait_one(ch, SIG_WRITABLE | SIG_PEER_CLOSED, deadline, &seen) ==
            ERR_TIMED_OUT)
            __atomic_store_n(&unheard, true, __ATOMIC_RELAXED);
    }
}

void lx_vforked(void)
{
    __atomic_store_n(&vforked, true, __ATOMIC_RELEASE);
}

void lx_exec_notice(void)
{
    if (__atomic_exchange_n(&vforked, false, __ATOMIC_ACQ_REL))
        notify(SIGNAL_EXEC, 0);
}

void lx_vfork_wait(int32_t pid)
{
    for (;;) {
        bool news = false;
        lock_take(&lock);
        struct child *c = find_locked(pid);
        if (c)
            (void)hear_locked(c, &news);   /* sent before its channel was bound, or not read yet */
        uint32_t g = __atomic_load_n(&gen, __ATOMIC_ACQUIRE);
        bool go = !c || c->dead || c->execed;   /* gone: reaped (SIGCHLD ignored, or a wait) */
        give_heard(news);
        if (go)
            return;
        (void)jam_futex_wait(&gen, g, DEADLINE_NEVER, 0);   /* interrupted: the signal waits */
    }
}

/* A signal to one child through h (a duplicate, closed here): its
 * session_ctl, its signal channel, or for SIGKILL its process or its job
 * (job: h is one). */
static long signal_one(handle_t h, bool session, bool job, int signo, int code, int32_t value)
{
    long e;
    if (job) {
        e = lx_err(jam_job_kill(h));
    } else if (session) {   /* init's: only a hang-up means anything to it */
        status_t st = session_ctl_signal(h, (uint8_t)signo);
        e = st == ERR_NOT_SUPPORTED || st == ERR_BAD_STATE || st == ERR_PEER_CLOSED ? 0
                                                                                    : lx_err(st);
    } else {
        e = signo == SIGKILL ? lx_err(jam_process_kill(h)) : sig_channel_send(h, signo, code, value);
    }
    jam_handle_close(h);
    return e;
}

long lx_child_signal(int32_t pid, int32_t pgid, int signo, int code, int32_t value)
{
    /* One child (pid > 0, at most one matches) needs no malloc: the signal
     * thread forwards that way (sigthread.c's arrived), and it has no
     * thread pointer for a program's own malloc to use (Chromium's
     * PartitionAlloc faulted there). */
    unsigned n = 0, matched = 0, room = 1;
    struct target { handle_t h; bool session, job; } one, *to = &one;
    lock_take(&lock);
    while (pid <= 0 && nkids > room) {   /* room for each, allocated without the lock (fork.c says why) */
        room = nkids;
        lock_give(&lock);
        if (to != &one)
            jam_free(to);
        to = jam_malloc(sizeof(*to) * room);
        if (!to)
            return -ENOMEM;
        lock_take(&lock);
    }
    bool news = false;
    if (pgid > 0)
        hear_all_locked(&news);
    for (unsigned i = 0; i < nkids; i++) {
        struct child *c = &kids[i];
        if ((pid > 0 && c->pid != pid) || (pgid > 0 && c->pgid != pgid))
            continue;
        matched++;
        bool kill = signo == SIGKILL && !c->session;
        handle_t h = kill ? (c->job ? c->job : c->proc) : c->sigch;
        to[n].session = c->session;
        to[n].job = kill && c->job;
        if (!c->dead && signo && h && jam_handle_duplicate(h, RIGHT_SAME, &to[n].h) == OK)
            n++;
    }
    give_heard(news);
    long r = 0;
    for (unsigned i = 0; i < n; i++) {
        long e = signal_one(to[i].h, to[i].session, to[i].job, signo, code, value);
        if (e && !r)
            r = e;
    }
    if (to != &one)
        jam_free(to);
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
        if (!w->keep) {
            reaped_cpu_ns += out->info.cpu_ns;   /* getrusage(RUSAGE_CHILDREN), times */
            reap_locked(i);
        }
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
        bool news = false;
        lock_take(&lock);
        ids_init_locked();
        if (w->pgid > 0)
            hear_all_locked(&news);
        if (w->nohang)
            peek_locked(w);
        uint32_t g = __atomic_load_n(&gen, __ATOMIC_ACQUIRE);
        long r = take_locked(w, out);
        give_heard(news);
        if (r != 0 || w->nohang)
            return r;
        if (jam_futex_wait(&gen, g, DEADLINE_NEVER, 0) == ERR_INTERRUPTED)
            return -EINTR;
    }
}

/* A child's end as waitpid's status: a fault the kernel killed it for
 * (with no handler, or one that gave it up) is that fault's signal, SIGSEGV
 * for a bad access, SIGILL for ud2 and so on; any other kill is SIGKILL. */
static int wait_status(const struct process_info *info)
{
    int64_t code = info->exit_code;
    if (info->killed && (info->flags & PROCESS_FLAG_FAULT))
        return sig_fault_signal(info->fault_vector, info->fault_status);
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
    struct rusage ru;   /* the caller's are written last (Linux's: the child is reaped) */
    fill_rusage(&ru, &got.info);
    if ((status && uc_put_int(status, wait_status(&got.info))) ||
        (rusage && uc_put((void *)(uintptr_t)rusage, &ru, sizeof(ru))))
        return -EFAULT;
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
    siginfo_t si;   /* the caller's info and rusage are written last */
    memset(&si, 0, sizeof(si));   /* WNOHANG with none: si_pid 0, as Linux */
    if (r)
        fill_siginfo(&si, &got);
    struct process_info none = { 0 };
    struct rusage ru;
    fill_rusage(&ru, r ? &got.info : &none);
    if ((info && uc_put((void *)(uintptr_t)info, &si, sizeof(si))) ||
        (rusage && uc_put((void *)(uintptr_t)rusage, &ru, sizeof(ru))))
        return -EFAULT;
    return 0;
}

long lx_child_wait(int32_t pid, int64_t *code)
{
    lx_exec_notice();   /* the program we exec'd is running: a vfork parent goes on */
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

uint64_t lx_children_cpu_ns(void)
{
    lock_take(&lock);
    uint64_t ns = reaped_cpu_ns;
    lock_give(&lock);
    return ns;
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
    bool news = false;
    lock_take(&lock);
    ids_init_locked();
    long r = -ESRCH;
    if (pid == 0 || pid == self_pid) {
        r = self_pgid;
    } else if (pid > 0 && pid <= INT32_MAX) {
        struct child *c = find_locked((int32_t)pid);
        if (c) {
            (void)hear_locked(c, &news);   /* what it has said of its group first */
            r = c->pgid;
        }
    }
    give_heard(news);
    return r;
}

long lx_getsid(long pid, long b, long c, long d, long e, long f)
{
    bool news = false;
    lock_take(&lock);
    ids_init_locked();
    long r = -ESRCH;
    if (pid == 0 || pid == self_pid) {
        r = self_sid;
    } else if (pid > 0 && pid <= INT32_MAX) {
        struct child *c = find_locked((int32_t)pid);
        if (c) {
            (void)hear_locked(c, &news);
            r = c->sid;
        }
    }
    give_heard(news);
    return r;
}

/* A change of our own group or session is told to our starter (a notice,
 * after the lock is given up): its table of children keeps them. */
long lx_setpgid(long pid, long pgid, long arg3, long d, long e, long f)
{
    if (pid < 0 || pgid < 0 || pid > INT32_MAX || pgid > INT32_MAX)
        return -EINVAL;
    bool news = false, tell = false;
    lock_take(&lock);
    ids_init_locked();
    hear_all_locked(&news);
    long r = 0;
    int32_t target = pid ? (int32_t)pid : self_pid;
    int32_t group = pgid ? (int32_t)pgid : target;
    struct child *c = target == self_pid ? NULL : find_locked(target);
    int32_t sid = c ? c->sid : self_sid;
    if (target != self_pid && !c) {
        r = -ESRCH;
    } else if (sid != self_sid || sid == target) {
        r = -EPERM;   /* another session's, or a session's leader */
    } else if (group != target && !group_in_locked(group, self_sid)) {
        r = -EPERM;   /* an existing group of our session (ours, or a child's) */
    } else if (c) {
        c->pgid = group;
    } else {
        tell = self_pgid != group;
        self_pgid = group;
    }
    give_heard(news);
    if (tell)
        notify(SIGNAL_GROUP, group);
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
    if (r > 0)
        notify(SIGNAL_SESSION, 0);
    return r;
}
