#include <jam/irq.h>
#include <jam/lapic.h>
#include <jam/trap.h>

static irq_handler_t handlers[256];
volatile uint64_t irq_unexpected;
volatile uint8_t irq_last_unexpected;

void irq_register(uint8_t vector, irq_handler_t fn)
{
    handlers[vector] = fn;
}

void irq_dispatch(struct trap_frame *f)
{
    uint8_t v = (uint8_t)f->vector;
    if (handlers[v]) {
        handlers[v](f);   /* handlers send their own EOI */
        return;
    }
    if (v >= VEC_PIC_BASE && v < VEC_PIC_BASE + 16)
        return;   /* spurious 8259 interrupt: no EOI */
    irq_last_unexpected = v;   /* counted, not logged: see lapic.c */
    __atomic_add_fetch(&irq_unexpected, 1, __ATOMIC_RELAXED);
    lapic_eoi();
}
