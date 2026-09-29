/* Device interrupt vectors and interrupt objects
 * (kernel/object/interrupt.c, vector allocator in kernel/arch/x86_64/irq.c).
 *
 * Vectors 0x31-0xef on every CPU are allocated per (cpu, vector) pair; 0x30
 * stays COM1's. An MSI targets one CPU (xAPIC format: APIC ID < 256, no
 * interrupt remapping yet). The allocator prefers E-cores, then the
 * CPU with the fewest vectors; CPU 0 only when nothing else is online.
 *
 * An interrupt object owns one vector. When it fires (IRQ context: no
 * allocation, no sleeping) it raises SIG_INTERRUPT and, for MSI-X or a
 * maskable MSI, masks the vector at the device; further fires before
 * interrupt_ack are counted. Userspace binds it to a port PERSISTENT for
 * SIG_INTERRUPT (the packet's `count` is the coalesced fires) and calls
 * interrupt_ack to clear the signal and unmask. Destroying it disables the
 * vector at the device, waits until no CPU is still in its handler, then
 * frees the vector. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/object.h>
#include <jam/status.h>

struct trap_frame;
struct pci_dev;

#define VEC_DEVICE_FIRST 0x31
#define VEC_DEVICE_LAST  0xef

typedef void (*vector_fn_t)(void *ctx);

/* Allocate a free (cpu, vector) and route it to fn(ctx), called from the
 * interrupt with interrupts off, before the LAPIC EOI. ERR_NO_RESOURCES
 * when every CPU is full. */
status_t vector_alloc(vector_fn_t fn, void *ctx, uint32_t *cpu, uint8_t *vec);
/* Unroute it and wait until no CPU is still running its handler. Thread
 * context only. */
void vector_free(uint32_t cpu, uint8_t vec);
/* The MSI address and data that deliver `vec` to `cpu` (fixed, edge). */
uint64_t msi_address(uint32_t cpu);
uint32_t msi_data(uint8_t vec);

/* Interrupt objects (OBJ_INTERRUPT). flags: IRQ_MSIX from <jam/abi.h>. */
status_t interrupt_create_msi(struct pci_dev *d, uint32_t index, uint32_t flags,
                              struct kobject **out);
status_t interrupt_ack(struct kobject *irq);
/* Statistics for tests and the benchmark. */
uint64_t interrupt_fire_count(struct kobject *irq);

/* ktests only: an interrupt object with no device behind it, fired by
 * interrupt_fire_virtual (from any context, including an IPI handler). */
status_t interrupt_create_virtual(struct kobject **out);
void interrupt_fire_virtual(struct kobject *irq);
