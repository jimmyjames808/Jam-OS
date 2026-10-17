/* fork (docs/FORK-PLAN.md section 1, track FL1b): POSIX's fork, vfork and
 * clone without CLONE_VM, on libjam's jam_fork (<os.h>, user/lib/fork.c),
 * which copies our address space at one instant into a new process in our
 * job and starts its one thread where the call was made.
 *
 * The ways in (section 1.11): musl's fork and _Fork (unmodified) make
 * SYS_fork; musl's vfork (its override) makes SYS_vfork, which is fork's
 * core here too, then the parent's wait until the child has exec'd or
 * ended (Q3 (A); proc.c's lx_vfork_wait, the child's notice); a raw
 * SYS_clone without CLONE_VM (lx_clone: CLONE_PARENT_SETTID,
 * CLONE_CHILD_SETTID and CLONE_CHILD_CLEARTID honoured, no new stack); and
 * musl's clone() without CLONE_VM, through the __clone override
 * (__jam_clone_process: the child runs fn(arg) on the stack given and
 * ends with its value). Whatever the child's exit signal says, its end
 * is SIGCHLD. A clone sharing our memory without being a thread of ours
 * (CLONE_VM alone, vfork's Linux way) is ENOSYS: such a child can't be
 * made. What is refused is named in the ENOSYS log.
 *
 * The parent: the child goes in the table of children (lx_child_add, as
 * posix_spawn's children do) with the client end of its new signal
 * channel, so waitpid, SIGCHLD, kill, kill(-pgid) and /proc/<pid> reach it
 * (section 1.8); the call returns its pid. ENOMEM if our job can't pay for
 * the copy (section 1.3), EAGAIN if the table is full (the child is killed
 * then, never seen).
 *
 * The child, before the call returns 0 there (section 1.7; libjam has
 * made the startup roles its own): start.c's handles and thread.c's table
 * the child's; each module's LX_FORK_CHILD step (layer.h); no timers; the
 * tid words CLONE_CHILD_* ask for; then the signal thread, on a port of
 * its own. The exception entry, the process's and so not in the child, is
 * set again by signal.c's step: always, for the checked copies (ucopy.S).
 *
 * Locks (section 1.5). Before the clone the layer takes, in this order,
 * and gives back after it in the reverse (the child makes them fresh):
 *   1. proc.c's lock (our ids, the children)
 *   2. path.c's cwd_lock
 *   3. meta.c's umask_lock
 *   4. fd.c's table_lock
 *   5. mem.c's mem_lock
 *   6. rlimit.c's limits_lock (brk asks the limit with mem_lock held)
 *   7. signal.c's sig_lock
 * Each is a leaf but for that one nesting: none is held across a wait, a
 * call into another of these modules but a later one, or malloc. With
 * musl's fork (third_party/musl/src/process/fork.c, _Fork.c) the whole
 * order is: the program's pthread_atfork prepare handlers; musl's locks
 * (the dynamic linker's, pthread keys', aio's, pthread_create's, the
 * atfork list: at_quick_exit, atexit, gettext, locale, random, sem_open,
 * stdio's open files, syslog, timezone, bump; then malloc's
 * (__malloc_atfork), the thread list's (__tl_lock) and _Fork's
 * __abort_lock); then ours above. A thread that holds one of ours and
 * waits for one of musl's would meet a fork holding musl's and waiting for
 * ours, so the layer never does: proc.c's kill allocates before taking its
 * lock, and signal.c keeps an ended thread's record for the next thread
 * rather than freeing it (a thread ends holding musl's thread list lock,
 * which a fork takes after malloc's). libjam holds none of its own locks
 * across the clone and allocates nothing in the parent's fork. vfork and
 * clone skip musl's handlers and locks (as on Linux), not ours, but for
 * malloc's, which they take first, as musl's fork does: their child's calls
 * into the layer allocate (execve's first of all), where Linux's allocate
 * nothing in the process, so a child cloned while another thread held it
 * would wait for it for ever (docs/FORK-REVIEW-LIBC.md, finding 1). A raw
 * SYS_fork or _Fork takes none: it can't be told from musl's fork, which
 * holds it already.
 *
 * Not on the list, so possibly held for ever in a child if another thread
 * had one at the clone: each description's lock and those of the kinds'
 * shared state (fd.c's walk and each kind's fork hook make them fresh in
 * the child), and those of modules whose state the child drops (thread.c's,
 * sigtimer.c's, and the `forgets` below: made fresh with it).
 *
 * The descriptors and the table of mappings (section 1.6, FL2). After the
 * clone, its locks still held, the parent's give walks the descriptor table
 * (fd.c) and the table of mappings (maptable.c), and writes the handles the
 * child's copies need on the child's startup channel, after libjam's
 * message (<posixfork.h>: each under a key naming what it is for); the
 * child walks its copies the same way and takes them. A description keeps
 * everything else it had (the offset, the status flags, FD_CLOEXEC on its
 * slots) but what the parent had read off a channel; netstack sockets and
 * epoll sets are absent there (fd.h's ops->fork has each kind's rule).
 * Entries mapped as copies get vmar_clone's children (prepare tells libjam
 * which VMOs: the table is whole only under mem_lock), shared ones a
 * duplicate. The child keeps no handle it wasn't given a use for (the
 * stream's leftovers are closed), and drops, unclosed, what holds the
 * parent's (a socket's list, the per-thread ports of poll and the socket
 * waits, the pseudo-terminals' table). libjam's per-connection buffers
 * (a file's transfer buffer, a socket's ring) are mapped VMAR_CLONE_NONE,
 * so a fork never copies them nor write-protects a server's pages. */
#include <errno.h>
#include <forkmsg.h>
#include <posixfork.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "layer.h"
#include "proc.h"
#include "sig.h"

/* What the clone flags Linux's raw call may carry here: the exit signal
 * (the low byte: always SIGCHLD here) and the tid words. */
#define CLONE_PROC_OK (0xff | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)

#define PASSABLE (RIGHT_DUPLICATE | RIGHT_TRANSFER)   /* a handle a child may be given */

/* How the child goes on (the hooks' context: on the forking thread's
 * stack, so at the same address in the child's copy). */
struct how {
    int *set_tid;     /* CLONE_CHILD_SETTID: the child's tid stored here in the child */
    int *clear_tid;   /* CLONE_CHILD_CLEARTID: the child's tid word */
    struct jam_fork_args *args;   /* fork_core's: prepare names the VMOs vmar_clone copies */
    bool no_room;     /* prepare couldn't list them: the fork fails (ENOMEM) */
    bool by_musl;     /* SYS_fork: musl's fork holds malloc's lock across it already */
};

/* musl's (its malloc's glue, hidden there; the layer is in the same
 * libc.a): who < 0 takes malloc's lock, 0 gives it back, > 0 makes it
 * fresh in a child. */
void __malloc_atfork(int who);

/* The modules whose locks fork holds across the clone, in the order taken
 * (the comment above). */
static void (*const steps[])(int) = {
    proc_fork, path_fork, umask_fork, fd_fork, mem_fork, rlimit_fork, sig_fork,
};
#define NSTEPS (sizeof(steps) / sizeof(steps[0]))

/* sigtimer.c's: weak, since a program that never sets a timer links none;
 * the same for the modules whose state a child drops (layer.h). */
extern __typeof__(sig_timer_forked) sig_timer_forked __attribute__((weak));
extern __typeof__(sock_forked) sock_forked __attribute__((weak));
extern __typeof__(epoll_forked) epoll_forked __attribute__((weak));
extern __typeof__(poll_forked) poll_forked __attribute__((weak));
extern __typeof__(sockwait_forked) sockwait_forked __attribute__((weak));
extern __typeof__(pty_forked) pty_forked __attribute__((weak));
extern __typeof__(procfs_forked) procfs_forked __attribute__((weak));
extern __typeof__(enosys_forked) enosys_forked __attribute__((weak));

static void (*const forgets[])(void) = {
    sock_forked, epoll_forked, poll_forked, sockwait_forked, pty_forked, procfs_forked,
    enosys_forked,
};
#define NFORGETS (sizeof(forgets) / sizeof(forgets[0]))

/* Forks made: the last one's number (layer.h's lx_fork_gen). Changed with
 * every lock held, before the clone, so the child's copy has it too. */
static uint32_t gens;

/* ---- the stream of handles (<posixfork.h>) ----------------------------------------- */

struct lx_fork_io {
    bool     child;
    uint32_t gen;
    handle_t ch;                  /* the child's startup channel: our end, or the child's */
    status_t st;                  /* the parent: its first failure (nothing more is sent) */
    uint32_t at;                  /* the child: the next of m's handles to look at */
    uint32_t prev;                /* the child: the last key read */
    bool     last;                /* the child: the last message has been read */
    struct posixfork_msg m;       /* the message being written, or the one read */
    handle_t hs[POSIXFORK_MAX];
};

bool lx_fork_child(const struct lx_fork_io *io)
{
    return io->child;
}

uint32_t lx_fork_gen(const struct lx_fork_io *io)
{
    return io->gen;
}

/* The parent: the message so far sent (its handles moved whatever the
 * status), marked the last if last. */
static void flush(struct lx_fork_io *io, bool last)
{
    io->m.flags = last ? POSIXFORK_LAST : 0;
    if (io->st == OK)
        io->st = jam_channel_write(io->ch, &io->m, sizeof(io->m), io->hs, io->m.nhandles);
    else
        for (uint32_t i = 0; i < io->m.nhandles; i++)
            jam_handle_close(io->hs[i]);
    io->m.nhandles = 0;
}

void lx_fork_put(struct lx_fork_io *io, uint32_t key, handle_t h)
{
    if (!h)
        return;
    if (io->st != OK) {
        jam_handle_close(h);
        return;
    }
    io->m.keys[io->m.nhandles] = key;
    io->hs[io->m.nhandles++] = h;
    if (io->m.nhandles == POSIXFORK_MAX)
        flush(io, false);
}

void lx_fork_dup(struct lx_fork_io *io, uint32_t key, handle_t h)
{
    struct handle_info hi;
    handle_t d;
    if (h && io->st == OK && jam_handle_info(h, &hi) == OK && (hi.rights & PASSABLE) == PASSABLE &&
        jam_handle_duplicate(h, RIGHT_SAME, &d) == OK)
        lx_fork_put(io, key, d);
}

/* The child: the next message, checked (keys growing from the last one);
 * a malformed one ends the child, as libjam's does (<forkmsg.h>). */
static void next_message(struct lx_fork_io *io)
{
    uint32_t nbytes = 0, nh = 0;
    struct channel_read_args r = {
        .h = io->ch, .rd_cap = sizeof(io->m), .rd_bytes = (uint64_t)(uintptr_t)&io->m,
        .rd_actual = (uint64_t)(uintptr_t)&nbytes, .rd_handles = (uint64_t)(uintptr_t)io->hs,
        .rd_hcap = POSIXFORK_MAX, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    io->at = 0;
    status_t st = jam_channel_read(&r);
    if (st != OK)
        nh = 0;
    bool ok = st == OK && nbytes == sizeof(io->m) && !io->m.txid &&
              io->m.magic == POSIXFORK_MAGIC && io->m.version == POSIXFORK_VERSION &&
              io->m.nhandles == nh && !(io->m.flags & ~POSIXFORK_LAST);
    for (uint32_t i = 0; ok && i < nh; i++) {
        ok = io->m.keys[i] > io->prev;
        io->prev = io->m.keys[i];
    }
    if (ok) {
        io->last = io->m.flags & POSIXFORK_LAST;
        return;
    }
    for (uint32_t i = 0; i < nh; i++)
        jam_handle_close(io->hs[i]);
    static const char line[] = "posix: a forked child given no usable descriptors message\n";
    jam_debug_write(line, sizeof(line) - 1);
    jam_process_exit(FORK_BAD_MESSAGE);
}

handle_t lx_fork_take(struct lx_fork_io *io, uint32_t key)
{
    for (;;) {
        while (io->at < io->m.nhandles && io->m.keys[io->at] < key)
            jam_handle_close(io->hs[io->at++]);   /* nothing here asked for it */
        if (io->at < io->m.nhandles)
            return io->m.keys[io->at] == key ? io->hs[io->at++] : HANDLE_INVALID;
        if (io->last)
            return HANDLE_INVALID;
        next_message(io);
    }
}

/* ---- libjam's hooks ------------------------------------------------------------------ */

static void prepare(void *ctx)
{
    struct how *how = ctx;
    if (!how->by_musl)
        __malloc_atfork(-1);   /* first, as in musl's fork: vfork and clone */
    for (unsigned i = 0; i < NSTEPS; i++)
        steps[i](LX_FORK_TAKE);
    gens++;
    const handle_t *vmos = NULL;
    handle_t *children = NULL;
    uint32_t n = 0;
    how->no_room = !mem_fork_clones(&vmos, &children, &n);
    how->args->vmos = vmos;   /* libjam reads them after this hook (<os.h>) */
    how->args->children = children;
    how->args->n = n;
}

static status_t give(void *ctx, handle_t to, handle_t proc)
{
    const struct how *how = ctx;
    (void)proc;
    struct lx_fork_io io = { .gen = gens, .ch = to, .st = how->no_room ? ERR_NO_MEMORY : OK,
                             .m = { .magic = POSIXFORK_MAGIC, .version = POSIXFORK_VERSION } };
    fd_fork_walk(&io);
    mem_fork_walk(&io);   /* every child of a copied VMO moved or closed, whatever happened */
    flush(&io, true);
    return io.st == ERR_SHOULD_WAIT ? ERR_NO_RESOURCES : io.st;   /* the channel's queue full */
}

static void parent(void *ctx, status_t st)
{
    const struct how *how = ctx;
    (void)st;
    for (unsigned i = NSTEPS; i-- > 0;)
        steps[i](LX_FORK_GIVE);
    if (!how->by_musl)
        __malloc_atfork(0);
}

static void child(void *ctx, handle_t from)
{
    const struct how *how = ctx;
    if (!how->by_musl)
        __malloc_atfork(1);
    lx_start_forked();
    uint32_t tid = 0;
    (void)kp_thread_tid(startup_handle(SR_SELF_THREAD), &tid);
    thread_forked(tid);
    for (unsigned i = 0; i < NSTEPS; i++)
        steps[i](LX_FORK_CHILD);
    for (unsigned i = 0; i < NFORGETS; i++)
        if (forgets[i])
            forgets[i]();
    struct lx_fork_io io = { .child = true, .gen = gens, .ch = from };
    fd_fork_walk(&io);
    mem_fork_walk(&io);
    (void)lx_fork_take(&io, UINT32_MAX);   /* the rest read, and closed */
    if (sig_timer_forked)
        sig_timer_forked();
    if (how->clear_tid)
        (void)kp_set_tid_word((uint32_t *)how->clear_tid);
    if (how->set_tid)
        *how->set_tid = (int)tid;
    sig_thread_forked();
    sig_tty_attached();   /* the terminal is the child's own now: listened to */
}

static const struct jam_fork_libc hooks = {
    .prepare = prepare, .give = give, .parent = parent, .child = child,
};

static uint64_t thread_pointer(void)
{
    uint64_t tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));   /* it points at itself: the FS base */
    return tp;
}

/* RLIMIT_NPROC, as Linux's fork: EAGAIN while the user's processes are at
 * the soft limit or over it. Those counted are the ones this process
 * knows of: itself and its children not yet reaped (one user, M13 Q7; no
 * process here may pass the limit, none being privileged). */
static bool at_nproc_limit(void)
{
    uint64_t lim = rlimit_cur(RLIMIT_NPROC);
    return lim != RLIM_INFINITY && 1 + (uint64_t)lx_child_pids(NULL, 0) >= lim;
}

/* fork's core: the child's pid in the parent, 0 in the child, or -errno. */
static long fork_core(struct how *how)
{
    if (at_nproc_limit())
        return -EAGAIN;
    jam_fork_set_libc(&hooks);   /* the same each time */
    handle_t sig = HANDLE_INVALID;
    struct jam_fork_args a = { .fs_base = thread_pointer(), .signals = &sig, .ctx = how };
    how->args = &a;   /* prepare fills in the VMOs */
    int64_t r = jam_fork(&a);
    if (r == 0)
        return 0;
    if (r < 0)
        return r == ERR_NO_MEMORY ? -ENOMEM : r == ERR_NO_RESOURCES ? -EAGAIN : lx_err((status_t)r);
    handle_t proc = (handle_t)r;
    struct process_info info;
    status_t st = jam_process_get_info(proc, &info);
    if (st != OK) {
        jam_process_kill(proc);
        jam_handle_close(proc);
        jam_handle_close(sig);
        return lx_err(st);
    }
    long e = lx_child_add(proc, sig, HANDLE_INVALID, (int32_t)info.pid, lx_pgid(), lx_sid());
    return e ? e : (long)info.pid;
}

long lx_fork(long a, long b, long c, long d, long e, long f)
{
    struct how how = { .by_musl = true };
    return fork_core(&how);
}

long lx_vfork(long a, long b, long c, long d, long e, long f)
{
    struct how how = { 0 };
    long r = fork_core(&how);
    if (r == 0)
        lx_vforked();   /* our exec tells the parent (proc.c) */
    else if (r > 0)
        lx_vfork_wait((int32_t)r);   /* until the child's exec or end (section 3) */
    return r;
}

/* A clone flag set this layer can't make: logged (the ENOSYS log). */
static long refused(long flags, long err)
{
    lx_enosys_option("clone flags", flags);
    return err;
}

long lx_clone(long flags, long stack, long ptid, long ctid, long tls, long f)
{
    if (flags & CLONE_VM)   /* a thread comes through __clone, never this way */
        return refused(flags, -ENOSYS);
    if ((flags & ~(long)CLONE_PROC_OK) || stack)
        return refused(flags, -EINVAL);
    int *word = (int *)(uintptr_t)ctid;
    struct how how = { .set_tid = (flags & CLONE_CHILD_SETTID) ? word : NULL,
                       .clear_tid = (flags & CLONE_CHILD_CLEARTID) ? word : NULL };
    long r = fork_core(&how);
    if (r > 0 && (flags & CLONE_PARENT_SETTID))
        *(int *)(uintptr_t)ptid = (int)r;
    return r;
}

/* ---- clone()'s child and its stack for calls (calls.h) ------------------------------- */

/* fn runs on the stack it was given, as on Linux, and that stack may be
 * tiny: gtest's death tests give two pages (gtest-death-test.cc), and the
 * child's first calls (chdir, execve) take a path. The layer runs a call
 * on its caller's stack, and its frames are not Linux's few bytes: one
 * path's are some 4 KiB each, the deepest call made only of direct calls
 * ~29 KiB (lx_mkdir, by GCC's -fcallgraph-info, 2026-10-17; most path
 * calls 16 to 25 KiB), and the handlers a call's boundary runs come on top.
 * So in the child each call fn makes runs on a stack the layer maps there,
 * CALL_STACK bytes over a guard page (about twice that deepest call),
 * as Linux's kernel runs a call on a stack of its own: fn's frames, and
 * what a handler raised in fn's own code does outside a call, stay where
 * Linux has them. The thread is told by a thread-local top, which every
 * thread the child makes later has 0 for. */
#define CALL_STACK  (64u << 10)
#define CALL_GUARD  4096u

bool lx_call_stack_set;              /* calls.h: the test each call makes */
static uintptr_t call_stack_lo;      /* the stack's lowest usable byte */
static __thread uintptr_t call_top;  /* its top, on the thread it is for */

void __jam_sig_stack_call(void (*fn)(void *), void *arg, void *top);   /* sigentry.S */

/* fn(arg) on the stack for calls when the calling thread has it and is
 * off it, and isn't on its sigaltstack either (a handler running there
 * may have come from a call whose frames are still on it: it runs in
 * place, as on any thread); else here. The signal thread has no thread
 * pointer to read ours by: it is told by its stack. */
static void on_call_stack(void (*fn)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0), base;
    size_t size;
    if (!lx_call_stack_set || sig_on_signal_thread() || !call_top ||
        (sp >= call_stack_lo && sp <= call_top) || sig_alt_state(sp, &base, &size) == SS_ONSTACK)
        fn(arg);
    else
        __jam_sig_stack_call(fn, arg, (void *)call_top);
}

/* The child's stack for calls, mapped and set (the child of a clone made
 * in a clone child has its parent's, at the same place). None if there's
 * no memory for it: fn's calls run on its own stack then. */
static void call_stack_map(void)
{
    if (call_top)
        return;
    long b = lx_mmap(0, CALL_STACK + CALL_GUARD, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b < 0)
        return;
    (void)lx_mprotect(b, CALL_GUARD, PROT_NONE, 0, 0, 0);
    call_stack_lo = (uintptr_t)b + CALL_GUARD;
    call_top = call_stack_lo + CALL_STACK;
    lx_call_stack_set = true;
}

struct moved {
    const struct sig_frame *f;
    long r;
};

static void moved_call(void *p)
{
    struct moved *m = p;
    m->r = sig_call(m->f);
}

long lx_call_on_stack(const struct sig_frame *f)
{
    struct moved m = { f, 0 };
    on_call_stack(moved_call, &m);
    return m.r;
}

static void exit_with(void *code)
{
    lx_exit_group(*(int *)code & 0xff, 0, 0, 0, 0, 0);
}

/* clone()'s child ends: the process with fn's value (musl's own __clone
 * makes SYS_exit, the last thread's end), off fn's stack too. */
static _Noreturn void clone_end(int code)
{
    on_call_stack(exit_with, &code);
    __builtin_unreachable();
}

/* fn(arg) on stack (its top, 16-aligned), then clone_end with its value. */
static _Noreturn void run_on(void *stack, int (*fn)(void *), void *arg)
{
    uintptr_t top = (uintptr_t)stack & ~(uintptr_t)15;
    __asm__ volatile("mov %0, %%rsp\n\t"
                     "xor %%ebp, %%ebp\n\t"
                     "call *%1\n\t"       /* fn(arg): rdi is arg */
                     "mov %%eax, %%edi\n\t"
                     "call *%3\n\t"       /* clone_end: rbx kept across fn */
                     "ud2"
                     :
                     : "r"(top), "a"(fn), "D"(arg), "b"(clone_end)
                     : "memory");
    __builtin_unreachable();
}

struct clone_call {
    struct how how;
    long r;
};

static void clone_core(void *p)
{
    struct clone_call *c = p;
    struct sig_thread *q = sig_quiet_begin();   /* the layer's own: no handler runs inside */
    c->r = fork_core(&c->how);
    sig_quiet_end(q);   /* the child too: its record is the copy */
}

int __jam_clone_process(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid,
                        void *tls, int *ctid)
{
    (void)tls;   /* musl's clone() refuses CLONE_SETTLS without CLONE_VM */
    if (flags & CLONE_VM)
        return (int)refused(flags, -ENOSYS);
    if ((flags & ~(CLONE_PROC_OK & ~CLONE_CHILD_CLEARTID)) || !stack)
        return (int)refused(flags, -EINVAL);
    struct clone_call c = { .how = { .set_tid = (flags & CLONE_CHILD_SETTID) ? ctid : NULL } };
    on_call_stack(clone_core, &c);   /* a clone child's own clone: off fn's stack */
    if (c.r > 0 && (flags & CLONE_PARENT_SETTID))
        *ptid = (int)c.r;
    if (c.r)
        return (int)c.r;
    call_stack_map();
    run_on(stack, fn, arg);
}
