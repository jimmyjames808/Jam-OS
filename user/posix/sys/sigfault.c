/* Faults as Linux's signals (docs/SIGNALS-PLAN.md section 1, libc's
 * part): the process's exception entry, the frame the kernel gives it
 * made into a siginfo_t and a ucontext_t, the handler run on the faulting
 * thread, and the thread resumed from the ucontext as the handler left it.
 *
 * The entry (sigentry.S's __jam_sig_entry, which calls sig_fault here) is
 * set from the start (sig_entry_need's SIG_ENTRY_COPY): a fault in one of
 * ucopy.S's checked copies goes on at its fixup with -EFAULT, first, with
 * no handler installed and no signal state touched. Any other fault whose
 * signal is blocked, ignored or default (a program that never asks for a
 * handler has only those), on a thread libc doesn't list, or on a thread
 * that holds sig_lock (sig_fault_begin) is EXC_UNHANDLED: the kernel's
 * kill with the line it would have had with no entry.
 *
 * From the entry to EXC_LEAVE the thread is in the kernel's window, where
 * a second fault kills the process: up to there only the frame, the stack
 * below it and the thread's own record are touched. Then the handler runs
 * as ordinary code of the thread (it may siglongjmp out: the window is
 * closed). When it returns, its ucontext's registers, vector state and
 * mask go back into the frame and the thread, and EXC_RESUME continues
 * the thread from them. A handler that returns without a change runs the
 * faulting instruction again: V8's and Chromium's way of passing a fault
 * on (reset the action, return), so the fault comes again to the next
 * handler or to the kernel's kill.
 *
 * What the handler is given (struct fault_ctx) is on the stack below the
 * frame, as Linux's rt_sigframe is: musl's x86-64 ucontext_t (gregs from
 * the frame, REG_TRAPNO, REG_ERR and REG_CR2 the vector, the CPU's error
 * code and the address; uc_sigmask the mask before the handler) and the
 * siginfo_t (si_code > 0 always: V8 tells a fault from a kill by it). The
 * ucontext's fpregs points at a copy of the frame's XSAVE area laid out as
 * Linux's: its first 512 bytes struct _fpstate, fpx_sw_bytes in their
 * reserved tail and FP_XSTATE_MAGIC2 just after the area, so a reader
 * that looks for the extended state finds it. A copy, because the frame
 * has no room after it (the interrupted code's red zone is there).
 *
 * The alternate stack (sigaltstack, SG4) is the kernel's exception stack
 * for the thread, set by sig_alt_set: while the thread's RSP is outside
 * it the kernel puts every frame at its top, whichever handler the signal
 * has. A handler with SA_ONSTACK runs there (uc_stack says so, and
 * sigaltstack inside it says SS_ONSTACK). One without it runs on the
 * interrupted stack, as on Linux: sig_fault copies the frame and what the
 * rest needs below the interrupted RSP's red zone, and the rest runs
 * there (__jam_sig_moved), still in the window, so a stack that can't take
 * the copy (an overflow) is the kernel's kill with the first fault's line,
 * a SIGSEGV for waitpid. It is the whole rest that moves, not only the
 * handler: a nested fault is served on the alternate stack again, over
 * this frame and what sig_fault keeps there.
 *
 * A raise (an EXC_RAISED frame: a signal sent to this thread while it was
 * busy in its own code, signal.c's kick) takes the same path with a
 * pending signal in place of the fault's (sig_raise_begin): the handler
 * runs with the registers where the thread was, on the stack its
 * SA_ONSTACK says, and the thread goes on from what it leaves. Then the
 * next signal due runs on the same frame (sig_raise_next), until none is,
 * each SA_ONSTACK one through sig_run_onstack and the others where the
 * first ran. A raise that finds the thread inside the layer, or nothing
 * due, resumes at once. */
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include "calls.h"
#include "proc.h"
#include "sig.h"

#define LEGACY_SIZE      512u          /* FXSAVE's area: struct _fpstate */
#define FCW_AT           0u            /* the x87 control word, in the legacy area */
#define FSW_AT           2u            /* its status word */
#define MXCSR_AT         24u
#define FPX_SW_AT        464u          /* fpx_sw_bytes: the legacy area's last 48 bytes */
#define XSTATE_BV_AT     512u          /* the XSAVE header's first word */
#define XSAVE_MAX        (64u << 10)   /* past any CPU's area: a frame saying more is refused */
#define FP_XSTATE_MAGIC1 0x46505853u   /* Linux's, in fpx_sw_bytes */
#define FP_XSTATE_MAGIC2 0x46505845u   /* Linux's, just after the area */
#define UC_FP_XSTATE     1ul           /* uc_flags: fpregs has the XSAVE header and more */
#define UC_SIGCONTEXT_SS 2ul           /* uc_flags: REG_CSGSFS's top 16 bits are SS */
#define RED_ZONE        128u          /* below RSP: the x86-64 ABI's, leaf code's */
#define FAULT_ROOM       1024u         /* sig_fault's own frames, in sig_alt_min */
#define HANDLER_ROOM     1024u         /* a small handler's, in sig_alt_min */

/* Linux's struct _fpx_sw_bytes. */
struct fpx_sw_bytes {
    uint32_t magic1;          /* FP_XSTATE_MAGIC1 */
    uint32_t extended_size;   /* the area and MAGIC2 */
    uint64_t xfeatures;       /* the components in the area (its XSTATE_BV) */
    uint32_t xstate_size;     /* the area */
    uint32_t padding[7];
};

_Static_assert(offsetof(struct exc_frame, r8) == 48 && offsetof(struct exc_frame, rbx) == 136 &&
               offsetof(struct exc_frame, rsp) == 168 && offsetof(struct exc_frame, rip) == 176,
               "sigentry.S's offsets");
_Static_assert(offsetof(struct exc_frame, rflags) - offsetof(struct exc_frame, r8) ==
               REG_EFL * sizeof(greg_t) && REG_EFL + 1 == SIG_REGS,
               "the frame's registers in gregs' order");
_Static_assert(sizeof(struct fpx_sw_bytes) == LEGACY_SIZE - FPX_SW_AT, "fpx_sw_bytes");

/* What the handler is given, below the frame (the vector state's copy is
 * below it, 64-byte aligned). */
struct fault_ctx {
    ucontext_t uc;
    siginfo_t  info;
};

/* A fault's signal (the table in docs/SIGNALS-PLAN.md section 1: Linux's
 * answers). */
struct fault_sig {
    int      sig;    /* 0: a spurious x87 or SIMD error (no unmasked one pending) */
    int      code;
    uint64_t addr;   /* si_addr */
};

/* What the rest of a fault or a raise needs once the signal is taken
 * (sig_fault_run): kept with the frame's copy below the interrupted RSP
 * when the handler runs there. */
struct fault_run {
    struct exc_frame *f;
    struct sig_info   si;    /* the signal: a fault's, or one pending for a raise */
    struct sig_act_io act;   /* the disposition as sig_fault_begin took it */
    uint64_t          was;   /* the mask before the handler */
};

/* A handler's call (through sig_run_onstack for one with SA_ONSTACK). */
struct handler_args {
    const struct sig_act_io *act;
    int                      sig;
    struct fault_ctx        *ctx;
};

void           __jam_sig_entry(void);   /* sigentry.S */
_Noreturn void __jam_sig_moved(struct exc_frame *copy, struct fault_run *run, void *top);
void           __jam_sig_stack_call(void (*fn)(void *), void *arg, void *top);
_Noreturn void sig_fault(struct exc_frame *f);
_Noreturn void sig_fault_run(struct fault_run *r);

/* ---- the entry ---------------------------------------------------------------------- */

static unsigned needs;   /* SIG_ENTRY_*: why the entry is set; sig_lock */

void sig_entry_need(unsigned why, bool on)
{
    unsigned now = on ? needs | why : needs & ~why;
    if (!now != !needs)
        (void)jam_exception_entry_set(now ? (uint64_t)(uintptr_t)__jam_sig_entry : 0);
    needs = now;
}

/* ---- the signal --------------------------------------------------------------------- */

/* An x87 (16) or SIMD (19) error's si_code from the unmasked exceptions
 * pending in the frame's state (Linux's fpu__exception_code); 0 if none. */
static int fpe_code(const struct exc_frame *f)
{
    unsigned err;
    if (f->vector == 16) {
        uint16_t cw, sw;
        memcpy(&cw, f->xsave + FCW_AT, sizeof(cw));
        memcpy(&sw, f->xsave + FSW_AT, sizeof(sw));
        err = sw & ~(cw & 0x3fu);
    } else {
        uint32_t mxcsr;
        memcpy(&mxcsr, f->xsave + MXCSR_AT, sizeof(mxcsr));
        err = ~(mxcsr >> 7) & mxcsr;   /* the flags whose mask bit is clear */
    }
    if (err & 0x01)
        return FPE_FLTINV;
    if (err & 0x04)
        return FPE_FLTDIV;
    if (err & 0x08)
        return FPE_FLTOVF;
    if (err & 0x12)
        return FPE_FLTUND;   /* denormal or underflow */
    if (err & 0x20)
        return FPE_FLTRES;
    return 0;
}

static struct fault_sig classify(const struct exc_frame *f)
{
    switch (f->vector) {
    case 14:   /* page fault: by aspace_fault's status */
        if (f->status == ERR_ACCESS_DENIED)
            return (struct fault_sig){ SIGSEGV, SEGV_ACCERR, f->addr };
        if (f->status == ERR_OUT_OF_RANGE || f->status == ERR_IO ||
            f->status == ERR_PEER_CLOSED || f->status == ERR_TIMED_OUT)
            return (struct fault_sig){ SIGBUS, BUS_ADRERR, f->addr };   /* a truncated or failed file */
        return (struct fault_sig){ SIGSEGV, SEGV_MAPERR, f->addr };    /* nothing mapped there */
    case 0:    /* divide error */
        return (struct fault_sig){ SIGFPE, FPE_INTDIV, f->rip };
    case 1:    /* debug trap: a single step, int1 */
        return (struct fault_sig){ SIGTRAP, TRAP_TRACE, f->rip };
    case 3:    /* int3, RIP after it: Linux's code for it is SI_KERNEL */
        return (struct fault_sig){ SIGTRAP, SI_KERNEL, 0 };
    case 6:    /* invalid opcode */
        return (struct fault_sig){ SIGILL, ILL_ILLOPN, f->rip };
    case 11:   /* segment not present */
    case 12:   /* stack segment */
        return (struct fault_sig){ SIGBUS, SI_KERNEL, 0 };
    case 16:   /* x87 */
    case 19: { /* SIMD */
        int code = fpe_code(f);
        return (struct fault_sig){ code ? SIGFPE : 0, code, f->rip };
    }
    case 17:   /* alignment check */
        return (struct fault_sig){ SIGBUS, BUS_ADRALN, 0 };
    default:   /* 13 general protection (a non-canonical address too), 4, 5, 21 */
        return (struct fault_sig){ SIGSEGV, SI_KERNEL, 0 };
    }
}

int sig_fault_signal(uint32_t vector, int32_t status)
{
    if (vector == 14 && status == ERR_NO_MEMORY)
        return SIGKILL;   /* out of memory, or the job's page limit */
    if (vector == 14 && status == ERR_BAD_STATE)
        return SIGBUS;    /* a page that doesn't match the approved program */
    if (vector == 16 || vector == 19)
        return SIGFPE;    /* killed with an unmasked exception pending */
    struct exc_frame f = { .vector = vector, .status = status };
    return classify(&f).sig;
}

/* ---- the alternate stack ------------------------------------------------------------ */

/* The calling thread's sigaltstack: this thread's own, so no lock; a new
 * thread's is zero (none). The kernel has the same range, but for ktop:
 * while a call a handler made here runs on the layer's stack for calls
 * (sig_alt_call), the kernel's range ends below what the handler uses. */
static __thread struct {
    uintptr_t base;
    size_t    size;
    uintptr_t ktop;   /* the kernel's range's top: base + size, or lower (cut) */
} alt;

#define CUT_ROOM 1024u   /* below sig_alt_call's frame: its own calls' and their red zones */

/* sp is on the stack: above its base, at most its top (Linux's on_sig_stack
 * and the kernel's own test). */
static bool on_alt(uintptr_t sp)
{
    return alt.size && sp > alt.base && sp - alt.base <= alt.size;
}

/* A call a handler made on the stack is running on the stack for calls:
 * the thread is on its alternate stack, as on Linux, whose call would
 * run on the kernel's stack from it. */
static bool alt_cut(void)
{
    return alt.size && alt.ktop != alt.base + alt.size;
}

int sig_alt_state(uintptr_t sp, uintptr_t *base, size_t *size)
{
    *base = alt.base;
    *size = alt.size;
    return !alt.size ? SS_DISABLE : on_alt(sp) || alt_cut() ? SS_ONSTACK : 0;
}

long sig_alt_set(uintptr_t base, size_t size)
{
    if (size == 0)
        base = 0;
    if (jam_thread_exception_stack(base, size) != OK)
        return -EFAULT;
    alt.base = base;
    alt.size = size;
    alt.ktop = base + size;
    return 0;
}

void sig_alt_reapply(void)
{
    if (alt.size)
        (void)jam_thread_exception_stack(alt.base, alt.ktop - alt.base);
}

/* The kernel's range made [alt.base, top) (top within the stack). */
static bool alt_kernel_top(uintptr_t top)
{
    if (jam_thread_exception_stack(alt.base, top - alt.base) != OK)
        return false;
    alt.ktop = top;
    return true;
}

bool sig_alt_from(uintptr_t sp)
{
    if (on_alt(sp))
        return true;
    /* Off it with the range still cut: a handler left a call on the
     * stack for calls by siglongjmp. Nothing of it is in use any more. */
    if (alt_cut())
        (void)alt_kernel_top(alt.base + alt.size);
    return false;
}

void sig_alt_call(void (*fn)(void *), void *arg, void *top)
{
    uintptr_t was = alt.ktop;
    uintptr_t cut = ((uintptr_t)__builtin_frame_address(0) - CUT_ROOM) & ~(uintptr_t)15;
    bool cutting = cut > alt.base && cut < was && alt_kernel_top(cut);
    __jam_sig_stack_call(fn, arg, top);
    if (cutting)
        (void)alt_kernel_top(was);
}

/* The bytes of vector state the kernel saves in a frame: the XSAVE area
 * for the features it enables (CPUID leaf 0xD's size, the area as the
 * kernel sizes it), or FXSAVE's 512. A CPU with XSAVE has leaf 0xD. */
static size_t xsave_bytes(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(c & (1u << 27)))   /* no XSAVE, or the OS hasn't enabled it */
        return LEGACY_SIZE;
    a = 0xd;
    c = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return b < LEGACY_SIZE ? LEGACY_SIZE : (b + 63u) & ~63u;
}

size_t sig_alt_min(void)
{
    static size_t cached;   /* a racing first call computes the same */
    size_t min = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (!min) {
        size_t xs = xsave_bytes();
        /* the kernel's frame at the top, then what sig_fault_run builds
         * (the ucontext and siginfo, a copy of the vector state) and
         * its own and a handler's frames */
        min = EXC_FRAME_XSAVE + xs + sizeof(struct fault_ctx) + xs + FAULT_ROOM + HANDLER_ROOM;
        if (min < MINSIGSTKSZ)
            min = MINSIGSTKSZ;
        __atomic_store_n(&cached, min, __ATOMIC_RELAXED);
    }
    return min;
}

void sig_run_onstack(void (*fn)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (!alt.size || on_alt(sp) || alt_cut()) {
        fn(arg);   /* on it (perhaps by a call moved from it): where it is */
        return;
    }
    lx_call_stack_hold(sp);   /* the frames of a call it may leave on the stack for calls */
    __jam_sig_stack_call(fn, arg, (void *)((alt.base + alt.size) & ~(uintptr_t)15));
}

/* ---- the context -------------------------------------------------------------------- */

/* The frame's registers, REG_R8 .. REG_EFL. */
static greg_t *frame_regs(struct exc_frame *f)
{
    return (greg_t *)(void *)((uint8_t *)f + offsetof(struct exc_frame, r8));
}

/* REG_CSGSFS: CS, GS and FS 0, and SS (UC_SIGCONTEXT_SS), as Linux's. */
static uint64_t selectors(void)
{
    uint16_t cs, ss;
    __asm__("mov %%cs, %0" : "=r"(cs));
    __asm__("mov %%ss, %0" : "=r"(ss));
    return cs | (uint64_t)ss << 48;
}

/* The frame's vector state copied to fp, laid out as Linux's signal frame
 * lays it out when the area is more than FXSAVE's: fpx_sw_bytes in the
 * legacy area's tail, FP_XSTATE_MAGIC2 after the area. Its uc_flags. */
static unsigned long fp_copy(uint8_t *fp, const struct exc_frame *f)
{
    memcpy(fp, f->xsave, f->xsave_size);
    if (f->xsave_size == LEGACY_SIZE)
        return 0;
    struct fpx_sw_bytes sw = {
        .magic1 = FP_XSTATE_MAGIC1,
        .extended_size = f->xsave_size + (uint32_t)sizeof(uint32_t),
        .xstate_size = f->xsave_size,
    };
    memcpy(&sw.xfeatures, fp + XSTATE_BV_AT, sizeof(sw.xfeatures));
    memcpy(fp + FPX_SW_AT, &sw, sizeof(sw));
    uint32_t magic2 = FP_XSTATE_MAGIC2;
    memcpy(fp + f->xsave_size, &magic2, sizeof(magic2));
    return UC_FP_XSTATE;
}

/* ---- the fault ---------------------------------------------------------------------- */

/* The kernel's kill, with the line the fault would have had with no entry. */
_Noreturn static void unhandled(struct exc_frame *f, int sig)
{
    (void)jam_exception_done(f, EXC_UNHANDLED);
    jam_process_exit(LX_EXIT_SIGNAL + sig);   /* never: the kill doesn't return */
}

/* Continue from the frame (the kernel's checks refuse a bad one with a kill). */
_Noreturn static void resume(struct exc_frame *f, int sig)
{
    (void)jam_exception_done(f, EXC_RESUME);
    jam_process_exit(LX_EXIT_SIGNAL + sig);   /* never: only a frame it can't read returns */
}

/* The handler runs on the interrupted stack, not on the alternate one it
 * was entered on: the thread has an alternate stack, its RSP wasn't on it,
 * and the handler has no SA_ONSTACK. */
static bool runs_on_interrupted(const struct exc_frame *f, const struct sig_act_io *act)
{
    return alt.size && !on_alt(f->rsp) && !(act->flags & SA_ONSTACK);
}

#define RUN_BYTES ((sizeof(struct fault_run) + 63) & ~(size_t)63)

/* A handler's way in and out: from EXC_LEAVE to the handler, and from its
 * return to the thread's resume (or the next handler), the thread is in
 * the layer, on the way out setting the mask the handler left and looking
 * for the next signal due, then resuming the frame. A raise that comes
 * there runs no handler: the registers it found are the
 * layer's, not where the thread goes on (V8's profiler would take a
 * sample in the layer; Chromium's stack copier would copy the wrong
 * stack), and the signal it brings may have come after the last look for
 * one, so it could wait for ever on a thread that makes no calls. It
 * notes `again` and goes on at once; the way out then takes the signal
 * on the frame it resumes. __jam_sig_resume (sigentry.S) clears `in` and
 * looks at `again` before its system call; a raise inside it, after that
 * look, also has the thread raised again, which comes once the frame is
 * resumed (at its next interrupt). The thread's own, read by its own
 * raise entry. */
struct sig_tail {
    volatile uint8_t in;      /* on a handler's way in or out */
    volatile uint8_t again;   /* a raise came there */
};
_Static_assert(offsetof(struct sig_tail, in) == 0 && offsetof(struct sig_tail, again) == 1,
               "sigentry.S's offsets");
static __thread struct sig_tail tail;

long __jam_sig_resume(struct exc_frame *f, struct sig_tail *t, long outer, long here);
extern const char __jam_sig_resume_end[];

/* Forks this process's memory has been through (sig_fault_forked, in the
 * child): a handler that was running when its thread forked returns, in
 * the child, to a frame the kernel made for the parent's thread, which
 * exception_done refuses; such a frame is resumed in user mode. */
static unsigned forks;

void sig_fault_forked(void)
{
    forks++;
}

/* A raise that found the thread on a handler's way out (in the window). */
static bool raised_on_way_out(const struct exc_frame *f)
{
    bool resuming = f->rip >= (uintptr_t)__jam_sig_resume &&
                    f->rip < (uintptr_t)__jam_sig_resume_end;
    if (sig_on_signal_thread() || (!tail.in && !resuming))
        return false;
    tail.again = 1;
    if (resuming)
        sig_raise_later();
    return true;
}

static volatile uint64_t way_out_ticks;   /* posixtest's wait (calls.h) */

void lx_sig_test_way_out(uint64_t ticks)
{
    way_out_ticks = ticks;
}

/* posixtest's wait on the way out, after the last look for a signal. */
static void way_out_wait(void)
{
    uint64_t ticks = way_out_ticks;
    for (uint64_t t0 = __builtin_ia32_rdtsc(); ticks && __builtin_ia32_rdtsc() - t0 < ticks;)
        __builtin_ia32_pause();
}

/* The way out's last step: f resumed, the thread's `in` as it was before
 * this handler (outer: a fault in a way out goes back into it), by the
 * kernel unless the process forked since f was made (made_at: `forks`
 * then). Returns only if a raise came on the way out: its signal is to be
 * taken first. */
static void way_out(struct exc_frame *f, uint8_t outer, unsigned made_at, int sig)
{
    way_out_wait();
    if (__jam_sig_resume(f, &tail, outer, made_at != forks) > 0) {
        tail.again = 0;
        return;
    }
    jam_process_exit(LX_EXIT_SIGNAL + sig);   /* never: only a frame it can't read returns */
}

_Noreturn void sig_fault(struct exc_frame *f)
{
    if (uc_fault(f))   /* a checked copy's bad pointer: on with -EFAULT (ucopy.c) */
        resume(f, SIGSEGV);
    if ((f->kind != EXC_FAULT && f->kind != EXC_RAISED) || f->xsave_size < LEGACY_SIZE ||
        f->xsave_size > XSAVE_MAX || f->size != EXC_FRAME_XSAVE + f->xsave_size)
        unhandled(f, SIGSEGV);
    struct fault_run run = { .f = f };
    if (f->kind == EXC_RAISED) {
        if (raised_on_way_out(f))
            resume(f, SIGSEGV);   /* the way out takes its signal */
        if (!sig_raise_begin((const uint64_t *)(void *)frame_regs(f), &run.si, &run.act, &run.was))
            resume(f, SIGSEGV);   /* nothing to run here: on at once */
    } else {
        struct fault_sig fs = classify(f);
        if (!fs.sig)
            resume(f, SIGFPE);   /* a spurious x87 or SIMD error: on, as Linux goes on */
        if (!sig_fault_begin(fs.sig, &run.act, &run.was))
            unhandled(f, fs.sig);
        run.si = (struct sig_info){ .signo = fs.sig, .code = fs.code, .addr = fs.addr };
    }
    if (!runs_on_interrupted(f, &run.act))
        sig_fault_run(&run);

    /* The move, still in the window: the run state and a copy of the
     * frame below the interrupted RSP and its red zone, 64-byte aligned,
     * and the rest of the fault on the stack below them. A stack that
     * can't take the copy faults here: the kernel's kill. */
    size_t fsz = ((size_t)f->size + 63) & ~(size_t)63;
    uintptr_t lo = ((uintptr_t)f->rsp - RED_ZONE - RUN_BYTES - fsz) & ~(uintptr_t)63;
    struct fault_run *r = (struct fault_run *)lo;
    struct exc_frame *copy = (struct exc_frame *)(lo + RUN_BYTES);
    memcpy(copy, f, f->size);
    *r = run;
    r->f = copy;
    __jam_sig_moved(copy, r, (void *)lo);
}

/* The handler, the program's code: off the way in and out (struct
 * sig_tail) just for it, so a raise during it runs a handler of its own. */
static void call_handler(void *arg)
{
    struct handler_args *h = arg;
    tail.in = 0;
    if (h->act->flags & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))h->act->handler)(h->sig, &h->ctx->info, &h->ctx->uc);
    else
        ((void (*)(int))h->act->handler)(h->sig);
    tail.in = 1;
}

/* The rest of a fault or a raise whose signal a handler takes: what it is
 * given, the handler, and the thread resumed from what it leaves (after a
 * raise's, the next signal due first). On the stack the handler runs on
 * (the frame's, or the interrupted one: sig_fault). */
_Noreturn void sig_fault_run(struct fault_run *r)
{
    struct exc_frame *f = r->f;
    struct fault_ctx ctx;
    uint8_t *fp = __builtin_alloca(f->xsave_size + sizeof(uint32_t) + 63);
    fp = (uint8_t *)(((uintptr_t)fp + 63) & ~(uintptr_t)63);   /* XSAVE's alignment */
    greg_t *g = ctx.uc.uc_mcontext.gregs;
    bool left = false;
    uint8_t outer = tail.in;   /* 1 only for a fault on a way out (the layer's own) */
    unsigned made_at = forks;
    for (;;) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.uc.uc_flags = UC_SIGCONTEXT_SS | fp_copy(fp, f);
        uintptr_t base;
        size_t size;
        ctx.uc.uc_stack.ss_flags = sig_alt_state((uintptr_t)f->rsp, &base, &size);
        ctx.uc.uc_stack.ss_sp = (void *)base;
        ctx.uc.uc_stack.ss_size = size;
        memcpy(g, frame_regs(f), SIG_REGS * sizeof(greg_t));
        g[REG_CSGSFS] = (greg_t)selectors();
        g[REG_ERR] = (greg_t)f->error;
        g[REG_TRAPNO] = f->vector;
        g[REG_OLDMASK] = (greg_t)r->was;
        g[REG_CR2] = (greg_t)f->addr;
        ctx.uc.uc_mcontext.fpregs = (fpregset_t)(void *)fp;
        memcpy(&ctx.uc.uc_sigmask, &r->was, sizeof(r->was));
        sig_fill_siginfo(&r->si, &ctx.info);

        /* On the way in from EXC_LEAVE to the handler, and out after it
         * (struct sig_tail; call_handler clears `in` for the handler). */
        tail.in = 1;
        if (!left)
            (void)jam_exception_done(f, EXC_LEAVE);
        left = true;
        lx_call_stack_hold(f->rsp);   /* what was interrupted there, if on the stack for calls */
        struct handler_args h = { &r->act, r->si.signo, &ctx };
        if (r->act.flags & SA_ONSTACK)
            sig_run_onstack(call_handler, &h);   /* where it is unless it moved off the stack */
        else
            call_handler(&h);

        /* Back from the handler, on the way out: on as its ucontext says
         * (fpregs as Linux's sigreturn takes it: wherever the handler left
         * it pointing). A raise that came on the way in or out is taken
         * below by its pending bit. */
        tail.again = 0;
        memcpy(frame_regs(f), g, SIG_REGS * sizeof(greg_t));
        if (ctx.uc.uc_mcontext.fpregs)
            memcpy(f->xsave, ctx.uc.uc_mcontext.fpregs, f->xsave_size);
        uint64_t mask;
        memcpy(&mask, &ctx.uc.uc_sigmask, sizeof(mask));
        if (f->kind != EXC_RAISED) {
            for (;;) {   /* what a raise on the way out brought: delivered here too */
                sig_fault_end(mask);
                way_out(f, outer, made_at, r->si.signo);
            }
        }
        while (!sig_raise_next(mask, &r->si, &r->act, &r->was))
            way_out(f, outer, made_at, SIGSEGV);
    }
}
