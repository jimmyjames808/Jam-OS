/* Inter-processor interrupts. */
#pragma once

#include <stdint.h>
#include <jam/sched.h>   /* cpumask_t */

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
/* Flush a range on the CPUs in mask only (the caller's own CPU included if
 * it is in the mask): for user address spaces, whose active-CPU masks say
 * who can hold their entries. Same calling rules as smp_call_on. */
void tlb_shootdown_mask(const cpumask_t *mask, uint64_t va, uint64_t len);
/* How many masked shootdowns `cpu` has handled (tests). */
uint64_t tlb_mask_flush_count(uint32_t cpu);
/* Flush a range from the calling CPU's own TLB (preemption off; see ipi.c). */
void tlb_flush_local(uint64_t va, uint64_t len);

/* Stop every other CPU with an NMI (panic). Returns how many confirmed. */
uint32_t ipi_halt_others(void);
/* Watchdog: `cpu` stopped ticking. NMI it so it panics with its own stack. */
void watchdog_fire(uint32_t cpu);
/* NMI handler, from trap_dispatch. */
struct trap_frame;
void nmi_handler(struct trap_frame *f);

/* Set (a release store) once every CPU can take IPIs; read it with
 * __atomic_load_n(&ipi_ready, __ATOMIC_ACQUIRE). */
extern int ipi_ready;
