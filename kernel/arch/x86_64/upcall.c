/* Fault upcalls (docs/SIGNALS-PLAN.md sections 1 and 2): a fault of user
 * code that would kill its process goes to the process's exception entry
 * instead, on the faulting thread; and a raise (thread_raise) makes a
 * thread enter it where it was interrupted. The ABI (struct exc_frame,
 * which faults, the registers at the entry, the exception stack, raises)
 * is in <jam/abi.h>; the calls are exception_entry_set, exception_done,
 * thread_exception_stack and thread_raise (abi/syscalls.def 170 to 173).
 *
 * Delivery (user_fault, from trap.c and trap_page_fault where the kill
 * was; user_raise, from user_trap_return): the frame is built in a zeroed
 * buffer on the kernel stack (the registers from the trap frame, the XSAVE
 * area from fpu_frame_save), so no kernel byte reaches it, and copied once
 * to the thread's own stack, or its exception stack, with copy_to_user,
 * interrupts on, from the bottom of the kernel stack, as a kill runs: a
 * stack page is committed as by any touch, and a copy that fails (nothing
 * writable there) is the kill it would have been (a raise: dropped). The
 * trap frame is then pointed at the entry and the vector registers reset;
 * the caller returns through user_trap_return, so a kill or a reschedule
 * that came meanwhile is still taken.
 *
 * Raises: thread_raise (sched/wait.c) latches t->raise_pending and
 * interrupts the CPU t runs on in ring 3. user_trap_return, the return to
 * ring 3 from an interrupt or exception, tests the latch (one load: the
 * only cost on that path) and comes here; the system call return never
 * looks. A raise inside the window stays latched until the window ends
 * (EXC_LEAVE or EXC_RESUME) and the thread's next interrupt.
 *
 * The window: from delivery until exception_done, the thread is in its
 * entry (t->exc_window). A fault there means libc's entry itself failed:
 * the process is killed with the first fault's line, which the thread
 * kept, and "again in its exception entry" (in a raise's window, with its
 * own line and "in its exception entry"). It is the only nesting rule the
 * kernel keeps; masks and handlers are libc's.
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
 * killed comes here, and a raise. No kernel memory per upcall: the frame
 * is on the user's stack, the thread keeps a few words (sched.h). */
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

/* Where a frame of `size` bytes goes for thread t at rsp, 64-byte
 * aligned with its return address's 8 bytes below it: below RSP less the
 * red zone; or, with an exception stack, at its top while RSP is outside
 * it, and below RSP less the red zone while RSP is inside it (a handler
 * running there: Linux's on_sig_stack), the frame then staying on it.
 * False: nowhere (the caller's kill, or a dropped raise). */
static bool frame_place(const struct thread *t, uint64_t rsp, uint64_t size, uint64_t *at)
{
    uint64_t base = t->exc_stack, len = t->exc_stack_size, top;
    if (len && !(rsp > base && rsp - base <= len))
        top = base + len;
    else if (rsp > USER_TOP || rsp < USER_BASE + RED_ZONE)
        return false;
    else
        top = rsp - RED_ZONE;
    if (top < USER_BASE + size + 64 + 8)
        return false;
    *at = (top - size) & ~63ull;
    return !len || *at - 8 >= base;   /* with an exception stack, the frame is on it */
}

/* Write the frame for the thread interrupted at f (kind, and what came:
 * vector, error code, status, address) where frame_place says. Interrupts
 * on. Returns its address, 0 if it couldn't be placed or written (f
 * untouched). */
static uint64_t frame_write(struct thread *t, const struct trap_frame *f, uint32_t kind,
                            uint32_t vector, uint64_t error, status_t status, uint64_t addr)
{
    uint32_t xsize = fpu_frame_size();
    uint64_t size = EXC_FRAME_XSAVE + xsize, at;
    if (!frame_place(t, f->rsp, size, &at))
        return 0;   /* nowhere to put it */

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
        .size = (uint32_t)size, .kind = kind, .vector = vector,
        .status = vector == 14 ? status : 0, .error = error,
        .addr = vector == 14 ? addr : 0, .cookie = t->exc_cookie, .xsave_size = xsize,
        .r8 = f->r8, .r9 = f->r9, .r10 = f->r10, .r11 = f->r11,
        .r12 = f->r12, .r13 = f->r13, .r14 = f->r14, .r15 = f->r15,
        .rdi = f->rdi, .rsi = f->rsi, .rbp = f->rbp, .rbx = f->rbx,
        .rdx = f->rdx, .rax = f->rax, .rcx = f->rcx, .rsp = f->rsp,
        .rip = f->rip, .rflags = f->rflags,
    };
    if (vector == 14 && addr >= USER_TOP)
        fr.error |= PF_PRESENT;   /* never tell which kernel pages are mapped */
    memcpy(out + 8, &fr, sizeof(fr));
    irq_disable();
    fpu_frame_save(t, out + 8 + EXC_FRAME_XSAVE);
    irq_enable();
    return copy_to_user(at - 8, out, 8 + size) == OK ? at : 0;
}

/* Enter the entry with the frame at `at`. Returns with interrupts off. */
static void frame_enter(struct thread *t, struct trap_frame *f, uint64_t entry, uint64_t at)
{
    f->rip = entry;
    f->rsp = at - 8;
    f->rdi = at;
    f->rflags &= ~(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF | RFLAGS_AC);
    irq_disable();
    fpu_reset_and_load(t);   /* the entry starts from the initial vector state */
}

void user_fault(struct trap_frame *f, const char *why, uint64_t addr, status_t status)
{
    struct thread *t = current_thread();
    uint64_t rip = f->rip;
    uint32_t vec = (uint32_t)f->vector;
    irq_enable();
    if (!t->process || vec >= 32 || !(EXC_VECTORS >> vec & 1) || !status_delivers(status))
        user_kill(why, rip, addr, NULL, vec, status);
    if (t->exc_window) {
        if (t->exc_why)
            user_kill(t->exc_why, t->exc_rip, t->exc_addr, ", again in its exception entry",
                      t->exc_vector, t->exc_status);
        user_kill(why, rip, addr, ", in its exception entry", vec, status);   /* a raise's */
    }
    uint64_t entry = process_exc_entry(t->process);
    if (!entry || !fpu_frame_size())
        user_kill(why, rip, addr, NULL, vec, status);
    uint64_t at = frame_write(t, f, EXC_FAULT, vec, f->error, status, addr);
    if (!at) {
        if (thread_cancel_pending()) {
            /* Killed while the copy waited: leave with no fault of its
             * own to report, as trap_page_fault does. */
            irq_disable();
            return_to_user_work();
        }
        user_kill(why, rip, addr, NULL, vec, status);
    }

    t->exc_window = true;
    t->exc_why = why;
    t->exc_rip = rip;
    t->exc_addr = addr;
    t->exc_vector = vec;
    t->exc_status = status;
    frame_enter(t, f, entry, at);
}

void user_raise(struct trap_frame *f)
{
    struct thread *t = current_thread();
    if (t->exc_window)
        return;   /* latched still: after EXC_LEAVE or EXC_RESUME, at a later interrupt */
    /* Taken before the frame is built: a raise from now on latches anew,
     * and one before it is this frame's (acquire: what its raiser wrote
     * first is seen by the entry). */
    (void)__atomic_exchange_n(&t->raise_pending, false, __ATOMIC_ACQ_REL);
    uint64_t entry = t->process ? process_exc_entry(t->process) : 0;
    if (!entry || !fpu_frame_size())
        return;   /* dropped: no entry to take it */
    irq_enable();
    uint64_t at = frame_write(t, f, EXC_RAISED, 0, 0, OK, 0);
    if (at) {
        t->exc_window = true;
        t->exc_why = NULL;   /* a raise's window: a fault in it has its own line */
        t->exc_rip = f->rip;
        t->exc_addr = 0;
        t->exc_vector = 0;
        t->exc_status = OK;
        frame_enter(t, f, entry, at);
    } else {
        irq_disable();   /* dropped: the thread goes on (libc's signal stays pending) */
    }
    return_to_user_work();   /* a kill or a reschedule that came meanwhile */
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

int64_t sysc_thread_exception_stack(uint64_t base, uint64_t size)
{
    struct thread *t = current_thread();
    if (!t->ustate)
        return ERR_BAD_STATE;   /* a kernel thread: it has no ring 3 */
    if (size ? !user_range_ok(base, size) : base != 0)
        return ERR_INVALID_ARGS;
    /* Read only by the thread itself (frame_place), so plain stores. */
    t->exc_stack = base;
    t->exc_stack_size = size;
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
        user_kill(bad, fr.rip, uframe, NULL, 13, OK);   /* Linux's SIGSEGV for a bad sigreturn */

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
        /* A raise's window has no kill to fall back on: resume or leave. */
        if (!t->exc_window || !t->exc_why)
            return ERR_BAD_STATE;
        user_kill(t->exc_why, t->exc_rip, t->exc_addr, NULL, t->exc_vector, t->exc_status);
    default:
        return ERR_INVALID_ARGS;
    }
}
