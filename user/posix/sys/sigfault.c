/* Faults as Linux's signals (docs/SIGNALS-PLAN.md section 1, libc's
 * part): the process's exception entry, the frame the kernel gives it
 * made into a siginfo_t and a ucontext_t, the handler run on the faulting
 * thread, and the thread resumed from the ucontext as the handler left it.
 *
 * The entry (sigentry.S's __jam_sig_entry, which calls sig_fault here) is
 * set only while one of SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP has a
 * handler (sig_entry_need, from sig_action): a program that never asks
 * keeps the kernel's own kill, line for line, with libc nowhere on the
 * path. With it set, a fault whose signal is blocked, ignored or default,
 * on a thread libc doesn't list, or on a thread that holds sig_lock
 * (sig_fault_begin) is EXC_UNHANDLED: the kernel's kill with the line it
 * would have had with no entry.
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
 * SG5 adds a raise of one of our threads (EXC_RAISED frames), marked
 * below. */
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
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
#define FRAME_REGS       18            /* REG_R8 .. REG_EFL: the frame's, in that order */
#define RED_ZONE         128u          /* below RSP: the x86-64 ABI's, leaf code's */
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
               REG_EFL * sizeof(greg_t), "the frame's registers in gregs' order");
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

/* What the rest of a fault needs once the signal is taken (sig_fault_run):
 * kept with the frame's copy below the interrupted RSP when the handler
 * runs there. */
struct fault_run {
    struct exc_frame *f;
    struct fault_sig  fs;
    struct sig_act_io act;   /* the disposition as sig_fault_begin took it */
    uint64_t          was;   /* the mask before the handler */
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
 * thread's is zero (none). The kernel has the same range. */
static __thread struct {
    uintptr_t base;
    size_t    size;
} alt;

/* sp is on the stack: above its base, at most its top (Linux's on_sig_stack
 * and the kernel's own test). */
static bool on_alt(uintptr_t sp)
{
    return alt.size && sp > alt.base && sp - alt.base <= alt.size;
}

int sig_alt_state(uintptr_t sp, uintptr_t *base, size_t *size)
{
    *base = alt.base;
    *size = alt.size;
    return !alt.size ? SS_DISABLE : on_alt(sp) ? SS_ONSTACK : 0;
}

long sig_alt_set(uintptr_t base, size_t size)
{
    if (size == 0)
        base = 0;
    if (jam_thread_exception_stack(base, size) != OK)
        return -EFAULT;
    alt.base = base;
    alt.size = size;
    return 0;
}

void sig_alt_reapply(void)
{
    if (alt.size)
        (void)jam_thread_exception_stack(alt.base, alt.size);
}

/* The bytes of vector state the kernel saves in a frame: the XSAVE area
 * for the features it enables (CPUID leaf 0xD's size, the area as the
 * kernel sizes it), or FXSAVE's 512. */
static size_t xsave_bytes(void)
{
    uint32_t a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    uint32_t top = a;
    a = 1;
    c = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (top < 0xd || !(c & (1u << 27)))   /* no XSAVE, or the OS hasn't enabled it */
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
    if (!alt.size || on_alt((uintptr_t)__builtin_frame_address(0))) {
        fn(arg);
        return;
    }
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

_Noreturn void sig_fault(struct exc_frame *f)
{
    /* SG5: an EXC_RAISED frame (a signal to this thread) goes to its own path. */
    if (f->kind != EXC_FAULT || f->xsave_size < LEGACY_SIZE || f->xsave_size > XSAVE_MAX ||
        f->size != EXC_FRAME_XSAVE + f->xsave_size)
        unhandled(f, SIGSEGV);
    struct fault_run run = { .f = f, .fs = classify(f) };
    if (!run.fs.sig)
        resume(f, SIGFPE);   /* a spurious x87 or SIMD error: on, as Linux goes on */
    if (!sig_fault_begin(run.fs.sig, &run.act, &run.was))
        unhandled(f, run.fs.sig);
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

/* The rest of a fault whose signal a handler takes: what it is given, the
 * handler, and the thread resumed from what it leaves. On the stack the
 * handler runs on (the frame's, or the interrupted one: sig_fault). */
_Noreturn void sig_fault_run(struct fault_run *r)
{
    struct exc_frame *f = r->f;
    int sig = r->fs.sig;
    struct fault_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint8_t *fp = __builtin_alloca(f->xsave_size + sizeof(uint32_t) + 63);
    fp = (uint8_t *)(((uintptr_t)fp + 63) & ~(uintptr_t)63);   /* XSAVE's alignment */
    ctx.uc.uc_flags = UC_SIGCONTEXT_SS | fp_copy(fp, f);
    uintptr_t base;
    size_t size;
    ctx.uc.uc_stack.ss_flags = sig_alt_state((uintptr_t)f->rsp, &base, &size);
    ctx.uc.uc_stack.ss_sp = (void *)base;
    ctx.uc.uc_stack.ss_size = size;
    greg_t *g = ctx.uc.uc_mcontext.gregs;
    memcpy(g, frame_regs(f), FRAME_REGS * sizeof(greg_t));
    g[REG_CSGSFS] = (greg_t)selectors();
    g[REG_ERR] = (greg_t)f->error;
    g[REG_TRAPNO] = f->vector;
    g[REG_OLDMASK] = (greg_t)r->was;
    g[REG_CR2] = (greg_t)f->addr;
    ctx.uc.uc_mcontext.fpregs = (fpregset_t)(void *)fp;
    memcpy(&ctx.uc.uc_sigmask, &r->was, sizeof(r->was));
    struct sig_info si = { .signo = sig, .code = r->fs.code, .addr = r->fs.addr };
    sig_fill_siginfo(&si, &ctx.info);

    (void)jam_exception_done(f, EXC_LEAVE);
    if (r->act.flags & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))r->act.handler)(sig, &ctx.info, &ctx.uc);
    else
        ((void (*)(int))r->act.handler)(sig);

    /* Back from the handler: on as its ucontext says (fpregs as Linux's
     * sigreturn takes it: wherever the handler left it pointing). */
    memcpy(frame_regs(f), g, FRAME_REGS * sizeof(greg_t));
    if (ctx.uc.uc_mcontext.fpregs)
        memcpy(f->xsave, ctx.uc.uc_mcontext.fpregs, f->xsave_size);
    uint64_t mask;
    memcpy(&mask, &ctx.uc.uc_sigmask, sizeof(mask));
    sig_fault_end(mask);
    resume(f, sig);
}
