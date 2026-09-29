/* Kernel log: every line goes to the in-memory ring buffer, the framebuffer
 * console and (when present) the COM1 serial port. */
#pragma once

#include <stddef.h>

void   klog_write(const char *s, size_t len);
/* Copy up to `size` bytes of the most recent log text into buf. */
size_t klog_tail(char *buf, size_t size);
/* Write text that already carries its own timestamps (the panic screen's
 * copy of the log tail). */
void   klog_write_raw(const char *s, size_t len);
/* Panic only: drop the log lock in case this CPU died holding it. */
void   klog_force_unlock(void);

/* M7: readers (kernel/abi/sysc_console.c). The log's position is the count
 * of bytes ever written; the ring keeps the last KLOG_SIZE of them.
 * klog_read_at copies up to cap bytes from pos (moved up to the oldest
 * byte still kept, or down to the end), and *first gets where they start.
 * Takes the log lock briefly: fine from any thread context. */
#define KLOG_SIZE (64 * 1024)
uint64_t klog_head(void);
size_t   klog_read_at(uint64_t pos, char *buf, size_t cap, uint64_t *first);
/* The copy itself, on any ring (size a power of two, h bytes ever written):
 * tests drive it on a small one. */
size_t   klog_ring_copy(const char *ring, uint64_t size, uint64_t h, uint64_t pos, char *buf,
                        size_t cap, uint64_t *first);
/* CPU 0's tick: raise SIG_READABLE on readers that are behind (deferred to
 * the tick because klog_write runs under arbitrary locks). */
void     klog_poll(void);
