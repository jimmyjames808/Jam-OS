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

/* Readers (kernel/abi/sysc_console.c). The log's position is the count
 * of bytes ever written; the ring keeps the last KLOG_SIZE of them.
 * klog_read_at copies up to cap bytes from pos (moved up to the oldest
 * byte still kept, or down to the end), and *first gets where they start.
 * Takes the log lock briefly: fine from any thread context.
 *
 * KLOG_SIZE is 4 MiB: about twenty minutes of a soak's log (3 KB/s on the
 * PC) while logd has no stick to write to, and every burst seen so far,
 * so a reader that falls behind loses nothing. The ring is a static array
 * of the kernel image (kexec hands its pages to the next kernel by
 * address), and the next boot's logd saves a panicked one's ring as one
 * file within init's SAVE_WAIT (60 s): 4 MiB took 3 s in QEMU. */
#define KLOG_SIZE (4u << 20)
uint64_t klog_head(void);
/* The ring itself (KLOG_SIZE bytes, page-aligned; byte h of the log is at
 * h % KLOG_SIZE): kexec hands its pages to the next kernel. */
const char *klog_ring(void);
size_t   klog_read_at(uint64_t pos, char *buf, size_t cap, uint64_t *first);
/* The same as if the ring kept only the last `keep` bytes (keep <=
 * KLOG_SIZE): a test's reader makes a gap with a little log instead of
 * the whole ring's worth. */
size_t   klog_read_kept(uint64_t pos, uint64_t keep, char *buf, size_t cap, uint64_t *first);
/* The copy itself, on any ring (size a power of two, h bytes ever written):
 * tests drive it on a small one. */
size_t   klog_ring_copy(const char *ring, uint64_t size, uint64_t h, uint64_t pos, char *buf,
                        size_t cap, uint64_t *first);
/* CPU 0's tick: raise SIG_READABLE on readers that are behind (deferred to
 * the tick because klog_write runs under arbitrary locks). */
void     klog_poll(void);
