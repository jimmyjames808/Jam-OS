/* trap_dispatch: where every interrupt and exception from isr.S lands.
 *
 * Vectors 32 and up are interrupts (irq_dispatch); below 32 are CPU
 * exceptions. A page fault may be demand paging or a user copy, handled
 * by trap_page_fault. Any other exception kills the user thread that
 * caused it, or panics if it happened in the kernel. */
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/trap.h>
#include <jam/uentry.h>
#include <jam/uentry_test.h>

void user_trap_return(struct trap_frame *f);

static const char *const user_fault_names[32] = {
    [0] = "divide error", [1] = "debug trap", [3] = "breakpoint", [4] = "overflow",
    [5] = "bound range", [6] = "invalid opcode", [7] = "device not available",
    [10] = "invalid TSS", [11] = "segment not present", [12] = "stack fault",
    [13] = "general protection fault", [14] = "page fault", [16] = "x87 FP error",
    [17] = "alignment check", [19] = "SIMD FP error", [21] = "control protection",
};

void trap_dispatch(struct trap_frame *f)
{
    bool from_user = f->cs & 3;
    if (f->vector >= 32) {
        struct cpu *c = this_cpu();
        PATH_COUNT(PATH_IRQ);
        c->irq_depth++;
        irq_dispatch(f);
        c->irq_depth--;
        sched_irq_exit(f->rflags);   /* may switch threads before iretq */
        if (from_user)
            user_trap_return(f);
        return;
    }
    if (f->vector != 2)
        PATH_COUNT(PATH_TRAP);   /* not an NMI: it may land anywhere */
    switch (f->vector) {
    case 2:
        /* Never schedules or touches the user return path: NMIs stay
         * blocked until this handler's iretq. */
#ifndef JAM_NO_KTESTS
        bool (*h)(struct trap_frame *) = __atomic_load_n(&uentry_test_nmi, __ATOMIC_ACQUIRE);
        if (h && h(f))
            return;
#endif
        nmi_handler(f);
        return;
    case 3:   /* int3: log and continue, handy for testing the trap path */
        if (from_user)
            break;
        kprintf("trap: breakpoint at %lx\n", f->rip);
        return;
    case 8:
    case 18:
        panic_trap(f);   /* double fault, machine check: fatal wherever they hit */
    case 14:
        if (trap_page_fault(f)) {   /* demand paging, user copies, user faults */
            if (from_user)
                user_trap_return(f);
            return;
        }
        panic_trap(f);
    }
    if (from_user) {
        const char *name = user_fault_names[f->vector];
        user_fault_kill(f, name ? name : "exception");
    }
    panic_trap(f);   /* every other exception in the kernel is fatal */
}
