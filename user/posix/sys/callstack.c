/* The layer's stack for calls (calls.h; docs/SG-REVIEW.md, design
 * question 1, the owner's answer (a)). On Linux a call runs on the
 * kernel's stack and uses the caller's for its return address alone; the
 * layer runs a call on its caller's stack, and its frames are not a few
 * bytes: a path's are some 4 KiB each, the deepest call made only of
 * direct calls ~29 KiB (lx_mkdir, by GCC's -fcallgraph-info, 2026-10-17;
 * most path calls 16 to 25 KiB), and the handlers a call's boundary runs
 * come on top. A small stack overruns, into whatever is mapped below it
 * (a malloc'd alternate stack has the heap there). So a thread that may
 * run on one gets a stack of the layer's own, CALL_STACK bytes over a
 * guard page (about twice the deepest call), mapped when it is known:
 *   - a thread whose own stack is under LX_CALL_STACK (pthread_create's
 *     size, musl's clone.c says it; one it can't say counts as small), at
 *     its creation (thread.c), and clone()'s child of a process (fork.c,
 *     gtest's two pages): every call it makes moves there ("always");
 *   - a thread given an alternate signal stack under LX_CALL_STACK, at
 *     sigaltstack (sigcalls.c): the calls a handler makes on it move
 *     there.
 * Threads with neither, and processes with none (lx_call_stack_set
 * false: the one test each call makes), run calls where they are.
 *
 * Where a call runs (lx_call_stack_run; musl's direct ways into the layer
 * go through it too, each inside its quiet stretch):
 *   - on the stack for calls already (a call a handler makes there, or a
 *     handler's from a call's boundary): where it is, below the frames in
 *     use, as one call nested in another;
 *   - on the thread's alternate stack (a handler's call): at `next`, under
 *     the kernel's exception stack cut to below what the handler uses
 *     (sigfault.c's sig_alt_call), so a frame the kernel makes meanwhile
 *     (a raise, a fault) goes below the handler's frames, not over them
 *     at the stack's top, and the thread counts as on its alternate stack
 *     until the call is back (sigaltstack says SS_ONSTACK, a handler with
 *     SA_ONSTACK at its boundary runs where it is);
 *   - anywhere else: at the top when every call moves, else where it is.
 * `next` is the top unless the thread left frames on the stack for calls
 * to run a handler on its alternate stack: sig_run_onstack from a call's
 * boundary, or a fault or a raise that came while it was on it
 * (sig_fault_run): those say where they left it (lx_call_stack_hold),
 * and a call from the thread's own stack, which only comes once none are
 * left, sets it back to the top.
 *
 * The stack is the thread's until it ends: unmapped at its exit (lx_exit,
 * from on it: registers only after the unmap, as musl's __unmapself does
 * for the thread's own stack) or by __unmapself's first half (a detached
 * thread, off it). A forked child has its copy (the forking thread's
 * thread-local state and the mapping both). */
#include <errno.h>
#include <sys/mman.h>
#include "layer.h"
#include "sig.h"

#define CALL_STACK LX_CALL_STACK
#define CALL_GUARD 4096u
#define HOLD_ROOM  512u   /* left below a held place: a red zone (128) and a small frame */

bool lx_call_stack_set;   /* calls.h: some thread of this process has one */

/* The calling thread's: [lo, top) over a guard page, 0 for none. */
static __thread struct {
    uintptr_t lo, top;
    uintptr_t next;     /* where a call moved from the alternate stack starts */
    bool      always;   /* the thread's own stack is small: every call moves */
} cs;

void __jam_sig_stack_call(void (*fn)(void *), void *arg, void *top);   /* sigentry.S */
/* callexit.S: vmar_unmap(vmar, base, size), then thread_exit. */
_Noreturn void __jam_call_stack_exit(handle_t vmar, uintptr_t base, size_t size);

uintptr_t lx_call_stack_map(void)
{
    long b = lx_mmap(0, CALL_STACK + CALL_GUARD, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b < 0)
        return 0;
    (void)lx_mprotect(b, CALL_GUARD, PROT_NONE, 0, 0, 0);
    return (uintptr_t)b + CALL_GUARD;
}

void lx_call_stack_unmap(uintptr_t lo)
{
    if (lo)
        (void)lx_munmap((long)(lo - CALL_GUARD), CALL_STACK + CALL_GUARD, 0, 0, 0, 0);
}

void lx_call_stack_adopt(uintptr_t lo, bool always)
{
    if (!lo)
        return;
    cs.lo = lo;
    cs.top = cs.next = lo + CALL_STACK;
    cs.always = always;
    __atomic_store_n(&lx_call_stack_set, true, __ATOMIC_RELAXED);
}

void lx_call_stack_need(bool always)
{
    if (cs.top)
        cs.always |= always;   /* a clone child of a clone child: its parent's, in the copy */
    else
        lx_call_stack_adopt(lx_call_stack_map(), always);
}

void lx_call_stack_run(void (*fn)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    /* The signal thread has no thread pointer to read ours by: it is told
     * by its stack, first. */
    if (!__atomic_load_n(&lx_call_stack_set, __ATOMIC_RELAXED) || sig_on_signal_thread() ||
        !cs.top || (sp > cs.lo && sp <= cs.top)) {
        fn(arg);
        return;
    }
    if (sig_alt_from(sp)) {
        sig_alt_call(fn, arg, (void *)cs.next);
        return;
    }
    cs.next = cs.top;   /* from its own stack: nothing of the thread's is left on ours */
    if (cs.always)
        __jam_sig_stack_call(fn, arg, (void *)cs.top);
    else
        fn(arg);
}

void lx_call_stack_hold(uintptr_t sp)
{
    if (!__atomic_load_n(&lx_call_stack_set, __ATOMIC_RELAXED) || !cs.top || sp <= cs.lo ||
        sp > cs.top)
        return;
    /* Below the red zone, and below the frame of a caller that passed its
     * frame's address and the two words its switch of stacks pushes. */
    uintptr_t below = sp - cs.lo > HOLD_ROOM ? (sp - HOLD_ROOM) & ~(uintptr_t)15 : cs.lo;
    if (below < cs.next)
        cs.next = below;
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
    lx_call_stack_run(moved_call, &m);
    return m.r;
}

void lx_call_stack_free(void)
{
    if (!cs.top)
        return;
    uintptr_t base = cs.lo - CALL_GUARD;
    cs.lo = cs.top = cs.next = 0;
    mem_forget(base, CALL_STACK + CALL_GUARD);
    (void)jam_vmar_unmap(lx_self_vmar(), base, CALL_STACK + CALL_GUARD);
}

_Noreturn void lx_call_stack_exit(void)
{
    if (!cs.top)
        jam_thread_exit();
    uintptr_t base = cs.lo - CALL_GUARD;
    mem_forget(base, CALL_STACK + CALL_GUARD);
    /* Perhaps on it: from the unmap to the thread's end, registers only. */
    __jam_call_stack_exit(lx_self_vmar(), base, CALL_STACK + CALL_GUARD);
}
