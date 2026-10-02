/* Ring 3: syscall setup, the C side of the syscall and trap returns, user
 * page faults and user copies, killing a faulting user thread, the first
 * entry into ring 3, and the per-switch state (TSS rsp0, FPU, address
 * space). The assembly halves are syscall.S, isr.S and usercopy.S.
 *
 * Invariant: while a thread runs in ring 3 its kernel stack is empty, it
 * holds no locks, and this CPU's TSS rsp0 and kernel_rsp both point at the
 * top of that stack (arch_thread_switch keeps them in step). */
#include <stddef.h>
#include <jam/aspace.h>
#include <jam/cpu.h>
#include <jam/entry_asm.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/syscall.h>
#include <jam/trap.h>
#include <jam/uentry.h>
#include <jam/uentry_test.h>
#include <jam/usercopy.h>
#include <jam/x86.h>

_Static_assert(offsetof(struct cpu, user_rsp) == PERCPU_USER_RSP, "entry_asm.h");
_Static_assert(offsetof(struct cpu, kernel_rsp) == PERCPU_KERNEL_RSP, "entry_asm.h");
_Static_assert(offsetof(struct syscall_frame, args) == 8, "syscall.S");
_Static_assert(offsetof(struct syscall_frame, user_rip) == 56, "syscall.S");
_Static_assert(offsetof(struct syscall_frame, user_rsp) == 72, "syscall.S");
_Static_assert(offsetof(struct syscall_frame, r15) == 120, "syscall.S");
_Static_assert(sizeof(struct syscall_frame) % 16 == 0, "keeps the C call 16-byte aligned");
_Static_assert(GDT_USER_DATA == GDT_USER_BASE + 8 && GDT_USER_CODE == GDT_USER_BASE + 16,
               "SYSRET selector layout (and the constants in syscall.S)");

void syscall_entry(void);
_Noreturn void enter_user_iret(uint64_t entry, uint64_t stack, uint64_t arg0, uint64_t arg1,
                               uint64_t kstack_top);
size_t copy_user_raw(void *dst, const void *src, size_t n);
long copy_str_user_raw(char *dst, const char *usrc, size_t n);
void fpu_save(void *area);

#ifndef JAM_NO_KTESTS
bool (*uentry_test_syscall)(struct syscall_frame *f, int64_t *ret);
uint64_t (*uentry_test_cr3)(struct thread *t);
bool (*uentry_test_nmi)(struct trap_frame *f);
#endif

/* RFLAGS bits user code may carry back through sysret. IOPL, NT, RF, VM
 * and friends are never the user's to set; IF is always on. */
#define USER_RFLAGS_OK (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | \
                        RFLAGS_TF | RFLAGS_DF | RFLAGS_OF | RFLAGS_AC | RFLAGS_ID)

uint64_t user_faults;   /* user threads killed for a fault (atomic) */

void syscall_init_cpu(void)
{
    /* SYSCALL: CS = STAR[47:32], SS = +8. SYSRET (64-bit): CS =
     * STAR[63:48] + 16, SS = +8, both with RPL 3. CSTAR (syscall from
     * 32-bit code) stays unset: there is no 32-bit user code segment. */
    wrmsr(MSR_STAR, (uint64_t)GDT_USER_BASE << 48 | (uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_AC | RFLAGS_NT);
    /* sysenter is legal in 64-bit mode on Intel; a zero SYSENTER_CS makes
     * it #GP in user mode instead of entering wherever firmware left it. */
    wrmsr(MSR_SYSENTER_CS, 0);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

static uint32_t preempt_count_now(void)
{
    uint32_t v;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(v) : "i"(PERCPU_OFF(preempt_count)));
    return v;
}

/* ---- leaving the kernel for ring 3 ---------------------------------------- */

/* With interrupts off, just before returning to ring 3 (from a syscall, an
 * interrupt or a resolved fault): take a pending reschedule, and exit if
 * the thread was cancelled. Returns with interrupts off and nothing left
 * to do, so the check can't go stale before the return: an IPI arriving
 * now is taken in user mode and comes straight back through here. */
/* The current thread leaves ring 3 for good: its process is dying (or,
 * for the kernel's own ring-3 test threads, which have no process, just
 * the thread). Nothing held; interrupts on. */
_Noreturn static void leave_user(void)
{
    if (current_thread()->process)
        uthread_exit_current();
    thread_exit();
}

static void return_to_user_work(void)
{
    for (;;) {
        /* A cancelled user thread's process is being killed (process_kill):
         * the thread leaves here instead of returning to user code. */
        if (thread_cancel_pending()) {
            irq_enable();
            leave_user();
        }
        struct cpu *c = this_cpu();
        if (c->preempt_count || c->held_depth || c->irq_depth) {
            lockdep_print_held();
            panic("returning to user mode with preempt_count %u, %u spinlock(s) held, "
                  "irq depth %u", c->preempt_count, c->held_depth, c->irq_depth);
        }
        if (!cpu_need_resched(c))
            break;
        schedule();   /* returns with interrupts off again */
    }
    struct thread *t = current_thread();
    if (t->sleep_depth)
        panic("thread \"%s\" returning to user mode holding %u mutex(es)", t->name,
              t->sleep_depth);
}

/* A user thread did something fatal: its whole process is killed (there
 * are no exception channels). Runs on its kernel stack at the bottom (nothing
 * of the kernel's below it, no locks), so leaving from here is like leaving
 * from a syscall. */
_Noreturn static void kill_current(const char *why, uint64_t rip, uint64_t addr)
{
    struct thread *t = current_thread();
    irq_enable();
    if (preempt_count_now())
        panic("user fault (%s) in \"%s\" with preemption disabled", why, t->name);
    __atomic_add_fetch(&user_faults, 1, __ATOMIC_RELAXED);
    if (t->process) {
        kprintf("user: process \"%s\" killed: %s at rip %lx, address %lx (thread \"%s\")\n",
                process_name(t->process), why, rip, addr, t->name);
        process_kill(t->process, PROCESS_KILLED_CODE, true);
    } else {
        kprintf("user: killed thread \"%s\" (id %lu): %s at rip %lx, address %lx\n", t->name,
                t->id, why, rip, addr);
    }
    leave_user();
}

_Noreturn void user_fault_kill(struct trap_frame *f, const char *why)
{
    if (!(f->cs & 3))
        panic("user_fault_kill (%s) for a kernel-mode frame at %lx", why, f->rip);
    kill_current(why, f->rip, f->vector == 14 ? read_cr2() : 0);
}

/* The C half of syscall_entry. Interrupts are off on entry and on return;
 * the returned value goes to user rax. */
int64_t syscall_entry_c(struct syscall_frame *f)
{
    PATH_SYSCALL_NR(f->nr);
    PATH_MARK_ARG(PATH_MK_SYS_ENTER, f->nr);
    irq_enable();
    int64_t r;
#ifndef JAM_NO_KTESTS
    bool (*h)(struct syscall_frame *, int64_t *) =
        __atomic_load_n(&uentry_test_syscall, __ATOMIC_ACQUIRE);
    if (!h || !h(f, &r))
#endif
        r = syscall_dispatch(f);
    irq_disable();
    return_to_user_work();
    /* The CPU put a canonical address in rcx, but the frame may have been
     * changed since. sysretq to a non-canonical RIP faults in ring 0 with
     * the user's RSP and GS loaded; nothing legitimate returns at or above
     * USER_TOP (that page is never mapped). */
    if (f->user_rip >= USER_TOP)
        kill_current("sysret to a bad address", f->user_rip, 0);
    f->user_rflags = (f->user_rflags & USER_RFLAGS_OK) | RFLAGS_IF | 2;
    PATH_MARK_ARG(PATH_MK_SYS_EXIT, f->nr);
    return r;
}

/* trap_dispatch, for a trap from ring 3 that returns there (not NMI/#MC).
 * Interrupts off. */
void user_trap_return(struct trap_frame *f)
{
    return_to_user_work();
    /* iretq to a non-canonical RIP would #GP in ring 0 after the swapgs. */
    if (f->rip >= USER_TOP || f->cs != (GDT_USER_CODE | 3) || f->ss != (GDT_USER_DATA | 3))
        kill_current("iret to a bad frame", f->rip, 0);
}

/* ---- page faults and user copies ------------------------------------------- */

struct ex_entry {
    uint64_t insn, fixup;   /* a copy instruction that may fault; where to resume if it does */
};
extern const struct ex_entry __ex_table_start[], __ex_table_end[];

static const struct ex_entry *ex_find(uint64_t rip)
{
    for (const struct ex_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (e->insn == rip)
            return e;
    return NULL;
}

#define PF_PRESENT (1u << 0)
#define PF_WRITE   (1u << 1)
#define PF_USER    (1u << 2)
#define PF_RSVD    (1u << 3)
#define PF_FETCH   (1u << 4)

bool trap_page_fault(struct trap_frame *f)
{
    uint64_t addr = read_cr2();   /* before anything can fault or sleep */
    bool from_user = f->cs & 3;
    const struct ex_entry *fix = from_user ? NULL : ex_find(f->rip);
    if (!from_user && !fix)
        return false;   /* a real kernel fault: panic as always */
    if (f->error & PF_RSVD)
        return false;   /* corrupt page tables are the kernel's bug */
    bool uaddr = addr >= USER_BASE && addr < USER_TOP;
    if (fix && !uaddr)
        return false;   /* a copy routine faulted on its kernel-side buffer */
    if (fix && !(f->rflags & RFLAGS_IF))
        panic("user copy faulted with interrupts off (at %lx, address %lx)", f->rip, addr);

    struct thread *t = current_thread();
    if (uaddr && t->aspace) {
        unsigned access = (f->error & PF_FETCH) ? ASPACE_EXEC
                        : (f->error & PF_WRITE) ? ASPACE_WRITE : ASPACE_READ;
        /* The interrupted code ran with interrupts on (user mode, or a user
         * copy), on this thread's stack with no spinlock: it may sleep. */
        irq_enable();
        status_t s = aspace_fault(t->aspace, addr, access);
        irq_disable();
        if (s == OK)
            return true;   /* retry the access */
        if (!fix && s == ERR_NO_MEMORY)
            kill_current("page fault: out of memory (or the job's page limit)", f->rip, addr);
        if (!fix && s == ERR_OUT_OF_RANGE)
            kill_current("bus error: page past the end of a shrunk VMO", f->rip, addr);
    }
    if (fix) {
        f->rip = fix->fixup;
        return true;
    }
    kill_current("page fault", f->rip, addr);
}

/* The copies may sleep (a page fault commits a page), so they need a
 * context that can: interrupts on and no spinlock held. */
static void check_copy_context(const char *fn)
{
    if (!irqs_enabled() || preempt_count_now())
        panic("%s with interrupts off or preemption disabled (it may sleep on a fault)", fn);
}

status_t copy_from_user(void *dst, uint64_t usrc, size_t n)
{
    check_copy_context("copy_from_user");
    if (!user_range_ok(usrc, n))
        return ERR_INVALID_ARGS;
    if (!n)
        return OK;
    PATH_COUNT(PATH_UCOPY_IN);
    PATH_ADD(PATH_UCOPY_IN_B, n);
    return copy_user_raw(dst, (const void *)usrc, n) ? ERR_INVALID_ARGS : OK;
}

status_t copy_to_user(uint64_t udst, const void *src, size_t n)
{
    check_copy_context("copy_to_user");
    if (!user_range_ok(udst, n))
        return ERR_INVALID_ARGS;
    if (!n)
        return OK;
    PATH_COUNT(PATH_UCOPY_OUT);
    PATH_ADD(PATH_UCOPY_OUT_B, n);
    return copy_user_raw((void *)udst, src, n) ? ERR_INVALID_ARGS : OK;
}

status_t copy_str_from_user(char *dst, uint64_t usrc, size_t cap, size_t *len)
{
    check_copy_context("copy_str_from_user");
    if (!cap || usrc < USER_BASE || usrc >= USER_TOP)
        return ERR_INVALID_ARGS;
    /* Never read past the user range: a string running into USER_TOP is a
     * bad pointer, not a long string. */
    size_t n = cap;
    bool clipped = n > USER_TOP - usrc;
    if (clipped)
        n = USER_TOP - usrc;
    long r = copy_str_user_raw(dst, (const char *)usrc, n);
    if (r >= 0) {
        if (len)
            *len = (size_t)r;
        return OK;
    }
    dst[(r == -1 ? n : 1) - 1] = '\0';   /* never hand back an unterminated buffer */
    if (r == -1 && !clipped)
        return ERR_OUT_OF_RANGE;
    return ERR_INVALID_ARGS;
}

/* ---- switching threads, entering ring 3 ------------------------------------ */

void arch_thread_switch(struct thread *prev, struct thread *next)
{
    struct cpu *c = this_cpu();
    /* Only matters for threads that enter ring 3, but kernel threads have
     * a stack too (thread "main" has none it knows of: leave the old). */
    if (next->stack_top) {
        c->tss.rsp[0] = (uint64_t)next->stack_top;
        c->kernel_rsp = (uint64_t)next->stack_top;
    }
    if (prev->ustate && thread_state(prev) != T_DEAD) {
        PATH_SW_COUNT(prev, next, PATH_FPU_SAVE);
        fpu_save(prev->ustate);
    }
    if (next->ustate)
        fpu_load(next);   /* skipped if this CPU still holds its state (fpu.c) */
    PATH_SW_MARK(prev, next, PATH_MK_ARCH_FPU);
    if (prev->aspace != next->aspace)
        PATH_SW_COUNT(prev, next, PATH_CR3);
    aspace_switch(prev->aspace, next->aspace);
    PATH_SW_MARK(prev, next, PATH_MK_ARCH_DONE);
#ifndef JAM_NO_KTESTS
    /* The test hook returns the CR3 to load for `next` (its own test tables,
     * or the kernel's when this CPU is leaving a test thread), or 0 to leave
     * CR3 alone. It tracks the per-CPU state itself, so the choice never
     * depends on prev, which a joiner may already be tearing down. */
    uint64_t (*h)(struct thread *) = __atomic_load_n(&uentry_test_cr3, __ATOMIC_ACQUIRE);
    if (h) {
        uint64_t want = h(next);
        if (want && read_cr3() != want)
            write_cr3(want);
    }
#endif
}

_Noreturn void arch_enter_user(uint64_t entry, uint64_t stack, uint64_t arg0, uint64_t arg1)
{
    struct thread *t = current_thread();
    if (!t->ustate || !t->stack_top)
        panic("arch_enter_user: thread \"%s\" has no %s", t->name,
              t->ustate ? "kernel stack" : "user FPU area");
    irq_disable();
    return_to_user_work();
    if (entry >= USER_TOP)
        kill_current("entry at a bad address", entry, 0);
    struct cpu *c = this_cpu();
    c->tss.rsp[0] = (uint64_t)t->stack_top;
    c->kernel_rsp = (uint64_t)t->stack_top;
    fpu_reset_and_load(t);
    /* No TLS yet (FS is not saved per thread). The user GS base sits in
     * KERNEL_GS_BASE until the swapgs; keep both zero. */
    wrmsr(MSR_FS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    enter_user_iret(entry, stack, arg0, arg1, (uint64_t)t->stack_top);
}
