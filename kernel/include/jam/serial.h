#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* COM1. Most modern PCs have no serial port; init detects that and the
 * write becomes a no-op. In QEMU it is the main log channel, and on the
 * PC (which has one) the debug channel. */
bool serial_init(void);
void serial_write(const char *s, size_t len);

/* M5.5: interrupt-driven output (serial.c). serial_start_irq routes COM1's
 * IRQ 4 once the I/O APIC is up ("noserialirq": stay synchronous);
 * serial_poll runs from CPU 0's tick; serial_panic makes output
 * synchronous for good, writing out what is queued first (panic path,
 * interrupts off). serial_async is the run-time switch. */
#define SERIAL_RING 65536   /* bytes, a power of two: ~5.7 s of output */
void serial_start_irq(void);
void serial_poll(void);
void serial_panic(void);
void serial_set_async(bool on);   /* the benchmark's switch; off drains the ring first */
extern volatile bool serial_async;
extern volatile uint64_t serial_dropped, serial_irqs, serial_rescues;
/* Bytes queued, not yet in the UART; whether output is interrupt-driven
 * (UART present, IRQ routed and working); whether the IRQ was given up on. */
uint32_t serial_pending(void);
bool serial_is_async(void);
bool serial_irq_broken(void);
/* Tests: stop (and restart) draining the ring. */
void serial_test_hold(bool on);

/* The ring itself, exposed so tests can drive a small one. size is a power
 * of two; head/tail count bytes ever put/taken; put drops (and counts) when
 * full; get returns -1 when empty. No locking of its own. */
struct serial_ring {
    char    *buf;
    uint32_t size;
    uint32_t head, tail;
    uint64_t dropped;
};
bool     serial_ring_put(struct serial_ring *r, char c);
int      serial_ring_get(struct serial_ring *r);
uint32_t serial_ring_used(const struct serial_ring *r);
