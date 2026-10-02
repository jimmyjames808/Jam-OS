/* COM1 (dev/serial.c): the kernel log's first output and, while a process
 * reads it, a line of input.
 *
 * Output goes through a ring drained by the transmit interrupt once one is
 * routed, and synchronously before that, after a panic, or when the IRQ
 * never arrives. Input is a second ring filled by the receive interrupt.
 * The ring has no lock of its own: tx_lock and rx_lock in serial.c guard
 * the two rings. */
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

/* Interrupt-driven output (serial.c). serial_start_irq routes COM1's
 * IRQ 4 once the I/O APIC is up ("noserialirq": stay synchronous);
 * serial_poll runs from CPU 0's tick; serial_panic makes output
 * synchronous for good, writing out what is queued first (its newest
 * 4 KiB: panic path, interrupts off). serial_async is the run-time
 * switch. */
#define SERIAL_RING 65536   /* bytes, a power of two: ~5.7 s of output */
void serial_start_irq(void);
void serial_poll(void);
void serial_panic(void);
void serial_set_async(bool on);   /* the benchmark's switch; off drains the ring first */
extern bool serial_async;
extern uint64_t serial_dropped, serial_irqs, serial_rescues;
/* Bytes queued, not yet in the UART; whether output is interrupt-driven
 * (UART present, IRQ routed and working); whether the IRQ was given up on. */
uint32_t serial_pending(void);
bool serial_is_async(void);
bool serial_irq_broken(void);
/* Tests: stop (and restart) draining the ring. */
void serial_test_hold(bool on);

/* The ring itself, exposed so tests can drive a small one. size is a power
 * of two; head/tail count bytes ever put/taken; put drops (and counts) when
 * full; get returns -1 when empty; keep_newest drops (and counts) all but
 * the newest `keep` bytes and returns how many it dropped (a synchronous
 * drain writes only those). No locking of its own. */
struct serial_ring {
    char    *buf;          /* size bytes */
    uint32_t size;         /* a power of two */
    uint32_t head, tail;   /* bytes ever put / taken */
    uint64_t dropped;      /* bytes dropped: the ring was full, or cut by keep_newest */
};
bool     serial_ring_put(struct serial_ring *r, char c);
int      serial_ring_get(struct serial_ring *r);
uint32_t serial_ring_used(const struct serial_ring *r);
uint32_t serial_ring_keep_newest(struct serial_ring *r, uint32_t keep);

/* COM1 input (serial.c "Input"). serial_rx_start turns the receive
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
