/* Signals' state and delivery (sig.h has the model): the dispositions,
 * each thread's mask and pending set, the process's own set, routing a
 * process's signal to a thread, and the call boundary that runs handlers.
 *
 * Each thread's record (struct sig_thread) is allocated by the thread at
 * its start and listed (sig_lock) from sig_thread_begin to
 * sig_thread_end, so a sender can route to it and interrupt it. The
 * thread finds its own through a thread-local pointer, and when that is
 * gone (musl's SIGEV_THREAD timers reset a thread's thread-local storage
 * between notifications) by its thread pointer, in the list. A record is written by its
 * own thread and, under sig_lock, by senders (pending, routed); the call
 * boundary reads `pending`, the process's set and `blocked` without the
 * lock, as atomics, and takes the lock only when something is due.
 *
 * Interrupting: a sender interrupts a thread while it is inside a call
 * (`calls` > 0), and raises it (thread_raise, docs/SIGNALS-PLAN.md section
 * 2) while it is in its own code: its exception entry then runs the
 * handler where it was, with those registers (sigfault.c). The two sides
 * are a Dekker pair, each with sequential consistency: the sender sets the
 * pending bit then reads `calls`; the thread raises `calls` then reads the
 * pending sets. So either the sender sees the call and interrupts it, or
 * the thread sees the signal at its next call, or its entry sees it first;
 * an interrupt left latched then ends the thread's next wait at once,
 * which the boundary takes for what it is (no handler ran: the call
 * starts again), and a raise that finds nothing due resumes at once.
 *
 * A raise runs no handler where one must not run: inside a call (the
 * boundary delivers; its handler's ucontext gets the registers the raise
 * found, `kept`), inside a quiet call (`quiet`: musl's malloc and unlock
 * paths) or between take() and give() (`taking`): those two raise the
 * thread again on their way out (`raise_again`).
 *
 * Standard signals (1 to 31) pending twice are one; real-time ones (32 to
 * 64) queue: the first in its set's info slot, the rest on one queue of
 * RTQ_MAX for the whole process, in order.
 *
 * A fault (sigfault.c) is no pending signal: the fault entry asks
 * sig_fault_begin on the faulting thread, which reads the record by the
 * thread-local pointer alone and takes sig_lock unless the thread holds
 * it itself (`holder`, marked by every take), and sig_action sets or
 * removes the entry as the fault signals gain or lose handlers. */
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include "proc.h"
#include "sig.h"

#define RTQ_MAX       1024   /* real-time signals queued behind the first, the process's */
#define DELIVER_MAX   256    /* handlers one boundary runs before going on */
#define SPURIOUS_MAX  1000   /* EINTRs in a row with no handler before giving up */
#define SIGRT_FIRST   32     /* the first signal that queues */
#define ONE_USER      1000   /* the one user's uid (M13 Q7) */

/* A disposition (rt_sigaction's). */
struct sig_act {
    uintptr_t handler;   /* SIG_DFL, SIG_IGN or a function */
    uint64_t  flags;     /* SA_* */
    uint64_t  mask;      /* blocked while the handler runs */
};

struct sig_thread {
    struct sig_thread *next;     /* the list; sig_lock */
    handle_t  handle;            /* its thread (RIGHT_SIGNAL), open while listed */
    uint32_t  tid;
    uintptr_t tp;                /* its thread pointer (%fs:0): finds it again */
    uint64_t  blocked;           /* its mask: written by itself under sig_lock, read atomically */
    uint64_t  waiting;           /* sigtimedwait's set while it waits; sig_lock */
    uint64_t  pending;           /* its signals: written under sig_lock, read atomically */
    uint64_t  routed;            /* those of pending sent to the process; sig_lock */
    uint32_t  calls;             /* calls it is inside (atomic; its own) */
    uint32_t  quiet;             /* quiet calls it is inside (atomic; its own) */
    bool      kept_ok;           /* `kept` is for the boundary's next handler (its own) */
    uint64_t  kept[SIG_REGS];    /* the registers a raise inside a call found (its own) */
    uint32_t  handled;           /* handlers run on it (its own) */
    uint32_t  norestart;         /* of them, without SA_RESTART (its own) */
    volatile int *cancel;        /* the cancellation point's flag it is in, or NULL (its own) */
    struct sig_info info[SIG_MAX];   /* pending's details, by signal; sig_lock */
};

/* A real-time signal queued behind the first of its kind in a set. */
struct rtq_entry {
    struct sig_thread *owner;    /* its thread, or NULL: the process's set */
    uint64_t           seq;      /* the order they came in */
    struct sig_info    info;
};

static lock_t            sig_lock;
static struct sig_act    acts[SIG_MAX + 1];      /* by signal; sig_lock */
static struct sig_thread *threads;               /* the main thread first; sig_lock */
static uint64_t          shared;                 /* the process's set: as `pending` */
static struct sig_info   shared_info[SIG_MAX];   /* sig_lock */
static struct rtq_entry  rtq[RTQ_MAX];           /* [0, nrtq) in use, any order; sig_lock */
static unsigned          nrtq;
static uint64_t          rtq_seq;                /* sig_lock */
static bool              ready;                  /* the main thread is listed (atomic) */

static __thread struct sig_thread *mine;   /* the calling thread's record, or NULL */
static struct sig_thread nobody;           /* stands in for a thread with none: never listed */

static uintptr_t thread_pointer(void)
{
    uintptr_t tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

/* sig_lock's holder (sig.h): a listed thread's thread pointer, or
 * HOLDER_OTHER for a thread that has none to read (the signal thread, the
 * main thread before it is listed); 0 while it is free. Only the fault
 * entry reads it, to tell whether the faulting thread itself holds it. */
#define HOLDER_OTHER 1
static uintptr_t holder;

/* A thread with a thread pointer is in take() .. give() from before it
 * takes the lock until after it gives it (`holder` is only set once it
 * holds it): its raise entry keeps out. A raise that found it there, or
 * in a quiet call, has it raised again on its way out. Its own, read by
 * its own entry: volatile is all the order needed. */
static __thread volatile bool taking, raise_again;

static uintptr_t holder_id(void)
{
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || sig_on_signal_thread())
        return HOLDER_OTHER;
    return thread_pointer();
}

static void raise_self(void);

static void take(void)
{
    uintptr_t id = holder_id();
    if (id != HOLDER_OTHER)
        taking = true;
    lock_take(&sig_lock);
    __atomic_store_n(&holder, id, __ATOMIC_RELAXED);
}

static void give(void)
{
    uintptr_t id = __atomic_load_n(&holder, __ATOMIC_RELAXED);
    __atomic_store_n(&holder, 0, __ATOMIC_RELAXED);
    lock_give(&sig_lock);
    if (id == HOLDER_OTHER)
        return;
    taking = false;
    if (raise_again)
        raise_self();
}

/* The thread-local pointer, as it is. Only ever called once the caller
 * is known not to be the signal thread, which has no thread-local
 * storage: a call the compiler can't move ahead of that test (reading
 * `mine` is a load through the thread pointer, which it may otherwise
 * make early). */
__attribute__((noinline)) static struct sig_thread *mine_now(void)
{
    return mine;
}

/* The calling thread's record (not the signal thread's; sig_lock not
 * held), found again by its thread pointer if the pointer was reset;
 * NULL for a thread not listed. */
__attribute__((noinline)) static struct sig_thread *me_ptr(void)
{
    if (mine)
        return mine;
    uintptr_t tp = thread_pointer();
    take();
    struct sig_thread *t = threads;
    while (t && t->tp != tp)
        t = t->next;
    give();
    mine = t;
    return t;
}

/* The calling thread's record, or the stand-in. */
static struct sig_thread *me_or_nobody(void)
{
    struct sig_thread *t = me_ptr();
    return t ? t : &nobody;
}

/* ---- dispositions ------------------------------------------------------------------- */

/* The default action of sig is to ignore it (no stops in M13: the stop
 * signals are ignored, and so is SIGCONT). */
static bool default_ignores(int sig)
{
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return true;
    }
    return false;
}

/* sig's disposition ignores it. sig_lock. */
static bool ignored_locked(int sig)
{
    uintptr_t h = acts[sig].handler;
    return h == (uintptr_t)SIG_IGN || (h == (uintptr_t)SIG_DFL && default_ignores(sig));
}

/* sig's disposition ends the process. sig_lock. */
static bool fatal_locked(int sig)
{
    return acts[sig].handler == (uintptr_t)SIG_DFL && !default_ignores(sig);
}

_Noreturn static void die(int sig)
{
    jam_process_exit(LX_EXIT_SIGNAL + sig);
}

/* ---- sets --------------------------------------------------------------------------- */

static uint64_t *set_of(struct sig_thread *t)
{
    return t ? &t->pending : &shared;
}

static struct sig_info *info_of(struct sig_thread *t)
{
    return t ? t->info : shared_info;
}

/* Add si to t's set (NULL: the process's). False if a real-time signal
 * found the queue full. sig_lock. */
static bool add_locked(struct sig_thread *t, const struct sig_info *si)
{
    uint64_t bit = SIG_BIT(si->signo), *set = set_of(t);
    if (!(*set & bit)) {
        info_of(t)[si->signo - 1] = *si;
        __atomic_or_fetch(set, bit, __ATOMIC_SEQ_CST);
        return true;
    }
    if (si->signo < SIGRT_FIRST)
        return true;   /* a standard signal pending twice is one */
    if (nrtq == RTQ_MAX)
        return false;
    rtq[nrtq++] = (struct rtq_entry){ t, rtq_seq++, *si };
    return true;
}

/* The oldest queued entry of t's set for sig, or -1. sig_lock. */
static int rtq_find_locked(struct sig_thread *t, int sig)
{
    int best = -1;
    for (unsigned i = 0; i < nrtq; i++)
        if (rtq[i].owner == t && rtq[i].info.signo == sig &&
            (best < 0 || rtq[i].seq < rtq[best].seq))
            best = (int)i;
    return best;
}

/* Take sig out of t's set into *out: the next queued one moves up. sig_lock. */
static void take_locked(struct sig_thread *t, int sig, struct sig_info *out)
{
    struct sig_info *slot = &info_of(t)[sig - 1];
    *out = *slot;
    int q = rtq_find_locked(t, sig);
    if (q >= 0) {
        *slot = rtq[q].info;
        rtq[q] = rtq[--nrtq];
        return;
    }
    __atomic_and_fetch(set_of(t), ~SIG_BIT(sig), __ATOMIC_SEQ_CST);
    if (t)
        t->routed &= ~SIG_BIT(sig);
}

/* Drop every pending sig, in every set (a disposition that ignores it). */
static void discard_locked(int sig)
{
    struct sig_info gone;
    for (struct sig_thread *t = threads; t; t = t->next)
        while (t->pending & SIG_BIT(sig))
            take_locked(t, sig, &gone);
    while (shared & SIG_BIT(sig))
        take_locked(NULL, sig, &gone);
}

/* ---- routing ------------------------------------------------------------------------ */

/* Interrupt t if it is inside a call (the Dekker pair above: the pending
 * bit was set with sequential consistency before this read): false if it
 * is in its own code. The sender itself looks on its way out of this call.
 * sig_lock. */
static bool interrupt_locked(struct sig_thread *t)
{
    if (!sig_on_signal_thread() && t == mine_now())
        return true;
    if (!__atomic_load_n(&t->calls, __ATOMIC_SEQ_CST))
        return false;
    (void)jam_thread_interrupt(t->handle);   /* a thread that has left is past caring */
    return true;
}

/* Make t look at its sets now: interrupted inside a call, raised in its
 * own code (the exception entry is set for raises from the first on).
 * sig_lock. */
static void kick_locked(struct sig_thread *t)
{
    if (interrupt_locked(t))
        return;
    sig_entry_need(SIG_ENTRY_RAISE, true);
    (void)jam_thread_raise(t->handle);   /* dropped if it has left */
}

/* Every thread blocks sig, counting a sigtimedwait's set as blocked (as
 * Linux's real_blocked): its default action then waits. sig_lock. */
static bool all_block_locked(int sig)
{
    for (struct sig_thread *t = threads; t; t = t->next)
        if (!((t->blocked | t->waiting) & SIG_BIT(sig)))
            return false;
    return true;
}

/* A thread to take sig: one not blocking it (the list's order, the main
 * thread first), one without it pending already if there is one. sig_lock. */
static struct sig_thread *pick_locked(int sig)
{
    struct sig_thread *first = NULL;
    for (struct sig_thread *t = threads; t; t = t->next) {
        if (t->blocked & SIG_BIT(sig))
            continue;
        if (!(t->pending & SIG_BIT(sig)))
            return t;
        if (!first)
            first = t;
    }
    return first;
}

/* sig, pending for the process, goes to a thread that can take it: moved
 * into its set, or left in the process's (with no such thread, or one that
 * has it pending already) with that thread interrupted to look. sig_lock. */
static void route_locked(int sig)
{
    for (unsigned guard = 0; guard < RTQ_MAX + 1 && (shared & SIG_BIT(sig)); guard++) {
        struct sig_thread *t = pick_locked(sig);
        if (!t)
            return;
        if (t->pending & SIG_BIT(sig)) {
            kick_locked(t);
            return;
        }
        struct sig_info si;
        take_locked(NULL, sig, &si);
        add_locked(t, &si);
        t->routed |= SIG_BIT(sig);
        kick_locked(t);
    }
}

/* What was routed to t and is in `moved` goes back to the process and on
 * to another thread (t blocks it now, or is ending). sig_lock. */
static void reroute_locked(struct sig_thread *t, uint64_t moved)
{
    moved &= t->routed;
    while (moved) {
        int sig = __builtin_ctzll(moved) + 1;
        moved &= moved - 1;
        struct sig_info si;
        take_locked(t, sig, &si);
        t->routed &= ~SIG_BIT(sig);
        if (!add_locked(NULL, &si))
            continue;   /* the queue is full: lost, as one sent then would be */
        route_locked(sig);
    }
}

/* t's mask is now m (never SIGKILL or SIGSTOP); what was routed to it and
 * is blocked now goes elsewhere. sig_lock. */
static void set_blocked_locked(struct sig_thread *t, uint64_t m)
{
    m &= ~SIG_NEVER_MASKED;
    uint64_t grew = m & ~t->blocked;
    __atomic_store_n(&t->blocked, m, __ATOMIC_SEQ_CST);
    if (grew)
        reroute_locked(t, grew);
}

/* ---- sending ------------------------------------------------------------------------ */

void sig_send_process(const struct sig_info *si)
{
    int sig = si->signo;
    if (sig == SIGKILL)
        die(SIGKILL);
    take();
    bool blocked = all_block_locked(sig);
    if (ignored_locked(sig) && !blocked) {
        give();
        return;
    }
    if (fatal_locked(sig) && !blocked)
        die(sig);   /* the lock dies with us */
    if (add_locked(NULL, si))
        route_locked(sig);
    give();
}

static struct sig_thread *find_locked(uint32_t tid)
{
    for (struct sig_thread *t = threads; t; t = t->next)
        if (t->tid == tid)
            return t;
    return NULL;
}

long sig_send_thread(uint32_t tid, const struct sig_info *si)
{
    int sig = si->signo;
    take();
    struct sig_thread *t = find_locked(tid);
    long r = t ? 0 : -ESRCH;
    if (!t || !sig) {
        give();
        return r;
    }
    if (sig == SIG_INTERNAL_CANCEL) {
        (void)interrupt_locked(t);   /* the cancel flag is set: its cancellation point looks */
    } else if (sig == SIGKILL) {
        die(SIGKILL);
    } else {
        bool blocked = (t->blocked | t->waiting) & SIG_BIT(sig);
        if (fatal_locked(sig) && !blocked)
            die(sig);
        if (!ignored_locked(sig) || blocked) {
            r = add_locked(t, si) ? 0 : -EAGAIN;
            if (!(t->blocked & SIG_BIT(sig)))
                kick_locked(t);
        }
    }
    give();
    return r;
}

bool sig_is_pending(int signo)
{
    take();
    bool p = shared & SIG_BIT(signo);
    for (struct sig_thread *t = threads; t && !p; t = t->next)
        p = t->pending & SIG_BIT(signo);
    give();
    return p;
}

/* ---- threads ------------------------------------------------------------------------ */

void sig_thread_begin(handle_t handle, uint32_t tid, uint64_t mask)
{
    struct sig_thread *t = calloc(1, sizeof(*t));
    if (!t)
        return;   /* no memory: this thread gets no signals (none is routed to it) */
    t->handle = handle;
    t->tid = tid;
    t->tp = thread_pointer();
    t->blocked = mask & ~SIG_NEVER_MASKED;
    take();
    struct sig_thread **tail = &threads;   /* at the end: the main thread stays first */
    while (*tail)
        tail = &(*tail)->next;
    *tail = t;
    give();
    mine = t;
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}

void sig_thread_end(void)
{
    struct sig_thread *t = me_ptr();
    if (!t)
        return;
    take();
    struct sig_thread **p = &threads;
    while (*p && *p != t)
        p = &(*p)->next;
    if (*p)
        *p = t->next;
    reroute_locked(t, t->routed);
    for (unsigned i = 0; i < nrtq;)   /* its own queued signals go with it */
        if (rtq[i].owner == t)
            rtq[i] = rtq[--nrtq];
        else
            i++;
    give();
    mine = NULL;
    free(t);   /* unlisted: nobody else can reach it */
    /* A raise latched before it was unlisted may still come: with no
     * exception stack its frame goes below RSP, where it can't be written
     * once musl has unmapped the stack (and the thread-local storage with
     * it), so it is dropped rather than entering with no storage. */
    uintptr_t base;
    size_t size;
    if (sig_alt_state(0, &base, &size) != SS_DISABLE)
        (void)sig_alt_set(0, 0);
}

uint64_t sig_mask_self(void)
{
    struct sig_thread *t = me_ptr();
    return t ? __atomic_load_n(&t->blocked, __ATOMIC_RELAXED) : 0;
}

/* ---- delivery ----------------------------------------------------------------------- */

/* The calling thread's record, or NULL: before the main thread is
 * listed, on the signal thread (no thread-local storage), or a thread
 * not listed. */
static struct sig_thread *self(void)
{
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || sig_on_signal_thread())
        return NULL;
    return me_ptr();
}

/* Something t doesn't block is pending, for it or the process. */
static bool due(struct sig_thread *t)
{
    uint64_t p = __atomic_load_n(&t->pending, __ATOMIC_SEQ_CST) |
                 __atomic_load_n(&shared, __ATOMIC_SEQ_CST);
    return p & ~__atomic_load_n(&t->blocked, __ATOMIC_RELAXED);
}

/* A siginfo of sig with this code carries an address, not a sender: a
 * fault's (the kernel's codes are > 0), or one sent as a fault with
 * rt_tgsigqueueinfo. Linux's layout. */
static bool has_addr(int sig, int code)
{
    return code > 0 && (SIG_BIT(sig) & SIG_FAULTS);
}

void sig_fill_siginfo(const struct sig_info *si, void *out)
{
    siginfo_t *info = out;
    memset(info, 0, sizeof(*info));
    info->si_signo = si->signo;
    info->si_code = si->code;
    if (has_addr(si->signo, si->code)) {
        info->si_addr = (void *)(uintptr_t)si->addr;
        return;
    }
    info->si_pid = si->pid;
    info->si_uid = ONE_USER;
    if (si->signo == SIGCHLD) {
        info->si_status = si->status;
        return;
    }
    if (si->code == SI_TIMER) {
        info->si_timerid = si->timer;
        info->si_overrun = si->overrun;
    }
    info->si_value.sival_ptr = (void *)(intptr_t)si->value;
}

/* Call the handler of act for si, with the mask `was` as the handler's
 * context says (the one restored after it). */
struct handler_call {
    const struct sig_act  *act;
    const struct sig_info *si;
    siginfo_t              info;   /* SA_SIGINFO only */
    ucontext_t             uc;
};

static void call_handler(void *arg)
{
    struct handler_call *c = arg;
    if (c->act->flags & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))c->act->handler)(c->si->signo, &c->info, &c->uc);
    else
        ((void (*)(int))c->act->handler)(c->si->signo);
}

/* With SA_ONSTACK on the thread's alternate stack, if it has one and isn't
 * on it (sigaltstack inside says SS_ONSTACK, as for a fault's handler).
 * The ucontext's registers are regs (REG_R8 .. REG_EFL: what a raise
 * inside the call found), else 0. */
static void run_handler(const struct sig_act *act, const struct sig_info *si, uint64_t was,
                        const uint64_t *regs)
{
    struct handler_call c = { .act = act, .si = si };
    if (act->flags & SA_SIGINFO) {
        sig_fill_siginfo(si, &c.info);
        memset(&c.uc, 0, sizeof(c.uc));
        if (regs)
            memcpy(c.uc.uc_mcontext.gregs, regs, SIG_REGS * sizeof(greg_t));
        memcpy(&c.uc.uc_sigmask, &was, sizeof(was));
        uintptr_t base;
        size_t size;
        c.uc.uc_stack.ss_flags = sig_alt_state((uintptr_t)__builtin_frame_address(0), &base, &size);
        c.uc.uc_stack.ss_sp = (void *)base;
        c.uc.uc_stack.ss_size = size;
    }
    if (act->flags & SA_ONSTACK)
        sig_run_onstack(call_handler, &c);
    else
        call_handler(&c);
}

/* Deliver one signal due on t (its own thread): 1 if one was taken (its
 * handler run, or its default done), 0 if none was due. */
static int deliver_one(struct sig_thread *t)
{
    take();
    uint64_t can = (t->pending | shared) & ~t->blocked;
    if (!can) {
        give();
        return 0;
    }
    int sig = __builtin_ctzll(can) + 1;
    struct sig_info si;
    take_locked(t->pending & SIG_BIT(sig) ? t : NULL, sig, &si);
    struct sig_act act = acts[sig];
    uint64_t was = t->blocked;
    bool handler = act.handler != (uintptr_t)SIG_DFL && act.handler != (uintptr_t)SIG_IGN;
    if (handler) {
        if (act.flags & SA_RESETHAND)
            acts[sig] = (struct sig_act){ .handler = (uintptr_t)SIG_DFL };
        set_blocked_locked(t, was | act.mask | (act.flags & SA_NODEFER ? 0 : SIG_BIT(sig)));
    }
    give();
    if (act.handler == (uintptr_t)SIG_DFL && !default_ignores(sig))
        die(sig);
    if (!handler)
        return 1;   /* ignored by the time it came to be delivered */
    t->handled++;
    if (!(act.flags & SA_RESTART))
        t->norestart++;
    uint64_t kept[SIG_REGS];
    bool regs = t->kept_ok;
    if (regs)
        memcpy(kept, t->kept, sizeof(kept));
    t->kept_ok = false;   /* one handler's */
    run_handler(&act, &si, was, regs ? kept : NULL);
    take();
    set_blocked_locked(t, was);
    give();
    return 1;
}

/* Deliver what is due on t, a bounded number of signals at a time. */
static void deliver(struct sig_thread *t)
{
    for (unsigned n = 0; n < DELIVER_MAX && deliver_one(t); n++)
        ;
}

/* Into a call: count it, then deliver what came before it. */
static void enter(struct sig_thread *t)
{
    __atomic_add_fetch(&t->calls, 1, __ATOMIC_SEQ_CST);
    for (unsigned n = 0; n < DELIVER_MAX && due(t); n++) {
        __atomic_sub_fetch(&t->calls, 1, __ATOMIC_SEQ_CST);
        deliver(t);
        __atomic_add_fetch(&t->calls, 1, __ATOMIC_SEQ_CST);
    }
}

static long call(const struct sig_frame *f)
{
    return f->fn(f->a[0], f->a[1], f->a[2], f->a[3], f->a[4], f->a[5]);
}

/* SIGPIPE for the write that got EPIPE, on its own thread. */
static void raise_pipe(struct sig_thread *t)
{
    struct sig_info si = { .signo = SIGPIPE, .code = SI_USER, .pid = lx_pid() };
    (void)sig_send_thread(t->tid, &si);   /* we are listed */
}

long sig_call(const struct sig_frame *f)
{
    struct sig_thread *t = self();
    long r;
    if (!t) {
        r = call(f);
        return r == -LX_EINTR_FINAL ? -EINTR : r;
    }
    if (f->kind & SIG_CALL_QUIET) {
        __atomic_add_fetch(&t->quiet, 1, __ATOMIC_RELAXED);
        r = call(f);
        bool out = !__atomic_sub_fetch(&t->quiet, 1, __ATOMIC_RELAXED);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* out of it before raise_again is read */
        if (out && raise_again)
            raise_self();
        return r == -LX_EINTR_FINAL ? -EINTR : r;
    }
    uint32_t handled = t->handled, norestart = t->norestart;
    volatile int *outer = t->cancel;
    t->cancel = f->cancel;
    for (unsigned spurious = 0;;) {
        enter(t);
        r = f->cancel && *f->cancel ? -EINTR : call(f);
        __atomic_sub_fetch(&t->calls, 1, __ATOMIC_SEQ_CST);
        bool final = r == -LX_EINTR_FINAL;   /* a socket's timed wait: never restarted */
        if (final)
            r = -EINTR;
        if (r == -EPIPE && (f->kind & SIG_CALL_PIPE))
            raise_pipe(t);
        if (due(t))
            deliver(t);
        if (r != -EINTR || (f->cancel && *f->cancel))
            break;
        if (t->handled == handled) {
            if (++spurious < SPURIOUS_MAX)
                continue;   /* an interrupt left over: nothing to report */
            break;
        }
        if (final || (f->kind & SIG_CALL_NORESTART) || t->norestart != norestart)
            break;
    }
    t->cancel = outer;
    t->kept_ok = false;   /* a raise's registers are this call's boundary's only */
    return r;
}

bool sig_cancel_asked(void)
{
    struct sig_thread *t = self();
    return t && t->cancel && *t->cancel;
}

/* ---- the calls' own steps (sigcalls.c) ---------------------------------------------- */

/* sig's disposition is a handler of the program's. sig_lock. */
static bool handled_locked(int sig)
{
    uintptr_t h = acts[sig].handler;
    return h != (uintptr_t)SIG_DFL && h != (uintptr_t)SIG_IGN;
}

/* A fault signal has a handler: the exception entry is needed. sig_lock. */
static bool fault_handled_locked(void)
{
    for (uint64_t m = SIG_FAULTS; m; m &= m - 1)
        if (handled_locked(__builtin_ctzll(m) + 1))
            return true;
    return false;
}

long sig_action(int sig, const struct sig_act_io *in, struct sig_act_io *out)
{
    take();
    if (out)
        *out = (struct sig_act_io){ acts[sig].handler, acts[sig].flags, acts[sig].mask };
    if (in) {
        acts[sig] = (struct sig_act){ in->handler, in->flags, in->mask & ~SIG_NEVER_MASKED };
        if (ignored_locked(sig))
            discard_locked(sig);   /* POSIX: an ignored pending signal is dropped */
        if (SIG_BIT(sig) & SIG_FAULTS)
            sig_entry_need(SIG_ENTRY_HANDLERS, fault_handled_locked());
    }
    give();
    return 0;
}

uint64_t sig_ignored_set(void)
{
    uint64_t ign = 0;
    take();
    for (int s = 1; s <= SIG_MAX; s++)
        if (acts[s].handler == (uintptr_t)SIG_IGN)
            ign |= SIG_BIT(s);
    give();
    return ign;
}

bool sig_reaps_children(void)
{
    take();
    bool reap = acts[SIGCHLD].handler == (uintptr_t)SIG_IGN || (acts[SIGCHLD].flags & SA_NOCLDWAIT);
    give();
    return reap;
}

uint64_t sig_set_mask(int how, uint64_t set, bool given)
{
    struct sig_thread *t = me_or_nobody();
    take();
    uint64_t was = t->blocked;
    if (given) {
        uint64_t m = how == SIG_BLOCK ? was | set : how == SIG_UNBLOCK ? was & ~set : set;
        set_blocked_locked(t, m);
    }
    give();
    return was;
}

uint64_t sig_pending_set(void)
{
    struct sig_thread *t = me_or_nobody();
    take();
    uint64_t p = (t->pending | shared) & t->blocked;
    give();
    return p;
}

bool sig_take_waited(uint64_t want, struct sig_info *out)
{
    struct sig_thread *t = me_or_nobody();
    take();
    uint64_t can = (t->pending | shared) & want;
    if (can) {
        int sig = __builtin_ctzll(can) + 1;
        take_locked(t->pending & SIG_BIT(sig) ? t : NULL, sig, out);
    }
    give();
    return can;
}

void sig_wait_begin(uint64_t want, uint64_t *was)
{
    struct sig_thread *t = me_or_nobody();
    take();
    *was = t->blocked;
    t->waiting = want;
    set_blocked_locked(t, t->blocked & ~want);
    for (uint64_t m = want & shared; m; m &= m - 1)   /* the process's, now ours to take */
        route_locked(__builtin_ctzll(m) + 1);
    give();
}

void sig_wait_end(uint64_t was)
{
    struct sig_thread *t = me_or_nobody();
    take();
    t->waiting = 0;
    set_blocked_locked(t, was);
    give();
}

bool sig_due_self(void)
{
    struct sig_thread *t = self();
    return t && due(t);
}

uint32_t sig_handled_self(void)
{
    return me_or_nobody()->handled;
}

void sig_deliver_self(void)
{
    struct sig_thread *t = self();
    if (t)
        deliver(t);
}

void sig_wait_mask(const uint64_t *mask, uint64_t *old)
{
    *old = sig_set_mask(SIG_SETMASK, *mask, true);
}

void sig_wait_unmask(uint64_t old, long r)
{
    if (r == -EINTR)
        sig_deliver_self();   /* under the wait's mask, as Linux's handler frame */
    (void)sig_set_mask(SIG_SETMASK, old, true);
}

void sig_spawn_masks(uint64_t *blocked, uint64_t *ignored)
{
    *blocked = sig_mask_self();
    *ignored = sig_ignored_set();
}

void sig_start_state(uint64_t ignored)
{
    take();
    for (int s = 1; s <= SIG_MAX; s++)
        if ((ignored & SIG_BIT(s)) && !(SIG_NEVER_MASKED & SIG_BIT(s)))
            acts[s].handler = (uintptr_t)SIG_IGN;
    give();
}

/* ---- faults (sigfault.c) ------------------------------------------------------------ */

bool sig_fault_begin(int sig, struct sig_act_io *act, uint64_t *was)
{
    /* In the kernel's window: nothing here may fault or wait for this
     * thread. The record by the thread-local pointer only (me_ptr's
     * search takes the lock), and the lock only if another thread has it. */
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || sig_on_signal_thread())
        return false;
    struct sig_thread *t = mine_now();
    if (!t || __atomic_load_n(&holder, __ATOMIC_RELAXED) == thread_pointer())
        return false;
    take();
    struct sig_act a = acts[sig];
    if (!handled_locked(sig) || (t->blocked & SIG_BIT(sig))) {
        give();
        return false;
    }
    if (a.flags & SA_RESETHAND)
        acts[sig] = (struct sig_act){ .handler = (uintptr_t)SIG_DFL };
    *was = t->blocked;
    set_blocked_locked(t, *was | a.mask | (a.flags & SA_NODEFER ? 0 : SIG_BIT(sig)));
    give();
    *act = (struct sig_act_io){ a.handler, a.flags, a.mask };
    return true;
}

void sig_fault_end(uint64_t mask)
{
    struct sig_thread *t = me_ptr();
    if (!t)
        return;
    take();
    set_blocked_locked(t, mask);
    give();
    if (due(t))
        deliver(t);
}

/* ---- raises (sigfault.c) ------------------------------------------------------------ */

/* The calling thread raised again: a raise found it where no handler may
 * run, and it has left there. */
static void raise_self(void)
{
    raise_again = false;
    struct sig_thread *t = mine_now();
    if (t)
        (void)jam_thread_raise(t->handle);
}

/* The next signal due on t taken for its handler, as sig_raise_begin says:
 * one ignored by now is dropped, a default that terminates ends the
 * process (the lock dies with us). sig_lock. */
static bool take_due_locked(struct sig_thread *t, struct sig_info *si, struct sig_act_io *act,
                            uint64_t *was)
{
    for (unsigned n = 0; n < DELIVER_MAX; n++) {
        uint64_t can = (t->pending | shared) & ~t->blocked;
        if (!can)
            return false;
        int sig = __builtin_ctzll(can) + 1;
        take_locked(t->pending & SIG_BIT(sig) ? t : NULL, sig, si);
        if (fatal_locked(sig))
            die(sig);
        if (!handled_locked(sig))
            continue;
        struct sig_act a = acts[sig];
        if (a.flags & SA_RESETHAND)
            acts[sig] = (struct sig_act){ .handler = (uintptr_t)SIG_DFL };
        *was = t->blocked;
        set_blocked_locked(t, *was | a.mask | (a.flags & SA_NODEFER ? 0 : SIG_BIT(sig)));
        *act = (struct sig_act_io){ a.handler, a.flags, a.mask };
        return true;
    }
    return false;
}

bool sig_raise_begin(const uint64_t *regs, struct sig_info *si, struct sig_act_io *act,
                     uint64_t *was)
{
    /* In the kernel's window, as sig_fault_begin: the record by the
     * thread-local pointer only, and the lock only if this thread is
     * nowhere near it. */
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || sig_on_signal_thread())
        return false;
    struct sig_thread *t = mine_now();
    if (!t || !due(t))
        return false;   /* delivered already, or blocked since */
    if (__atomic_load_n(&t->calls, __ATOMIC_RELAXED)) {
        memcpy(t->kept, regs, sizeof(t->kept));
        t->kept_ok = true;
        return false;
    }
    if (__atomic_load_n(&t->quiet, __ATOMIC_RELAXED) || taking) {
        raise_again = true;
        return false;
    }
    take();
    bool run = take_due_locked(t, si, act, was);
    give();
    return run;
}

bool sig_raise_next(uint64_t mask, struct sig_info *si, struct sig_act_io *act, uint64_t *was)
{
    struct sig_thread *t = mine_now();
    if (!t)
        return false;
    take();
    set_blocked_locked(t, mask);
    bool run = take_due_locked(t, si, act, was);
    give();
    return run;
}
