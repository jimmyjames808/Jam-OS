/* fork (docs/FORK-PLAN.md section 1, track FL1b): POSIX's fork, vfork and
 * clone without CLONE_VM, on libjam's jam_fork (<os.h>, user/lib/fork.c),
 * which copies our address space at one instant into a new process in our
 * job and starts its one thread where the call was made.
 *
 * The ways in (section 1.11): musl's fork and _Fork (unmodified) make
 * SYS_fork; musl's vfork (its override) makes SYS_vfork, which is fork's
 * core here too: the parent's wait for the child's exec or exit is FL4's
 * (Q3 (A)), so until then the parent runs on at once, as after fork; a raw
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
 * set again by signal.c's step if a fault signal has a handler.
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
 * across the clone and allocates nothing in the parent's fork. A raw clone
 * or vfork skips musl's handlers and locks (as on Linux), not ours.
 *
 * Not on the list, so possibly held for ever in a child if another thread
 * had one at the clone: each descriptor kind's own locks (FL2's: each
 * kind's fork step makes them the child's), and those of modules whose
 * state the child drops (thread.c's, sigtimer.c's: made fresh with it).
 *
 * What a child can't use until FL2 (section 1.6): its descriptors (each
 * description's handles are the parent's: a call on one fails, EBADF or
 * the like), handles in libc's table of mappings (munmap still unmaps, but
 * madvise's decommit, a MAP_FIXED over libc's own private memory, an
 * mprotect adding write to a file's pages and the break's lowering fail),
 * the cached service channels (/svc/dns, /svc/unix, /svc/exec), sockets
 * and epoll sets. Memory, the namespace (libjam's, passed), ids, waits,
 * signals, threads, malloc and _exit work. */
#include <errno.h>
#include <sched.h>
#include "layer.h"
#include "proc.h"
#include "sig.h"

/* What the clone flags Linux's raw call may carry here: the exit signal
 * (the low byte: always SIGCHLD here) and the tid words. */
#define CLONE_PROC_OK (0xff | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)

/* How the child goes on (the hooks' context: on the forking thread's
 * stack, so at the same address in the child's copy). */
struct how {
    int *set_tid;     /* CLONE_CHILD_SETTID: the child's tid stored here in the child */
    int *clear_tid;   /* CLONE_CHILD_CLEARTID: the child's tid word */
};

/* The modules whose locks fork holds across the clone, in the order taken
 * (the comment above). */
static void (*const steps[])(int) = {
    proc_fork, path_fork, umask_fork, fd_fork, mem_fork, rlimit_fork, sig_fork,
};
#define NSTEPS (sizeof(steps) / sizeof(steps[0]))

/* sigtimer.c's: weak, since a program that never sets a timer links none. */
extern __typeof__(sig_timer_forked) sig_timer_forked __attribute__((weak));

static void prepare(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < NSTEPS; i++)
        steps[i](LX_FORK_TAKE);
}

static void parent(void *ctx, status_t st)
{
    (void)ctx;
    (void)st;
    for (unsigned i = NSTEPS; i-- > 0;)
        steps[i](LX_FORK_GIVE);
}

static void child(void *ctx, handle_t from)
{
    const struct how *how = ctx;
    (void)from;   /* FL2: the descriptors' messages come on it */
    lx_start_forked();
    uint32_t tid = 0;
    (void)kp_thread_tid(startup_handle(SR_SELF_THREAD), &tid);
    thread_forked(tid);
    for (unsigned i = 0; i < NSTEPS; i++)
        steps[i](LX_FORK_CHILD);
    if (sig_timer_forked)
        sig_timer_forked();
    if (how->clear_tid)
        (void)kp_set_tid_word((uint32_t *)how->clear_tid);
    if (how->set_tid)
        *how->set_tid = (int)tid;
    sig_thread_forked();
}

/* give: FL2's (the descriptors and the table of mappings' handles). */
static const struct jam_fork_libc hooks = { .prepare = prepare, .parent = parent, .child = child };

static uint64_t thread_pointer(void)
{
    uint64_t tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));   /* it points at itself: the FS base */
    return tp;
}

/* fork's core: the child's pid in the parent, 0 in the child, or -errno. */
static long fork_core(struct how *how)
{
    jam_fork_set_libc(&hooks);   /* the same each time */
    handle_t sig = HANDLE_INVALID;
    struct jam_fork_args a = { .fs_base = thread_pointer(), .signals = &sig, .ctx = how };
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
    struct how how = { 0 };
    return fork_core(&how);
}

long lx_vfork(long a, long b, long c, long d, long e, long f)
{
    struct how how = { 0 };
    /* FL4: the parent waits here for the child's exec or exit (section 3). */
    return fork_core(&how);
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
    struct how how = { (flags & CLONE_CHILD_SETTID) ? (int *)(uintptr_t)ctid : NULL,
                       (flags & CLONE_CHILD_CLEARTID) ? (int *)(uintptr_t)ctid : NULL };
    long r = fork_core(&how);
    if (r > 0 && (flags & CLONE_PARENT_SETTID))
        *(int *)(uintptr_t)ptid = (int)r;
    return r;
}

/* clone()'s child ends: the process with fn's value (musl's own __clone
 * makes SYS_exit, the last thread's end). */
static _Noreturn void clone_end(int code)
{
    lx_exit_group(code & 0xff, 0, 0, 0, 0, 0);
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

int __jam_clone_process(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid,
                        void *tls, int *ctid)
{
    (void)tls;   /* musl's clone() refuses CLONE_SETTLS without CLONE_VM */
    if (flags & CLONE_VM)
        return (int)refused(flags, -ENOSYS);
    if ((flags & ~(CLONE_PROC_OK & ~CLONE_CHILD_CLEARTID)) || !stack)
        return (int)refused(flags, -EINVAL);
    struct how how = { (flags & CLONE_CHILD_SETTID) ? ctid : NULL, NULL };
    long r = fork_core(&how);
    if (r > 0 && (flags & CLONE_PARENT_SETTID))
        *ptid = (int)r;
    if (r)
        return (int)r;
    run_on(stack, fn, arg);
}
