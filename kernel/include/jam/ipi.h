/* Inter-processor interrupts. */
#pragma once

#include <stdint.h>

void ipi_init(void);
void ipi_send(uint32_t cpu, uint8_t vector);

/* Run fn(arg) on the given CPU / every other CPU / every CPU, and wait
 * until it has finished everywhere. Must be called with interrupts on and
 * not from an interrupt handler (two CPUs waiting on each other with
 * interrupts off would deadlock). */
void smp_call_on(uint32_t cpu, void (*fn)(void *), void *arg);
void smp_call_others(void (*fn)(void *), void *arg);
void smp_call_all(void (*fn)(void *), void *arg);

/* Flush a kernel virtual range from every other CPU's TLB. */
void tlb_shootdown(uint64_t va, uint64_t len);
/* Flush a range from the calling CPU's own TLB (preemption off; see ipi.c). */
void tlb_flush_local(uint64_t va, uint64_t len);

/* Stop every other CPU with an NMI (panic). Returns how many confirmed. */
uint32_t ipi_halt_others(void);
/* Watchdog: `cpu` stopped ticking. NMI it so it panics with its own stack. */
void watchdog_fire(uint32_t cpu);
/* NMI handler, from trap_dispatch. */
struct trap_frame;
void nmi_handler(struct trap_frame *f);

extern volatile int ipi_ready;   /* set once every CPU can take IPIs */
