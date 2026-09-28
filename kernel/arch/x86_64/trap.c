#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/trap.h>

void trap_dispatch(struct trap_frame *f)
{
    if (f->vector >= 32) {
        irq_dispatch(f);
        return;
    }
    switch (f->vector) {
    case 3:   /* int3: log and continue, handy for testing the trap path */
        kprintf("trap: breakpoint at %lx\n", f->rip);
        return;
    default:
        panic_trap(f);   /* M1: every other exception is fatal */
    }
}
