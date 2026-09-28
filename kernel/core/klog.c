#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/serial.h>
#include <jam/spinlock.h>

#define KLOG_SIZE (64 * 1024)   /* power of two */

static char ring[KLOG_SIZE];
static uint64_t head;           /* total bytes ever written */
static spinlock_t ring_lock = SPINLOCK_INIT;

/* One lock across ring, serial and console so lines from different CPUs
 * never interleave. Interrupt handlers must not log (M3 adds irqsave). */
void klog_write(const char *s, size_t len)
{
    spin_lock(&ring_lock);
    for (size_t i = 0; i < len; i++)
        ring[head++ & (KLOG_SIZE - 1)] = s[i];
    serial_write(s, len);
    fbcon_write(s, len);
    spin_unlock(&ring_lock);
}

void klog_force_unlock(void)
{
    spin_unlock(&ring_lock);
}

size_t klog_tail(char *buf, size_t size)
{
    spin_lock(&ring_lock);
    uint64_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    uint64_t n = avail < size ? avail : size;
    for (uint64_t i = 0; i < n; i++)
        buf[i] = ring[(head - n + i) & (KLOG_SIZE - 1)];
    spin_unlock(&ring_lock);
    return (size_t)n;
}
