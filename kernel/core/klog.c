#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/serial.h>
#include <jam/kprintf.h>
#include <jam/spinlock.h>
#include <jam/time.h>

#define KLOG_SIZE (64 * 1024)   /* power of two */

static char ring[KLOG_SIZE];
static uint64_t head;           /* total bytes ever written */
static spinlock_t ring_lock = SPINLOCK_INIT("klog");
static bool at_line_start = true;

/* One lock across ring, serial and console so lines from different CPUs
 * never interleave. */
static void emit(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        ring[head++ & (KLOG_SIZE - 1)] = s[i];
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
