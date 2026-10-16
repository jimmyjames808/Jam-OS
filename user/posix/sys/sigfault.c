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
 * that holds sig_lock
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
 * SG4 adds sigaltstack (the kernel's exception stack: uc_stack, and the
 * move to the interrupted stack for a handler without SA_ONSTACK) and SG5
 * a raise of one of our threads (EXC_RAISED frames): both are marked
 * below. */
#include <stddef.h>
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

void           __jam_sig_entry(void);   /* sigentry.S */
_Noreturn void sig_fault(struct exc_frame *f);

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

_Noreturn void sig_fault(struct exc_frame *f)
{
    if (uc_fault(f))   /* a checked copy's bad pointer: on with -EFAULT (ucopy.c) */
        resume(f, SIGSEGV);
    /* SG5: an EXC_RAISED frame (a signal to this thread) goes to its own path. */
    if (f->kind != EXC_FAULT || f->xsave_size < LEGACY_SIZE || f->xsave_size > XSAVE_MAX ||
        f->size != EXC_FRAME_XSAVE + f->xsave_size)
        unhandled(f, SIGSEGV);
    struct fault_sig fs = classify(f);
    if (!fs.sig)
        resume(f, SIGFPE);   /* a spurious x87 or SIMD error: on, as Linux goes on */
    struct sig_act_io act;
    uint64_t was;
    if (!sig_fault_begin(fs.sig, &act, &was))
        unhandled(f, fs.sig);

    struct fault_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint8_t *fp = __builtin_alloca(f->xsave_size + sizeof(uint32_t) + 63);
    fp = (uint8_t *)(((uintptr_t)fp + 63) & ~(uintptr_t)63);   /* XSAVE's alignment */
    ctx.uc.uc_flags = UC_SIGCONTEXT_SS | fp_copy(fp, f);
    ctx.uc.uc_stack.ss_flags = SS_DISABLE;   /* SG4: the alternate stack, if one is set */
    greg_t *g = ctx.uc.uc_mcontext.gregs;
    memcpy(g, frame_regs(f), FRAME_REGS * sizeof(greg_t));
    g[REG_CSGSFS] = (greg_t)selectors();
    g[REG_ERR] = (greg_t)f->error;
    g[REG_TRAPNO] = f->vector;
    g[REG_OLDMASK] = (greg_t)was;
    g[REG_CR2] = (greg_t)f->addr;
    ctx.uc.uc_mcontext.fpregs = (fpregset_t)(void *)fp;
    memcpy(&ctx.uc.uc_sigmask, &was, sizeof(was));
    struct sig_info si = { .signo = fs.sig, .code = fs.code, .addr = fs.addr };
    sig_fill_siginfo(&si, &ctx.info);

    /* SG4: a handler without SA_ONSTACK, entered on the exception stack,
     * gets ctx moved to the interrupted stack here, still in the window. */
    (void)jam_exception_done(f, EXC_LEAVE);
    if (act.flags & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))act.handler)(fs.sig, &ctx.info, &ctx.uc);
    else
        ((void (*)(int))act.handler)(fs.sig);

    /* Back from the handler: on as its ucontext says (fpregs as Linux's
     * sigreturn takes it: wherever the handler left it pointing). */
    memcpy(frame_regs(f), g, FRAME_REGS * sizeof(greg_t));
    if (ctx.uc.uc_mcontext.fpregs)
        memcpy(f->xsave, ctx.uc.uc_mcontext.fpregs, f->xsave_size);
    uint64_t mask;
    memcpy(&mask, &ctx.uc.uc_sigmask, sizeof(mask));
    sig_fault_end(mask);
    resume(f, fs.sig);
}
