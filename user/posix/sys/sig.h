/* Signals in the Linux call layer (docs/M13-PLAN.md section 6): what
 * signal.c (the dispositions, the masks, the pending sets, routing and
 * delivery at the call boundary), sigcalls.c (the signal calls),
 * sigthread.c (the signal thread: the starter's channel, the children,
 * the terminal, the timers) and sigtimer.c (alarm, setitimer, the POSIX
 * timers) share, and what the rest of the layer calls.
 *
 * The model. A signal is delivered in libc: at a call boundary, or by a
 * raise for a thread busy in its own code (below). calls.c's
 * __jam_syscall runs every call through sig_call, which runs the handlers
 * due before and after it. A thread blocked in an interruptible wait is woken for a
 * signal by thread_interrupt (K1's latched interrupt): the wait ends with
 * ERR_INTERRUPTED (EINTR), the handler runs, and the call returns EINTR
 * or starts again (SA_RESTART). The rule that loses nothing (M13-PLAN's
 * K1 notes): whoever sends marks the signal pending first and interrupts
 * second; the woken thread looks at the pending sets after its call
 * returns.
 *
 * A signal sent to the process (kill, the terminal, a child's end, a
 * timer) is routed at once to one thread that doesn't block it, the main
 * thread first, and becomes that thread's (so a thread that sleeps long
 * is never woken for a signal another thread then takes); with no such
 * thread it waits in the process's set until a thread unblocks it. A
 * default action happens at once, in whichever thread sent it (often the
 * signal thread): "terminate" is process_exit(LX_EXIT_SIGNAL + N), so
 * Ctrl+C ends a busy loop. A fault is a signal on the faulting thread
 * only while a fault signal has a handler (sigfault.c: the kernel's
 * exception entry); otherwise the kernel kills the process, as for any
 * program, and waitpid says the fault's signal (sig_fault_signal).
 * A handler with SA_ONSTACK runs on the thread's alternate stack
 * (sigaltstack, below), a fault's and one delivered at a call boundary;
 * one without it on the stack the thread was on.
 *
 * A thread busy in its own code (no call to look at its sets) is raised
 * instead of interrupted (docs/SIGNALS-PLAN.md section 2): the kernel
 * enters the exception entry on it with the registers where it was, and
 * the entry runs its due handlers there, as Linux's signal frame would
 * (sigfault.c), with that ucontext. A raise that finds the thread inside
 * the layer leaves the signal to its call's boundary.
 *
 * Locks: sig_lock (signal.c) guards the dispositions, every pending set
 * and the list of threads. It is a leaf: nothing waits, calls another
 * process or runs a handler with it held. Its holder is marked, so the
 * fault entry never waits for a lock its own thread holds (a fault in
 * signal.c is the kernel's kill). */
#pragma once

#include "layer.h"

#define SIG_MAX 64                                  /* Linux's signals: 1 to 64 */
#define SIG_BIT(n) (1ull << ((n) - 1))              /* signal n in a mask */
#define SIG_NEVER_MASKED (SIG_BIT(9) | SIG_BIT(19)) /* SIGKILL and SIGSTOP */
#define SIG_INTERNAL_CANCEL 33                      /* musl's SIGCANCEL: a wake, never pending */
#define SIG_REGS 18                                 /* REG_R8 .. REG_EFL: an exc_frame's, gregs' order */

/* What a handler's siginfo_t says of a signal (Linux's fields). */
struct sig_info {
    int32_t signo;     /* 1 to 64 */
    int32_t code;      /* SI_USER, SI_QUEUE, SI_TKILL, SI_TIMER, SI_KERNEL, CLD_* */
    int32_t pid;       /* the sender's pid (a child's for SIGCHLD); 0: the kernel's */
    int32_t status;    /* SIGCHLD: the child's exit code or signal */
    int64_t value;     /* sigqueue's value; a timer's sigev_value */
    int32_t timer;     /* SI_TIMER: the timer's id */
    int32_t overrun;   /* SI_TIMER: expirations not delivered */
    uint64_t addr;     /* a fault's si_addr: SIGSEGV, SIGBUS, SIGILL, SIGFPE and
                          SIGTRAP with si_code > 0 carry it in place of a sender */
};

/* ---- threads (signal.c, from thread.c) --------------------------------------------- */

/* This thread starts: handle (its thread handle, kept open by thread.c
 * until sig_thread_end) and tid; it blocks mask. */
void     sig_thread_begin(handle_t handle, uint32_t tid, uint64_t mask);
/* This thread ends: what was routed to it goes to another thread; its
 * own signals are dropped. Before its handle is closed. */
void     sig_thread_end(void);
/* The signals the calling thread blocks (a new thread starts with its
 * creator's, as on Linux). */
uint64_t sig_mask_self(void);

/* ---- what the rest of the layer asks ------------------------------------------------ */

/* posix_spawn's state for a child: the signals this thread blocks and
 * the ones the process ignores (SIG_IGN), as the hand-over's masks. */
void     sig_spawn_masks(uint64_t *blocked, uint64_t *ignored);
/* ppoll, pselect and epoll_pwait: block exactly *mask (SIGKILL and
 * SIGSTOP left out) for the wait, the old mask into *old. */
void     sig_wait_mask(const uint64_t *mask, uint64_t *old);
/* After that wait, which returned r: an EINTR delivers what the wait's
 * mask let through (with that mask, as Linux's handler frame does), then
 * the old mask is back. */
void     sig_wait_unmask(uint64_t old, long r);
/* The process's terminal has been set (tty.c): the signal thread starts
 * listening to it (a resize is SIGWINCH; ISIG's keys SIGINT and SIGQUIT). */
void     sig_tty_attached(void);
/* A child posix_spawn started has ended (proc.c, once per child, no lock
 * held): SIGCHLD with its pid and wait status. True if it should be
 * reaped at once (SIGCHLD ignored, or SA_NOCLDWAIT): no wait sees it. */
bool     sig_child_ended(int32_t pid, int wstatus);

/* ---- sending (signal.c) ------------------------------------------------------------- */

/* Signal the process (si->signo 1 to 64): its default action now (for a
 * default that terminates, unless every thread blocks it), or routed to
 * a thread. From any thread, the signal thread's included. */
void sig_send_process(const struct sig_info *si);
/* Signal one thread of ours (tid): 0, or -ESRCH (no such thread), -EAGAIN
 * (the queue of real-time signals is full). Signal 0 checks only;
 * SIG_INTERNAL_CANCEL only wakes it (pthread_cancel). */
long sig_send_thread(uint32_t tid, const struct sig_info *si);
/* signo is pending for the process or any thread (a timer's overrun). */
bool sig_is_pending(int signo);

/* ---- the calls' own steps (signal.c, for sigcalls.c and the start) ------------------ */

/* A disposition as rt_sigaction gives and takes it. */
struct sig_act_io {
    uintptr_t handler;   /* SIG_DFL, SIG_IGN or a function */
    uint64_t  flags;     /* SA_* */
    uint64_t  mask;      /* blocked while the handler runs */
};
/* sig's disposition into *out (if out), then in's (if in): one that
 * ignores sig drops it where it is pending. sig is 1 to 64. 0. */
long     sig_action(int sig, const struct sig_act_io *in, struct sig_act_io *out);
/* The signals the process ignores (SIG_IGN). */
uint64_t sig_ignored_set(void);
/* A child's end is reaped at once: SIGCHLD is SIG_IGN, or SA_NOCLDWAIT. */
bool     sig_reaps_children(void);
/* The calling thread's mask changed as rt_sigprocmask's how says (if
 * given); the old one. */
uint64_t sig_set_mask(int how, uint64_t set, bool given);
/* The calling thread's blocked signals pending (sigpending). */
uint64_t sig_pending_set(void);
/* Take a signal of want pending for the calling thread or the process
 * into *out, blocked or not (sigwait's): true if there was one. */
bool     sig_take_waited(uint64_t want, struct sig_info *out);
/* sigtimedwait waits for want: the calling thread takes want's signals
 * (they are unblocked, but no handler runs for them: the call takes them
 * itself) until sig_wait_end puts the mask back (was). */
void     sig_wait_begin(uint64_t want, uint64_t *was);
void     sig_wait_end(uint64_t was);
/* For a call that waits for a signal itself (pause, sigsuspend): one is
 * due on the calling thread; deliver what is due; handlers run on it so
 * far (a count). */
bool     sig_due_self(void);
void     sig_deliver_self(void);
uint32_t sig_handled_self(void);
/* The calling thread is in a cancellation point whose thread has been
 * cancelled: a wait for a signal ends (EINTR) so the cancellation acts. */
bool     sig_cancel_asked(void);
/* si as a siginfo_t at out (Linux's fields; the one user's uid). */
void     sig_fill_siginfo(const struct sig_info *si, void *out);
/* The start: the hand-over's ignored signals, and the exception entry set
 * for the checked copies (SIG_ENTRY_COPY) (sigthread.c's start). */
void     sig_start_state(uint64_t ignored);

/* ---- fork (fork.c) ------------------------------------------------------------------ */

/* signal.c's step (layer.h's LX_FORK_*): sig_lock across the clone; in
 * the child, the calling thread alone on the list (after thread.c's
 * thread_forked: its id), nothing pending, the exception entry set again
 * if a fault signal has a handler, then the thread's alternate stack
 * (sig_alt_reapply). */
void sig_fork(int step);
/* In the child, last: the signal thread started again on a port of its
 * own, listening to the child's SR_SIGNALS (the parent's client end is in
 * its table of children). sigthread.c. */
void sig_thread_forked(void);
/* In the child: no timer, alarm or interval timer (POSIX: they aren't
 * inherited); their kernel timers were the parent's. sigtimer.c. */
void sig_timer_forked(void);

/* ---- faults (sigfault.c; signal.c's side of them) ------------------------------------ */

/* The signals a fault becomes (SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV):
 * while one has a handler, the exception entry is set. */
#define SIG_FAULTS (SIG_BIT(4) | SIG_BIT(5) | SIG_BIT(7) | SIG_BIT(8) | SIG_BIT(11))

/* Why the process's exception entry is set (sig_entry_need's `why`). */
#define SIG_ENTRY_HANDLERS 1u   /* a fault signal has a handler */
#define SIG_ENTRY_COPY     2u   /* always, from the start: the checked copies' faults
                                 * (ucopy.h) */
#define SIG_ENTRY_RAISE    4u   /* one of our threads has been raised: set from then on */

/* The entry is needed for `why` (on) or no longer (off): it is set while
 * any reason holds and removed when none does. sig_lock held. */
void sig_entry_need(unsigned why, bool on);

/* The fault entry (sigfault.c), on the faulting thread, in the kernel's
 * window: is the fault's signal sig taken by a handler? True with *act its
 * disposition as it was and *was the thread's mask: the action is reset
 * first if SA_RESETHAND, and the thread now blocks act's mask and sig
 * (unless SA_NODEFER). False, nothing changed, for the kernel's kill: sig
 * blocked, ignored or default (Linux forces the default on a fault), a
 * thread libc doesn't list (the signal thread, one whose record it can't
 * find without the lock), or one that holds sig_lock itself. */
bool sig_fault_begin(int sig, struct sig_act_io *act, uint64_t *was);
/* After the handler returned: the thread's mask is `mask` (its ucontext's
 * uc_sigmask), and what that lets through is delivered now, as Linux does
 * on the way out of sigreturn (a signal the handler sent itself, blocked
 * while it ran, then acts before the thread goes on). */
void sig_fault_end(uint64_t mask);

/* The raise entry (sigfault.c), on the raised thread, in the kernel's
 * window; regs are its registers where it was (SIG_REGS). Take a signal
 * due on it for its handler to run now: true with *si the signal and *act
 * and *was as sig_fault_begin gives them (the handler's mask is set). An
 * ignored one is dropped, a default that terminates ends the process.
 * False to go on at once: nothing due; a thread libc doesn't list; inside
 * a call (its boundary delivers, with regs in the handler's ucontext);
 * inside a quiet call or holding sig_lock (raised again once out). */
bool sig_raise_begin(const uint64_t *regs, struct sig_info *si, struct sig_act_io *act,
                     uint64_t *was);
/* After a raise's handler: the thread's mask is `mask` (its ucontext's),
 * and the next signal due is taken as sig_raise_begin takes one (true). */
bool sig_raise_next(uint64_t mask, struct sig_info *si, struct sig_act_io *act, uint64_t *was);
/* Raise the calling (listed) thread again: the raise comes at its next
 * interrupt in its own code (sigfault.c: a raise that came on a handler's
 * way out, once that has resumed the thread). */
void sig_raise_later(void);

/* The signal a fault that killed a process (process_info's
 * PROCESS_FLAG_FAULT with its fault_vector and fault_status) is, as
 * Linux's waitpid says it: SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGTRAP; out
 * of memory is SIGKILL (the OOM killer's), a page that doesn't match the
 * approved program SIGBUS. */
int sig_fault_signal(uint32_t vector, int32_t status);

/* ---- the alternate stack (sigfault.c; sigcalls.c's sigaltstack) ---------------------- */

/* Each thread's own (a new thread has none; no lock). The kernel's
 * exception stack (thread_exception_stack) is the same range, so a fault
 * that finds the thread's stack unusable reaches the entry on it: a
 * handler with SA_ONSTACK runs there, one without it is moved to the
 * interrupted stack (sigfault.c). */

/* The calling thread's, as sigaltstack says it for the stack pointer sp:
 * the return is SS_DISABLE (none: *base and *size 0), SS_ONSTACK (sp is on
 * it) or 0. */
int  sig_alt_state(uintptr_t sp, uintptr_t *base, size_t *size);
/* Set it to [base, base + size), or remove it (size 0). The kernel's
 * range is set first. 0, or -EFAULT (the kernel refused the range). The
 * caller checked the size (sig_alt_min) and that it isn't on it. */
long sig_alt_set(uintptr_t base, size_t size);
/* The smallest stack a fault's frame and its siginfo and ucontext fit on
 * with a little room for a handler: what sigaltstack's ENOMEM compares
 * with (never less than MINSIGSTKSZ). Depends on the CPU's vector state. */
size_t sig_alt_min(void);
/* fn(arg) as a handler with SA_ONSTACK runs: on the calling thread's
 * alternate stack unless there is none or it is on it already. */
void sig_run_onstack(void (*fn)(void *), void *arg);
/* The calling thread's kernel exception stack set again from the record,
 * for a thread that has the record's memory and not the kernel's state (a
 * forked child's). */
void sig_alt_reapply(void);

/* ---- the signal thread (sigthread.c) ------------------------------------------------- */

/* The port the signal thread waits on, made at the start: proc.c binds
 * every child to it (keys below SIG_KEY_OURS are its serials) and the
 * signal thread hands their packets to lx_child_packet. */
#define SIG_KEY_OURS (1ull << 63)
handle_t sig_port(void);
/* The calling thread is the signal thread (it has no thread-local storage:
 * the call boundary leaves it alone). */
bool     sig_on_signal_thread(void);
/* Write `send` on a child's signal channel ch, waiting up to a second for
 * room: 0, or -errno. A channel whose program has ended is 0 (it is gone). */
long     sig_channel_send(handle_t ch, int signo, int code, int32_t value);
/* A timer's binding: the kernel timer h fires a packet the signal thread
 * hands to sigtimer.c (sig_timer_fired). */
status_t sig_timer_bind(handle_t h, unsigned index);

/* ---- timers (sigtimer.c) ----------------------------------------------------------- */

/* The timer at index fired (the signal thread's): its signal, and the next
 * expiry for an interval timer. */
void sig_timer_fired(unsigned index);
