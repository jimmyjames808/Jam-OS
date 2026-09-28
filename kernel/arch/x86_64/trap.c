#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/trap.h>

void trap_dispatch(struct trap_frame *f)
{
    if (f->vector >= 32) {
        struct cpu *c = this_cpu();
        c->irq_depth++;
        irq_dispatch(f);
        c->irq_depth--;
        sched_irq_exit(f->rflags);   /* may switch threads before iretq */
        return;
    }
    switch (f->vector) {
    case 2:
        nmi_handler(f);
        return;
    case 3:   /* int3: log and continue, handy for testing the trap path */
        kprintf("trap: breakpoint at %lx\n", f->rip);
        return;
    default:
        panic_trap(f);   /* M1: every other exception is fatal */
    }
}
