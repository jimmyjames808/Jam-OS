#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* x2apic: the loader already switched the APICs to x2APIC mode. */
void     lapic_init_bsp(bool x2apic);
/* Per-CPU setup: enable, spurious/error vectors, NMI pins from the MADT. */
void     lapic_init_cpu(struct cpu *c);
uint32_t lapic_id(void);
void     lapic_eoi(void);
/* Fixed-vector IPI to one CPU, or to all CPUs except this one. */
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_ipi_others(uint8_t vector);
void     lapic_send_nmi(uint32_t apic_id);
void     lapic_send_nmi_others(void);
bool     lapic_x2apic_mode(void);

/* Periodic per-CPU tick. TSC-deadline mode when the CPU has it (and
 * "nodeadline" is not on the command line), else the APIC's own counter. */
void     lapic_timer_calibrate(void);   /* BSP, once, after tsc_calibrate */
void     lapic_timer_start(unsigned hz);
const char *lapic_timer_mode(void);

/* One-shot timers (see lapic.c): with interrupts off, ask for a timer
 * interrupt on THIS CPU at TSC value when_tsc (0 = none needed); it calls
 * sched_timer_expire. In the periodic mode the request waits for the next
 * tick. lapic_oneshot switches the requests off at run time (boot:
 * "nooneshot"); lapic_timer_has_oneshot says whether the mode can do them. */
void     lapic_timer_set(uint64_t when_tsc);
bool     lapic_timer_has_oneshot(void);
extern volatile bool lapic_oneshot;
extern volatile uint64_t lapic_early_irqs;

extern volatile uint64_t lapic_errors;
extern volatile uint32_t lapic_last_esr;
