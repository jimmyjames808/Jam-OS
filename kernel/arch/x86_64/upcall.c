/* Fault upcalls (docs/SIGNALS-PLAN.md section 1): a fault of user code that
 * would kill its process goes to the process's exception entry instead,
 * on the faulting thread. The ABI (struct exc_frame, which faults, the
 * registers at the entry) is in <jam/abi.h>; the calls are
 * exception_entry_set and exception_done (abi/syscalls.def 170, 171).
 *
 * Delivery (user_fault, from trap.c and trap_page_fault where the kill
 * was): the frame is built in a zeroed buffer on the kernel stack (the
 * registers from the trap frame, the XSAVE area from fpu_frame_save), so
 * no kernel byte reaches it, and copied once to the thread's own stack
 * with copy_to_user, interrupts on, from the bottom of the kernel stack,
 * as a kill runs: a stack page is committed as by any touch, and a copy
 * that fails (RSP in nothing writable) is the kill it would have been.
 * The trap frame is then pointed at the entry and the vector registers
 * reset; the caller returns through user_trap_return, so a kill or a
 * reschedule that came meanwhile is still taken.
 *
 * The window: from delivery until exception_done, the thread is in its
 * entry (t->exc_window). A fault there means libc's entry itself failed:
 * the process is killed with the first fault's line, which the thread
 * kept, and "again in its exception entry". It is the only nesting rule
 * the kernel keeps; masks and handlers are libc's.
 *
 * What the kernel trusts from a frame (EXC_RESUME, docs/SIGNALS-PLAN.md
 * section 4): nothing unchecked. The cookie (random per thread: a forged
 * frame needs a read of the stack too), the sizes, RIP below USER_TOP,
 * RFLAGS through USER_RFLAGS_OK, the XSAVE header and MXCSR
 * (fpu_frame_ok); CS, SS, the FS base and any supervisor state never come
 * from it. A frame that fails is a kill of its own process, never a
 * kernel fault.
 *
 * The cost: none on a fault that is served (trap_page_fault returns
 * before any of this) or on a system call; only a fault that would have
 * killed comes here. No kernel memory per upcall: the frame is on the
 * user's stack, the thread keeps a few words (sched.h). */
#include <jam/abi.h>
#include <jam/cpu.h>
#include <jam/irq.h>
#include <jam/process.h>
#include <jam/random.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/syscall_impl.h>
#include <jam/trap.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/x86.h>

#define RFLAGS_RF (1ull << 16)
#define RED_ZONE  128   /* below RSP, the x86-64 ABI's: a leaf function's scratch */
#define PF_PRESENT 1u

/* The vectors a fault of user code is delivered for (<jam/abi.h>). NMI,
 * double fault and machine check never come here; the others can't come
 * from user code on this kernel (#NM: the FPU is always on), or mean
 * nothing a handler could act on. */
#define EXC_VECTORS ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | \
                     (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 16) |      \
                     (1u << 17) | (1u << 19) | (1u << 21))

/* Page-fault statuses that stay kills with an entry (docs/SIGNALS-PLAN.md
 * Q3): a page that doesn't match the approved program (a handler must not
 * look at it or step over it), and out of memory or the job's page limit
 * (the handler would need memory itself). */
static bool status_delivers(status_t s)
{
    return s != ERR_BAD_STATE && s != ERR_NO_MEMORY;
}

/* Where the frame goes below: RSP less the red zone. (SG2: the thread's
 * exception stack, when it has one and RSP is outside it.) */
static uint64_t frame_top(uint64_t rsp)
{
    return rsp - RED_ZONE;
}

void user_fault(struct trap_frame *f, const char *why, uint64_t addr, status_t status)
{
    struct thread *t = current_thread();
    uint64_t rip = f->rip;
    irq_enable();
    if (!t->process || f->vector >= 32 || !(EXC_VECTORS >> f->vector & 1) ||
        !status_delivers(status))
        user_kill(why, rip, addr, NULL);
    if (t->exc_window)
        user_kill(t->exc_why, t->exc_rip, t->exc_addr, ", again in its exception entry");
    uint64_t entry = process_exc_entry(t->process);
    uint32_t xsize = fpu_frame_size();
    if (!entry || !xsize)
        user_kill(why, rip, addr, NULL);
    uint64_t size = EXC_FRAME_XSAVE + xsize;
    uint64_t top = frame_top(f->rsp);
    if (f->rsp > USER_TOP || f->rsp < USER_BASE + RED_ZONE + size + 64 + 8)
        user_kill(why, rip, addr, NULL);   /* nowhere to put it */
    uint64_t at = (top - size) & ~63ull;

    if (!t->exc_cookie) {
        uint64_t c;
        do
            c = random_u64();
        while (!c);
        t->exc_cookie = c;
    }
    /* out: the entry's return address (0), then the frame. */
    uint8_t out[8 + EXC_FRAME_XSAVE + FPU_FRAME_MAX];
    memset(out, 0, 8 + size);
    struct exc_frame fr = {
        .size = (uint32_t)size, .kind = EXC_FAULT, .vector = (uint32_t)f->vector,
        .status = f->vector == 14 ? status : 0, .error = f->error,
        .addr = f->vector == 14 ? addr : 0, .cookie = t->exc_cookie, .xsave_size = xsize,
        .r8 = f->r8, .r9 = f->r9, .r10 = f->r10, .r11 = f->r11,
        .r12 = f->r12, .r13 = f->r13, .r14 = f->r14, .r15 = f->r15,
        .rdi = f->rdi, .rsi = f->rsi, .rbp = f->rbp, .rbx = f->rbx,
        .rdx = f->rdx, .rax = f->rax, .rcx = f->rcx, .rsp = f->rsp,
        .rip = f->rip, .rflags = f->rflags,
    };
    if (f->vector == 14 && addr >= USER_TOP)
        fr.error |= PF_PRESENT;   /* never tell which kernel pages are mapped */
    memcpy(out + 8, &fr, sizeof(fr));
    irq_disable();
    fpu_frame_save(t, out + 8 + EXC_FRAME_XSAVE);
    irq_enable();
    status_t st = copy_to_user(at - 8, out, 8 + size);
    if (st != OK) {
        if (thread_cancel_pending()) {
            /* Killed while the copy waited: leave with no fault of its
             * own to report, as trap_page_fault does. */
            irq_disable();
            return_to_user_work();
        }
        user_kill(why, rip, addr, NULL);
    }

    t->exc_window = true;
    t->exc_why = why;
    t->exc_rip = rip;
    t->exc_addr = addr;
    f->rip = entry;
    f->rsp = at - 8;
    f->rdi = at;
    f->rflags &= ~(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF | RFLAGS_AC);
    irq_disable();
    fpu_reset_and_load(t);   /* the entry starts from the initial vector state */
}

int64_t sysc_exception_entry_set(uint64_t entry)
{
    if (entry >= USER_TOP)
        return ERR_INVALID_ARGS;
    struct process *p = current_thread()->process;
    if (!p)
        return ERR_BAD_STATE;   /* the kernel's own ring-3 test threads */
    process_set_exc_entry(p, entry);
    return OK;
}

/* EXC_RESUME: the frame at uframe, read once, checked, then loaded and
 * returned to; a frame that fails a check is a kill. */
static int64_t exc_resume(struct thread *t, uint64_t uframe)
{
    uint32_t xsize = fpu_frame_size();
    uint64_t size = EXC_FRAME_XSAVE + xsize;
    uint8_t in[EXC_FRAME_XSAVE + FPU_FRAME_MAX];
    if (!xsize || copy_from_user(in, uframe, size) != OK)
        return ERR_INVALID_ARGS;
    struct exc_frame fr;
    memcpy(&fr, in, sizeof(fr));
    const char *bad = NULL;
    if (!t->exc_cookie || fr.cookie != t->exc_cookie)
        bad = "exception_done: a frame that isn't this thread's";
    else if (fr.size != size || fr.xsave_size != xsize)
        bad = "exception_done: a frame of the wrong size";
    else if (fr.rip >= USER_TOP)
        bad = "exception_done: a frame with a bad address";
    else if (!fpu_frame_ok(in + EXC_FRAME_XSAVE))
        bad = "exception_done: a frame with a bad vector state";
    if (bad)
        user_kill(bad, fr.rip, uframe, NULL);

    struct trap_frame tf = {
        .r15 = fr.r15, .r14 = fr.r14, .r13 = fr.r13, .r12 = fr.r12,
        .r11 = fr.r11, .r10 = fr.r10, .r9 = fr.r9, .r8 = fr.r8,
        .rbp = fr.rbp, .rdi = fr.rdi, .rsi = fr.rsi, .rdx = fr.rdx,
        .rcx = fr.rcx, .rbx = fr.rbx, .rax = fr.rax,
        .rip = fr.rip, .cs = GDT_USER_CODE | 3,
        .rflags = (fr.rflags & USER_RFLAGS_OK) | RFLAGS_IF | 2,
        .rsp = fr.rsp, .ss = GDT_USER_DATA | 3,
    };
    irq_disable();
    return_to_user_work();   /* a switch in here is still inside the call */
    t->in_syscall = false;   /* its registers are live again (fpu.c), from the frame */
    t->exc_window = false;
    fpu_frame_load(t, in + EXC_FRAME_XSAVE);
    user_resume_iret(&tf);
}

int64_t sysc_exception_done(uint64_t frame, uint32_t how)
{
    struct thread *t = current_thread();
    switch (how) {
    case EXC_RESUME:
        return exc_resume(t, frame);
    case EXC_LEAVE:
        if (!t->exc_window)
            return ERR_BAD_STATE;
        t->exc_window = false;
        return OK;
    case EXC_UNHANDLED:
        if (!t->exc_window)
            return ERR_BAD_STATE;
        user_kill(t->exc_why, t->exc_rip, t->exc_addr, NULL);
    default:
        return ERR_INVALID_ARGS;
    }
}
