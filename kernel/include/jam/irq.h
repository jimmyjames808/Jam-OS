/* Interrupt vectors and handler registration.
 *
 *   0x00-0x1f  CPU exceptions
 *   0x20-0x2f  legacy 8259 PIC (masked; only spurious ones can arrive)
 *   0x30-0xef  device interrupts (I/O APIC, MSI), from M6; 0x30 = COM1 (M5.5)
 *   0xf0       LAPIC timer
 *   0xf1-0xfd  IPIs, from M3
 *   0xfe       LAPIC error
 *   0xff       LAPIC spurious
 */
#pragma once

#include <stdint.h>

#define VEC_PIC_BASE    0x20
#define VEC_COM1        0x30   /* M5.5: the serial port's transmit interrupt */
#define VEC_TIMER       0xf0
#define VEC_RESCHEDULE  0xf1
#define VEC_CALL        0xf2
#define VEC_LAPIC_ERROR 0xfe
#define VEC_SPURIOUS    0xff

struct trap_frame;
typedef void (*irq_handler_t)(struct trap_frame *f);

void irq_register(uint8_t vector, irq_handler_t fn);
extern volatile uint64_t irq_unexpected;
extern volatile uint8_t  irq_last_unexpected;
void irq_dispatch(struct trap_frame *f);

static inline void irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
