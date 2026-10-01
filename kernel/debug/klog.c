/* The kernel log. Every line gets an uptime stamp and goes, under one
 * lock, to the ring (KLOG_SIZE bytes), COM1 and the framebuffer console, so
 * lines from different CPUs never interleave. The ring is what the panic
 * screen shows (klog_tail) and what the console process follows through the
 * klog read syscall (klog_read_at). */
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/time.h>

/* KLOG_SIZE (klog.h): a power of two */

/* Page-aligned: a crash kernel is handed exactly its pages (kexec). */
static char ring[KLOG_SIZE] __attribute__((aligned(4096)));
static uint64_t head;           /* total bytes ever written; ring_lock (klog_head reads it
                                   without, so it is stored atomically) */
static spinlock_t ring_lock = SPINLOCK_INIT("klog");
static bool at_line_start = true;

/* One lock across ring, serial and console so lines from different CPUs
 * never interleave. */
static void emit(const char *s, size_t len)
{
    uint64_t h = head;
    for (size_t i = 0; i < len; i++)
        ring[h++ & (KLOG_SIZE - 1)] = s[i];
    __atomic_store_n(&head, h, __ATOMIC_RELAXED);
    serial_write(s, len);
    fbcon_write(s, len);
}

/* Every line starts with seconds since the TSC was calibrated. Interrupt
 * handlers may log: the lock is always taken with interrupts off. */
void klog_write(const char *s, size_t len)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    while (len) {
        if (at_line_start) {
            char stamp[24];
            uint64_t ns = tsc_hz ? uptime_ns() : 0;
            int n = ksnprintf(stamp, sizeof(stamp), "[%5lu.%06lu] ", ns / 1000000000,
                              (ns / 1000) % 1000000);
            emit(stamp, (size_t)n);
            at_line_start = false;
        }
        size_t chunk = 0;
        while (chunk < len && s[chunk] != '\n')
            chunk++;
        if (chunk < len) {
            chunk++;   /* include the newline */
            at_line_start = true;
        }
        emit(s, chunk);
        s += chunk;
        len -= chunk;
    }
    spin_unlock_irqrestore(&ring_lock, f);
}

void klog_write_raw(const char *s, size_t len)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    emit(s, len);
    if (len)
        at_line_start = s[len - 1] == '\n';
    spin_unlock_irqrestore(&ring_lock, f);
}

void klog_force_unlock(void)
{
    spin_force_unlock(&ring_lock);
}

size_t klog_tail(char *buf, size_t size)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    uint64_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    uint64_t n = avail < size ? avail : size;
    for (uint64_t i = 0; i < n; i++)
        buf[i] = ring[(head - n + i) & (KLOG_SIZE - 1)];
    spin_unlock_irqrestore(&ring_lock, f);
    return (size_t)n;
}

/* ---- readers ------------------------------------------------------------------ */

uint64_t klog_head(void)
{
    return __atomic_load_n(&head, __ATOMIC_RELAXED);
}

const char *klog_ring(void)
{
    return ring;
}

size_t klog_ring_copy(const char *r, uint64_t size, uint64_t h, uint64_t pos, char *buf,
                      size_t cap, uint64_t *first)
{
    uint64_t oldest = h > size ? h - size : 0;
    if (pos < oldest)
        pos = oldest;
    if (pos > h)
        pos = h;
    uint64_t n = h - pos < cap ? h - pos : cap;
    for (uint64_t i = 0; i < n; i++)
        buf[i] = r[(pos + i) & (size - 1)];
    *first = pos;
    return (size_t)n;
}

size_t klog_read_at(uint64_t pos, char *buf, size_t cap, uint64_t *first)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    size_t n = klog_ring_copy(ring, KLOG_SIZE, head, pos, buf, cap, first);
    spin_unlock_irqrestore(&ring_lock, f);
    return n;
}
