#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

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

/* M7: COM1 input (serial.c "Input"). serial_rx_start turns the receive
 * interrupt on and calls notify(ctx) (interrupts off, under the rx lock)
 * whenever bytes arrived; ERR_BAD_STATE if a reader exists already,
 * ERR_NOT_FOUND without a UART. serial_rx_read takes up to cap bytes and,
 * if the ring is then empty, calls empty(ctx) under the same lock (so a
 * reader's "readable" signal can't be cleared after new bytes set it).
 * serial_rx_stop: no more calls to notify once it returns. inject: tests
 * put bytes in as if received. */
bool     serial_present(void);
status_t serial_rx_start(void (*notify)(void *), void *ctx);
void     serial_rx_stop(void);
size_t   serial_rx_read(char *buf, size_t cap, void (*empty)(void *), void *ctx);
void     serial_rx_inject(const char *s, size_t len);
uint64_t serial_rx_dropped(void);
extern volatile uint64_t serial_rx_bytes, serial_rx_errors;
