/* Interrupt vectors and handler registration.
 *
 *   0x00-0x1f  CPU exceptions
 *   0x20-0x2f  legacy 8259 PIC (masked; only spurious ones can arrive)
 *   0x30-0xef  device interrupts (I/O APIC, MSI); 0x30 = COM1,
 *              0x31-0xef per-CPU MSI vectors (vector_alloc)
 *   0xf0       LAPIC timer
 *   0xf1-0xfd  IPIs
 *   0xfe       LAPIC error
 *   0xff       LAPIC spurious
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VEC_PIC_BASE    0x20
#define VEC_COM1        0x30   /* the serial port's interrupt */
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

/* Device vectors: 0x31-0xef on every CPU, allocated per (cpu, vector)
 * by vector_alloc / vector_free (<jam/interrupt.h>). A device vector that
 * arrives with no owner (a message still in flight after its owner freed
 * it, a stray IPI) is EOI'd and counted here, never a panic. */
#define VEC_DEVICE_COUNT (0xef - 0x31 + 1)
extern volatile uint64_t irq_device_unowned;
extern volatile uint8_t  irq_device_last_unowned;
/* Vectors allocated on `cpu` right now (tests, reports). */
uint32_t vector_count(uint32_t cpu);

/* The target-CPU rule, over a snapshot of the CPUs (index = CPU index):
 * eligible = online, APIC ID usable in an xAPIC MSI (< 255) and not full;
 * CPU 0 only when it is the only usable CPU; then E-cores before others,
 * then the fewest vectors, then the lowest index. UINT32_MAX: none. The
 * real allocator runs it on the live topology; tests on a made-up one. */
struct vector_cpu_view {
    uint8_t  type;      /* enum core_type */
    bool     usable;    /* online and APIC ID < 255 */
    uint16_t nvec;      /* allocated vectors */
};
uint32_t vector_pick_cpu(const struct vector_cpu_view *v, uint32_t n);

static inline void irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
