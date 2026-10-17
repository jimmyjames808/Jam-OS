/* The layer's stack for calls (calls.h): a clone child's calls. The
 * layer runs a call on its caller's stack, and its frames are not
 * Linux's few bytes: one path's are some 4 KiB each, the deepest call
 * made only of direct calls ~29 KiB (lx_mkdir, by GCC's -fcallgraph-info,
 * 2026-10-17; most path calls 16 to 25 KiB), and the handlers a call's
 * boundary runs come on top. So in clone()'s child (fork.c), whose fn
 * may run on two pages, each call fn makes runs on a stack the layer maps
 * there, CALL_STACK bytes over a guard page (about twice that deepest
 * call), as Linux's kernel runs a call on a stack of its own. The thread
 * is told by a thread-local top, which every thread the child makes
 * later has 0 for. */
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include "layer.h"
#include "sig.h"

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
void lx_call_stack_run(void (*fn)(void *), void *arg)
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
void lx_call_stack_need(bool always)
{
    (void)always;   /* every call moves: the one kind there is */
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
    lx_call_stack_run(moved_call, &m);
    return m.r;
}
