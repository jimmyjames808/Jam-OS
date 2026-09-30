/* The serial transmit ring and its interrupt (kernel/dev/serial.c). */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/time.h>

/* ---- serial transmit ring ------------------------------------------------------ */

/* The ring drops (and counts) what doesn't fit, never overwrites, and keeps
 * order across wrap-around. */
KTEST(serial_ring_drops_when_full)
{
    char buf[8];
    struct serial_ring r = { buf, sizeof(buf), 0xfffffff0u, 0xfffffff0u, 0 };   /* near wrap */
    for (int i = 0; i < 10; i++)
        serial_ring_put(&r, (char)('a' + i));
    KT_EQ(serial_ring_used(&r), 8);
    KT_EQ(r.dropped, 2);
    for (int i = 0; i < 8; i++)
        KT_EQ(serial_ring_get(&r), 'a' + i);
    KT_EQ(serial_ring_get(&r), -1);
    for (int round = 0; round < 100; round++) {   /* head and tail wrap 32 bits */
        for (int i = 0; i < 5; i++)
            KT_ASSERT(serial_ring_put(&r, (char)i));
        for (int i = 0; i < 5; i++)
            KT_EQ(serial_ring_get(&r), i);
    }
    KT_EQ(r.dropped, 2);
}

/* Queued output is sent by the UART's transmit interrupt (QEMU emulates
 * COM1 and its IRQ 4). The ring is shared: from the shell, the console
 * and the kernel log write to COM1 at the same time, so a full 32 KiB ring
 * can't empty in 2 s (at 115200 baud it drains ~11 KiB/s). What was queued
 * and what is left are system-wide counts, checked only at the boot menu;
 * that the interrupt fired is checked everywhere. */
KTEST(serial_irq_drains_ring)
{
    if (!serial_is_async() || !__atomic_load_n(&serial_async, __ATOMIC_RELAXED))
        return;
    static const char line[] = "serial: this line was sent by the transmit interrupt\n";
    serial_test_hold(true);
    uint32_t before = serial_pending();
    serial_write(line, sizeof(line) - 1);
    uint32_t queued = serial_pending() - before;
    uint64_t irqs0 = __atomic_load_n(&serial_irqs, __ATOMIC_RELAXED);
    serial_test_hold(false);
    uint64_t deadline = uptime_ns() + 2000 * NS_PER_MS;
    while (serial_pending() && uptime_ns() < deadline)
        thread_sleep_ms(1);
    uint32_t left = serial_pending();   /* before our own kprintf queues more */
    uint64_t irqs = __atomic_load_n(&serial_irqs, __ATOMIC_RELAXED) - irqs0;
    kprintf("serial: %u bytes queued, drained with %lu interrupts (%lu tick rescues so far)\n",
            queued, irqs, __atomic_load_n(&serial_rescues, __ATOMIC_RELAXED));
    KT_GLOBAL_EQ(queued, sizeof(line));   /* the newline went out as \r\n */
    KT_GLOBAL_EQ(left, 0);
    KT_ASSERT(irqs > 0);
}
