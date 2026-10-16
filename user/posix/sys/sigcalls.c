/* The signal calls (docs/M13-PLAN.md section 6): rt_sigaction,
 * rt_sigprocmask, rt_sigpending, rt_sigsuspend, pause, rt_sigtimedwait,
 * sigaltstack, and sending: kill, tkill, tgkill, rt_sigqueueinfo and
 * rt_tgsigqueueinfo. The state they act on is signal.c's (sig.h).
 *
 * Who can be signalled: this process, its threads, and its children
 * (through the signal channel posix_spawn made for each: proc.c's
 * lx_child_signal). Any other pid is ESRCH: a pid names nothing a program
 * can reach otherwise. A process group is our children in it, and us if
 * it is ours.
 *
 * The waits for a signal (sigsuspend, pause, sigtimedwait) sleep with
 * sleep_until, an interruptible wait: a sender interrupts the thread it
 * routed a signal to, and the wait looks again. sigaltstack is the
 * thread's own alternate stack, which handlers with SA_ONSTACK run on
 * (signal.c's call-boundary handlers and sigfault.c's faults). */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include "proc.h"
#include "sig.h"

/* Linux's struct k_sigaction (x86-64), what musl's sigaction passes. */
struct k_sigaction {
    uintptr_t handler;
    unsigned long flags;
    uintptr_t restorer;   /* never used: handlers return to libc */
    uint64_t mask;
};

#define SA_RESTORER_LX 0x04000000   /* musl always sets it */
#define SA_KNOWN (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK | SA_RESTART | \
                  SA_NODEFER | SA_RESETHAND | SA_RESTORER_LX)

static bool valid_signo(long sig)
{
    return sig >= 1 && sig <= SIG_MAX;
}

/* ---- dispositions and masks --------------------------------------------------------- */

long lx_rt_sigaction(long sig, long act, long oact, long size, long e, long f)
{
    if (size != sizeof(uint64_t) || !valid_signo(sig))
        return -EINVAL;
    struct k_sigaction k;   /* the caller's, read once */
    const struct k_sigaction *ka = act ? &k : NULL;
    if (act && uc_get(&k, (const void *)(uintptr_t)act, sizeof(k)))
        return -EFAULT;
    struct sig_act_io in, out;
    if (ka) {
        /* musl passes sa_flags, an int, sign-extended: SA_RESETHAND is bit 31. */
        uint32_t flags = (uint32_t)ka->flags;
        if (sig == SIGKILL || sig == SIGSTOP || (flags & ~(uint32_t)SA_KNOWN))
            return -EINVAL;
        in = (struct sig_act_io){ ka->handler, flags & ~(uint32_t)SA_RESTORER_LX, ka->mask };
    }
    long r = sig_action((int)sig, ka ? &in : NULL, oact ? &out : NULL);
    if (!r && oact) {
        struct k_sigaction o = { .handler = out.handler, .flags = out.flags, .mask = out.mask };
        r = uc_put((void *)(uintptr_t)oact, &o, sizeof(o));   /* after the change, as Linux */
    }
    return r;
}

long lx_rt_sigprocmask(long how, long set, long old, long size, long e, long f)
{
    if (size != sizeof(uint64_t))
        return -EINVAL;
    if (set && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
        return -EINVAL;
    uint64_t s = 0;
    if (set && uc_get(&s, (const void *)(uintptr_t)set, sizeof(s)))
        return -EFAULT;
    uint64_t was = sig_set_mask((int)how, s, set != 0);
    return old ? uc_put((void *)(uintptr_t)old, &was, sizeof(was)) : 0;
}

long lx_rt_sigpending(long set, long size, long c, long d, long e, long f)
{
    if (size != sizeof(uint64_t))
        return -EINVAL;
    uint64_t s = sig_pending_set();
    return uc_put((void *)(uintptr_t)set, &s, sizeof(s));
}

/* The calling thread's alternate stack (sigfault.c keeps it, and the
 * kernel's exception stack the same). The order is Linux's: on it is
 * EPERM, a flag other than SS_DISABLE (SS_ONSTACK is taken as "on")
 * EINVAL, a size under the smallest a fault's frame fits (never under
 * MINSIGSTKSZ) ENOMEM; the old one is written only if the call succeeds.
 * Whether we are on it is this call's own stack pointer: the layer runs
 * on the caller's stack. */
long lx_sigaltstack(long ss, long old, long c, long d, long e, long f)
{
    stack_t want;   /* the caller's, read once */
    const stack_t *in = ss ? &want : NULL;
    if (ss && uc_get(&want, (const void *)(uintptr_t)ss, sizeof(want)))
        return -EFAULT;
    uintptr_t base;
    size_t size;
    int flags = sig_alt_state((uintptr_t)__builtin_frame_address(0), &base, &size);
    if (in) {
        if (flags == SS_ONSTACK)
            return -EPERM;
        if (in->ss_flags & ~(SS_DISABLE | SS_ONSTACK))
            return -EINVAL;
        long r;
        if (in->ss_flags & SS_DISABLE) {
            r = sig_alt_set(0, 0);
        } else {
            if (in->ss_size < sig_alt_min())
                return -ENOMEM;
            r = sig_alt_set((uintptr_t)in->ss_sp, in->ss_size);
        }
        if (r)
            return r;
    }
    stack_t o;
    memset(&o, 0, sizeof(o));
    o.ss_sp = (void *)base;
    o.ss_size = size;
    o.ss_flags = flags;
    return old ? uc_put((void *)(uintptr_t)old, &o, sizeof(o)) : 0;
}

/* ---- waiting for a signal ----------------------------------------------------------- */

/* Sleep until a handler has run on this thread (or a cancellation is
 * asked for): what pause and sigsuspend wait for. -EINTR. */
static long until_handled(void)
{
    uint32_t h = sig_handled_self();
    while (sig_handled_self() == h && !sig_cancel_asked()) {
        if (sig_due_self())
            sig_deliver_self();
        else
            (void)jam_sleep_until(DEADLINE_NEVER);   /* ended by the sender's interrupt */
    }
    return -EINTR;
}

long lx_pause(long a, long b, long c, long d, long e, long f)
{
    return until_handled();
}

long lx_rt_sigsuspend(long mask, long size, long c, long d, long e, long f)
{
    if (size != sizeof(uint64_t))
        return -EINVAL;
    uint64_t m;
    if (uc_get(&m, (const void *)(uintptr_t)mask, sizeof(m)))
        return -EFAULT;
    uint64_t was = sig_set_mask(SIG_SETMASK, m, true);
    long r = until_handled();   /* the handlers run with the call's mask */
    (void)sig_set_mask(SIG_SETMASK, was, true);
    return r;
}

/* A relative timeout as an uptime deadline: 0, or -EINVAL. */
static long deadline_of(const struct timespec *ts, uint64_t *out)
{
    if (!ts) {
        *out = DEADLINE_NEVER;
        return 0;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (long)NS_PER_S)
        return -EINVAL;
    uint64_t ns, at;
    if (__builtin_mul_overflow((uint64_t)ts->tv_sec, NS_PER_S, &ns) ||
        __builtin_add_overflow(ns, (uint64_t)ts->tv_nsec, &ns) || __builtin_add_overflow(now(), ns, &at))
        at = DEADLINE_NEVER;
    *out = at;
    return 0;
}

/* Wait until one of want is pending (into *si: its number), deadline
 * passes (-EAGAIN), or another signal's handler is due (-EINTR). */
static long wait_for(uint64_t want, uint64_t deadline, struct sig_info *si)
{
    for (;;) {
        if (sig_take_waited(want, si))
            return si->signo;
        if (sig_cancel_asked() || sig_due_self())
            return -EINTR;   /* the boundary runs the handler, with the mask back */
        if (jam_sleep_until(deadline) == OK)
            return sig_take_waited(want, si) ? si->signo : -EAGAIN;
    }
}

long lx_rt_sigtimedwait(long set, long info, long timeout, long size, long e, long f)
{
    if (size != sizeof(uint64_t))
        return -EINVAL;
    uint64_t want, deadline;   /* the caller's set and timeout, read once */
    struct timespec ts;
    if (uc_get(&want, (const void *)(uintptr_t)set, sizeof(want)) ||
        (timeout && uc_get(&ts, (const void *)(uintptr_t)timeout, sizeof(ts))))
        return -EFAULT;
    want &= ~SIG_NEVER_MASKED;
    long r = deadline_of(timeout ? &ts : NULL, &deadline);
    if (r)
        return r;
    struct sig_info si;
    if (!sig_take_waited(want, &si)) {
        uint64_t was;
        sig_wait_begin(want, &was);
        r = wait_for(want, deadline, &si);
        sig_wait_end(was);
        if (r < 0)
            return r;
    }
    siginfo_t out;
    if (info) {
        sig_fill_siginfo(&si, &out);
        if (uc_put((void *)(uintptr_t)info, &out, sizeof(out)))
            return -EFAULT;   /* the signal is taken: Linux's too */
    }
    return si.signo;
}

/* ---- sending ------------------------------------------------------------------------ */

/* Signal pid's targets (kill's rules) with si (a child gets its signal
 * number, code and value). */
static long send_to(long pid, const struct sig_info *si)
{
    int32_t self = lx_pid(), group = lx_pgid();
    if (pid > INT32_MAX || pid < -(long)INT32_MAX)
        return -ESRCH;
    if (pid == self) {
        if (si->signo)
            sig_send_process(si);
        return 0;
    }
    long n;
    if (pid > 0)
        n = lx_child_signal((int32_t)pid, 0, si->signo, si->code, (int32_t)si->value);
    else if (pid == -1)
        n = lx_child_signal(0, 0, si->signo, si->code, (int32_t)si->value);
    else
        n = lx_child_signal(0, pid ? (int32_t)-pid : group, si->signo, si->code, (int32_t)si->value);
    if (n < 0)
        return n;
    bool us = pid == 0 || -pid == group;
    if (us && si->signo)
        sig_send_process(si);   /* after the children: a default may end us */
    return n || us ? 0 : -ESRCH;
}

long lx_kill(long pid, long sig, long c, long d, long e, long f)
{
    if (sig < 0 || sig > SIG_MAX)
        return -EINVAL;
    struct sig_info si = { .signo = (int32_t)sig, .code = SI_USER, .pid = lx_pid() };
    return send_to(pid, &si);
}

long lx_rt_sigqueueinfo(long pid, long sig, long info, long d, long e, long f)
{
    if (sig < 0 || sig > SIG_MAX)
        return -EINVAL;
    siginfo_t in;   /* the caller's, read once */
    if (uc_get(&in, (const void *)(uintptr_t)info, sizeof(in)))
        return -EFAULT;
    if (in.si_code >= 0 && pid != lx_pid())
        return -EPERM;   /* only sigqueue's own codes go to another process (Linux's rule) */
    struct sig_info si = { .signo = (int32_t)sig, .code = in.si_code, .pid = lx_pid(),
                           .value = (intptr_t)in.si_value.sival_ptr,
                           .addr = (uintptr_t)in.si_addr };
    return send_to(pid, &si);
}

long lx_tkill(long tid, long sig, long c, long d, long e, long f)
{
    if (sig < 0 || sig > SIG_MAX)
        return -EINVAL;
    if (tid <= 0 || tid > UINT32_MAX)
        return -EINVAL;
    struct sig_info si = { .signo = (int32_t)sig, .code = SI_TKILL, .pid = lx_pid() };
    return sig_send_thread((uint32_t)tid, &si);
}

long lx_tgkill(long tgid, long tid, long sig, long d, long e, long f)
{
    if (tgid <= 0 || tid <= 0)
        return -EINVAL;
    return tgid == lx_pid() ? lx_tkill(tid, sig, 0, 0, 0, 0) : -ESRCH;
}

long lx_rt_tgsigqueueinfo(long tgid, long tid, long sig, long info, long e, long f)
{
    if (sig < 0 || sig > SIG_MAX || tid <= 0 || tid > UINT32_MAX)
        return -EINVAL;
    if (tgid != lx_pid())
        return -ESRCH;
    /* Any code to a thread of ours, as Linux allows within a thread group: a
     * fault's code and address too (Chromium's stack dump sends a fault on
     * so), given as the handler's siginfo at this call's boundary. */
    siginfo_t in;   /* the caller's, read once */
    if (uc_get(&in, (const void *)(uintptr_t)info, sizeof(in)))
        return -EFAULT;
    struct sig_info si = { .signo = (int32_t)sig, .code = in.si_code, .pid = lx_pid(),
                           .value = (intptr_t)in.si_value.sival_ptr,
                           .addr = (uintptr_t)in.si_addr };
    return sig_send_thread((uint32_t)tid, &si);
}
