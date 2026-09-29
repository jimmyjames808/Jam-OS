/* COM1, 115200 8N1.
 *
 * Output (M5.5) goes through a transmit ring drained by the UART's own
 * transmit-empty interrupt (COM1 = ISA IRQ 4, routed through the I/O APIC
 * to CPU 0). M5 wrote every character synchronously, waiting on the UART
 * with interrupts off under the log lock: about 87 us a character at
 * 115200 baud, ~9 ms for a 100-character line, on every CPU that logged.
 *
 *   - serial_write (called by klog under its lock, interrupts off) copies
 *     into the ring under the "serial tx" lock (always taken with
 *     interrupts off; the handler takes it too; nothing is taken under it)
 *     and, if the transmitter is idle, "kicks" it by enabling the
 *     transmit-empty interrupt, which fires at once. Each interrupt
 *     refills the FIFO (16 bytes) and, when the ring is empty, disables
 *     the interrupt again.
 *   - A full ring drops characters instead of waiting; serial_dropped
 *     counts them (the text is still in the klog ring and on the screen).
 *   - The ISA line is edge-triggered, so a lost edge would stall output.
 *     CPU 0's tick checks (serial_poll): if the transmitter is idle with
 *     bytes queued and no interrupt came since the last tick, it refills
 *     the FIFO itself. If that has had to happen SERIAL_RESCUES_MAX times
 *     and no interrupt ever came (IRQ 4 not wired where the MADT says),
 *     output goes back to synchronous for good (serial_irq_broken; the
 *     RESULTS box says so).
 *   - Before the interrupt is routed (early boot), with "noserialirq", and
 *     after serial_panic, output is synchronous as in M5. serial_panic
 *     (interrupts off, other CPUs halted) first writes out whatever the
 *     ring still holds, so the panic text follows it in order.
 *   - serial_async is the run-time switch (the benchmark flips it). */
#include <stdint.h>
#include <jam/cmdline.h>
#include <jam/ioapic.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/percpu.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/x86.h>

#define COM1 0x3f8
#define REG_DATA 0
#define REG_IER  1
#define REG_IIR  2
#define REG_LSR  5
#define IER_THRE 0x02
#define LSR_THRE 0x20
#define FIFO_LEN 16
#define SERIAL_RESCUES_MAX 8
#define BROKEN_FLUSH 4096

static bool present;
static bool irq_routed;          /* the interrupt is wired: async possible */
volatile bool serial_async;      /* the switch */
static volatile bool broken;     /* no interrupt ever came: synchronous for good */

static char tx_buf[SERIAL_RING];
static struct serial_ring tx = { tx_buf, SERIAL_RING, 0, 0, 0 };
static spinlock_t tx_lock = SPINLOCK_INIT("serial tx");
static bool thre_on;             /* transmit interrupt enabled (tx_lock) */
static volatile bool hold;       /* tests: don't drain */
volatile uint64_t serial_dropped, serial_irqs, serial_rescues;
static uint64_t last_irqs;
static uint32_t last_tail;

/* ---- the ring (also used by the tests on a ring of their own) ---------------- */

bool serial_ring_put(struct serial_ring *r, char c)
{
    if (r->head - r->tail == r->size) {
        r->dropped++;
        return false;
    }
    r->buf[r->head++ & (r->size - 1)] = c;
    return true;
}

int serial_ring_get(struct serial_ring *r)
{
    if (r->head == r->tail)
        return -1;
    return (unsigned char)r->buf[r->tail++ & (r->size - 1)];
}

uint32_t serial_ring_used(const struct serial_ring *r)
{
    return r->head - r->tail;
}

/* ---- the UART ------------------------------------------------------------------ */

bool serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xc7);   /* FIFO on, cleared */
    outb(COM1 + 4, 0x1e);   /* loopback mode for the self-test */
    outb(COM1 + 0, 0xae);
    if (inb(COM1 + 0) != 0xae) {
        present = false;    /* no UART: normal on modern PCs */
        return false;
    }
    outb(COM1 + 4, 0x0f);   /* normal operation; OUT2 gates the IRQ line */
    present = true;
    return true;
}

static void put_sync(char c)
{
    for (int spins = 0; !(inb(COM1 + REG_LSR) & LSR_THRE); spins++)
        if (spins > 100000)
            return;         /* never hang the kernel on a stuck UART */
    outb(COM1 + REG_DATA, (uint8_t)c);
}

/* tx_lock held: if the FIFO is empty, move up to FIFO_LEN bytes into it. */
static void fill_fifo_locked(void)
{
    if (hold || !(inb(COM1 + REG_LSR) & LSR_THRE))
        return;
    for (int i = 0; i < FIFO_LEN; i++) {
        int c = serial_ring_get(&tx);
        if (c < 0)
            break;
        outb(COM1 + REG_DATA, (uint8_t)c);
    }
}

/* tx_lock held: the transmit interrupt on exactly while bytes are queued. */
static void set_thre_locked(void)
{
    bool want = serial_ring_used(&tx) && !hold;
    if (want != thre_on) {
        thre_on = want;
        outb(COM1 + REG_IER, want ? IER_THRE : 0);
    }
}

static void on_com1(struct trap_frame *f)
{
    (void)f;
    spin_lock(&tx_lock);   /* interrupt handler: interrupts are off */
    serial_irqs++;
    (void)inb(COM1 + REG_IIR);   /* acknowledges a transmit-empty interrupt */
    fill_fifo_locked();
    set_thre_locked();
    spin_unlock(&tx_lock);
    lapic_eoi();
}

void serial_start_irq(void)
{
    if (!present || cmdline_has("noserialirq"))
        return;
    irq_register(VEC_COM1, on_com1);
    if (!ioapic_route_isa(4, VEC_COM1, cpus[0]->lapic_id)) {
        kprintf("serial: can't route COM1's IRQ 4: output stays synchronous\n");
        return;
    }
    irq_routed = true;
    serial_async = true;
}

static void write_sync(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            put_sync('\r');
        put_sync(s[i]);
    }
}

void serial_write(const char *s, size_t len)
{
    if (!present)
        return;
    if (!serial_async || broken || !irq_routed) {
        write_sync(s, len);
        return;
    }
    uint64_t f = spin_lock_irqsave(&tx_lock);
    uint64_t lost = tx.dropped;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            serial_ring_put(&tx, '\r');
        serial_ring_put(&tx, s[i]);
    }
    serial_dropped += tx.dropped - lost;
    /* Kick: enabling the transmit-empty interrupt while the transmitter
     * is empty raises it at once (16550; QEMU too), and the handler fills
     * the FIFO. So the writer pays one port write, not sixteen (port I/O
     * to a Super I/O UART takes on the order of a microsecond each). A
     * UART that only interrupts on the transition is rescued by
     * serial_poll within a tick. */
    set_thre_locked();
    spin_unlock_no_resched(&tx_lock);
    irq_restore(f);
}

/* CPU 0's tick, interrupts off. See the top: rescue a stalled transmitter. */
void serial_poll(void)
{
    if (!irq_routed || broken || !serial_ring_used(&tx))
        return;
    spin_lock(&tx_lock);
    uint64_t irqs = serial_irqs;
    bool stalled = !hold && thre_on && irqs == last_irqs && tx.tail == last_tail &&
                   (inb(COM1 + REG_LSR) & LSR_THRE);
    if (stalled) {
        serial_rescues++;
        fill_fifo_locked();
        set_thre_locked();
        if (!irqs && serial_rescues >= SERIAL_RESCUES_MAX)
            broken = true;
    }
    if (broken) {
        /* Write out what is queued, synchronously, before anyone writes
         * synchronously after it, with this lock held so other CPUs' log
         * lines wait (and keep their order). At most BROKEN_FLUSH bytes
         * (~0.36 s at 115200 baud, far below the 5 s the lock checker and
         * the watchdog allow); the rest is dropped and counted. This is
         * detected within SERIAL_RESCUES_MAX ticks of the first queued
         * byte, so little is queued by then. A failure path: M5's
         * behaviour from here on. */
        outb(COM1 + REG_IER, 0);
        thre_on = false;
        int c;
        for (int n = 0; n < BROKEN_FLUSH && (c = serial_ring_get(&tx)) >= 0; n++)
            put_sync((char)c);
        serial_dropped += serial_ring_used(&tx);
        tx.tail = tx.head;
    }
    last_irqs = irqs;
    last_tail = tx.tail;
    spin_unlock(&tx_lock);
}

bool serial_irq_broken(void)
{
    return broken;
}

void serial_panic(void)
{
    if (!present)
        return;
    spin_force_unlock(&tx_lock);   /* a halted CPU may have held it */
    serial_async = false;
    outb(COM1 + REG_IER, 0);
    thre_on = false;
    hold = false;
    int c;
    while ((c = serial_ring_get(&tx)) >= 0)
        put_sync((char)c);
}

/* The run-time switch, for the benchmark. Turning it off first drains the
 * ring synchronously with the lock held and the transmit interrupt off, so
 * synchronous writes that follow can't overtake or interleave with bytes
 * still queued (review finding 4). */
void serial_set_async(bool on)
{
    if (!present)
        return;
    uint64_t f = spin_lock_irqsave(&tx_lock);
    if (!on && serial_async) {
        outb(COM1 + REG_IER, 0);
        thre_on = false;
        int c;
        while ((c = serial_ring_get(&tx)) >= 0)
            put_sync((char)c);
    }
    serial_async = on;
    if (on)
        set_thre_locked();
    spin_unlock_no_resched(&tx_lock);
    irq_restore(f);
}

uint32_t serial_pending(void)
{
    return serial_ring_used(&tx);
}

bool serial_is_async(void)
{
    return present && irq_routed && !broken;
}

void serial_test_hold(bool on)
{
    uint64_t f = spin_lock_irqsave(&tx_lock);
    hold = on;
    if (!on)
        fill_fifo_locked();
    set_thre_locked();
    spin_unlock_no_resched(&tx_lock);
    irq_restore(f);
}
