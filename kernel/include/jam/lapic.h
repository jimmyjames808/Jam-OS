/* The local APIC (arch/x86_64/lapic.c): x2APIC or xAPIC, IPIs and NMIs,
 * and the per-CPU timer that drives both the scheduler tick and the
 * one-shot sleeper deadlines. Each function works on the calling CPU's own
 * APIC; call with interrupts off where it says so. */
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
/* Fixed-vector IPI to one CPU. */
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_nmi(uint32_t apic_id);
void     lapic_send_nmi_others(void);
/* The startup IPIs, to one CPU by APIC ID: INIT (it stops whatever the
 * CPU is doing and leaves it waiting for a SIPI) and SIPI (start in real
 * mode at vector << 12). Every wait inside is bounded: false if an xAPIC
 * never reported the IPI sent. Interrupts may be on or off. */
bool     lapic_send_init(uint32_t apic_id);
bool     lapic_send_sipi(uint32_t apic_id, uint8_t vector);
/* The APIC's error status (send/receive errors since the last read). */
uint32_t lapic_read_esr(void);
/* The APICs are in x2APIC mode (32-bit IDs) rather than xAPIC (8-bit). */
bool     lapic_x2apic(void);
/* INIT to every other CPU: each resets and waits for a SIPI, running no
 * code of anyone's (kexec's last step, after their NMI halt: the next
 * kernel starts them itself, and may reuse the memory they halted in). */
void     lapic_send_init_others(void);

/* Periodic per-CPU tick. TSC-deadline mode when the CPU has it (and
 * "nodeadline" is not on the command line), else the APIC's own counter. */
void     lapic_timer_calibrate(void);   /* BSP, once, after tsc_calibrate_with_loader */
void     lapic_timer_start(unsigned hz);
const char *lapic_timer_mode(void);

/* One-shot timers (see lapic.c): with interrupts off, ask for a timer
 * interrupt on THIS CPU at TSC value when_tsc (0 = none needed); it calls
 * sched_timer_expire. In the periodic mode the request waits for the next
 * tick. lapic_oneshot switches the requests off at run time (boot:
 * "nooneshot"); lapic_timer_has_oneshot says whether the mode can do them. */
void     lapic_timer_set(uint64_t when_tsc);
bool     lapic_timer_has_oneshot(void);
extern bool lapic_oneshot;

extern uint64_t lapic_errors;
extern uint32_t lapic_last_esr;
