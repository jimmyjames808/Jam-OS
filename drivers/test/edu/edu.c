/* edu: the driver for QEMU's `edu` test device (1234:11e8, hw/misc/edu.c),
 * the sample driver: drv/edu in bootfs, a process devmgr starts.
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_BAR(0)   its registers (1 MiB of MMIO; the first page is all we use)
 *   DR_IRQ(0)   its MSI, bound to a port PERSISTENT for SIG_INTERRUPT
 *   DR_DMA      its dma_cap (Bus Master Enable starts off; setup turns it
 *               on once the device's DMA engine is idle)
 *   DR_PCIDEV   its function (config reads; no RIGHT_MANAGE)
 *   DR_SERVE    the channel it serves abi/idl/edu.idl on
 *
 * At start it checks the device (identification, the liveness register,
 * a factorial by polling), then serves requests one at a time until the
 * client closes the channel. Every interrupt wait goes through the port:
 * a packet, then the device's interrupt status (0x24) is read and
 * acknowledged (0x64), then the interrupt object (drv_interrupt_ack). Bits
 * seen for another operation are kept for it.
 *
 * Registers (BAR 0): 0x00 identification 0xRRrr00ed, 0x04 liveness (reads
 * back ~x), 0x08 factorial (write n, read n! once status bit 0 clears),
 * 0x20 status (bit 0 computing, bit 7 raise FACT_IRQ when done), 0x24
 * interrupt status, 0x60 raise (ORs into 0x24 and sends the MSI), 0x64
 * acknowledge (clears those bits), DMA 0x80 source, 0x88 destination, 0x90
 * count, 0x98 command (bit 0 start, bit 1 direction: 0 RAM -> device,
 * 1 device -> RAM, bit 2 raise DMA_IRQ when done). The device's DMA buffer
 * is at device address 0x40000, 4 KiB; a transfer takes ~100 ms of QEMU
 * time. DMA addresses must be below 4 GiB (qemu-test.sh sets dma_mask). */
#include <jam/driver.h>
#include <idl/edu.h>

#define R_ID        0x00
#define R_LIVE      0x04
#define R_FACT      0x08
#define R_STATUS    0x20
#define R_IRQ_STAT  0x24
#define R_IRQ_RAISE 0x60
#define R_IRQ_ACK   0x64
#define R_DMA_SRC   0x80
#define R_DMA_DST   0x88
#define R_DMA_CNT   0x90
#define R_DMA_CMD   0x98

#define ST_COMPUTING 0x01u
#define ST_IRQFACT   0x80u
#define IRQ_FACT     0x00000001u
#define IRQ_DMA      0x00000100u
#define IRQ_TEST     0x00010000u   /* raise_irq's own bit */
#define DMA_RUN      0x1u
#define DMA_TO_RAM   0x2u
#define DMA_IRQ      0x4u
#define BUF_ADDR     0x40000u      /* the device's DMA buffer */
#define BUF_SIZE     4096u

#define PAGE         4096u
#define IRQ_KEY      0xed
#define OP_TIMEOUT   (2000 * NS_PER_MS)   /* a DMA is ~100 ms in QEMU: generous under load */

struct edu {
    volatile void *regs;   /* BAR0, mapped */
    handle_t irq, port;    /* DR_IRQ(0); the port its interrupts arrive on */
    handle_t dma, vmo;     /* DR_DMA; the DMA buffer's VMO */
    uint8_t *buf;          /* the DMA VMO mapped: page 0 goes out, page 1 comes back */
    uint32_t seen;         /* interrupt status bits taken off the device, not yet used */
    uint32_t seq;          /* varies the DMA pattern */
    bool     held;         /* dma_start's transfer: running, its pin held */
    uint64_t held_pin;     /* that pin's id */
    uint64_t irqs;         /* interrupts taken */
};

static uint32_t rd(struct edu *e, uint32_t off) { return drv_read32(e->regs, off); }
static void wr(struct edu *e, uint32_t off, uint32_t v) { drv_write32(e->regs, off, v); }

/* Wait until one of `bits` has been raised by the device (from a packet
 * now or bits kept from before). *woke (may be NULL): when the packet that
 * brought them was taken. */
static status_t wait_irq(struct edu *e, uint32_t bits, uint64_t deadline, uint64_t *woke)
{
    while (!(e->seen & bits)) {
        struct port_packet pkt;
        status_t st = drv_port_wait(e->port, deadline, &pkt);
        if (st != OK)
            return st;
        uint64_t t = drv_clock_ns();
        if (pkt.key != IRQ_KEY || pkt.type != PORT_PACKET_SIGNAL)
            continue;
        e->irqs += pkt.signal.count;
        /* The device first (its status bits), then the object: a raise
         * after this point sends a new MSI, which queues a new packet. */
        uint32_t s = rd(e, R_IRQ_STAT);
        if (s)
            wr(e, R_IRQ_ACK, s);
        st = drv_interrupt_ack(e->irq);
        if (st != OK)
            return st;
        e->seen |= s;
        if (woke)
            *woke = t;
    }
    e->seen &= ~bits;
    return OK;
}

/* ---- factorial ---------------------------------------------------------------- */

static status_t fact_wait_idle(struct edu *e, uint64_t deadline)
{
    while (rd(e, R_STATUS) & ST_COMPUTING)
        if (drv_clock_ns() > deadline)
            return ERR_TIMED_OUT;
    return OK;
}

static status_t fact_poll(struct edu *e, uint32_t n, uint32_t *out)
{
    uint64_t deadline = drv_clock_ns() + OP_TIMEOUT;
    status_t st = fact_wait_idle(e, deadline);
    if (st != OK)
        return st;
    wr(e, R_STATUS, 0);   /* no interrupt */
    wr(e, R_FACT, n);
    st = fact_wait_idle(e, deadline);
    if (st == OK)
        *out = rd(e, R_FACT);
    return st;
}

static status_t fact_irq(struct edu *e, uint32_t n, uint32_t *out)
{
    uint64_t deadline = drv_clock_ns() + OP_TIMEOUT;
    status_t st = fact_wait_idle(e, deadline);
    if (st != OK)
        return st;
    e->seen &= ~IRQ_FACT;
    wr(e, R_STATUS, ST_IRQFACT);
    wr(e, R_FACT, n);
    st = wait_irq(e, IRQ_FACT, deadline, NULL);
    wr(e, R_STATUS, 0);
    if (st != OK)
        return st;
    if (rd(e, R_STATUS) & ST_COMPUTING)
        return ERR_INTERNAL;   /* interrupt before the result: a device bug */
    *out = rd(e, R_FACT);
    return OK;
}

static uint32_t fact_expected(uint32_t n)
{
    uint32_t r = 1;
    for (uint32_t i = 2; i <= n; i++)
        r *= i;
    return r;
}

/* ---- DMA ------------------------------------------------------------------------ */

/* Finish dma_start's transfer, if one is running: its interrupt, then the
 * pin goes. */
static status_t finish_held(struct edu *e)
{
    if (!e->held)
        return OK;
    status_t st = wait_irq(e, IRQ_DMA, drv_clock_ns() + OP_TIMEOUT, NULL);
    status_t u = drv_vmo_unpin(e->vmo, e->dma, e->held_pin);
    e->held = false;
    return st != OK ? st : u;
}

static status_t dma_run(struct edu *e, uint64_t src, uint64_t dst, uint32_t len, uint32_t dir)
{
    if (rd(e, R_DMA_CMD) & DMA_RUN)
        return ERR_BAD_STATE;   /* someone else's transfer: never overlap */
    e->seen &= ~IRQ_DMA;
    drv_write64(e->regs, R_DMA_SRC, src);
    drv_write64(e->regs, R_DMA_DST, dst);
    drv_write64(e->regs, R_DMA_CNT, len);
    drv_write64(e->regs, R_DMA_CMD, DMA_RUN | dir | DMA_IRQ);
    return OK;
}

static void fill(struct edu *e, uint32_t len)
{
    e->seq++;
    for (uint32_t i = 0; i < len; i++)
        e->buf[i] = (uint8_t)(i * 7 + e->seq * 13 + (i >> 8));
    for (uint32_t i = 0; i < len; i++)
        e->buf[PAGE + i] = 0;
}

static status_t do_dma_roundtrip(void *ctx, uint32_t len)
{
    struct edu *e = ctx;
    if (len == 0 || len > BUF_SIZE)
        return ERR_INVALID_ARGS;
    status_t st = finish_held(e);
    if (st != OK)
        return st;
    fill(e, len);
    uint64_t addrs[2], pin;
    st = drv_vmo_pin(e->vmo, e->dma, 0, 2 * PAGE, addrs, &pin);
    if (st != OK)
        return st;
    /* RAM (page 0) -> the device's buffer -> RAM (page 1). */
    st = dma_run(e, addrs[0], BUF_ADDR, len, 0);
    if (st == OK)
        st = wait_irq(e, IRQ_DMA, drv_clock_ns() + OP_TIMEOUT, NULL);
    if (st == OK)
        st = dma_run(e, BUF_ADDR, addrs[1], len, DMA_TO_RAM);
    if (st == OK)
        st = wait_irq(e, IRQ_DMA, drv_clock_ns() + OP_TIMEOUT, NULL);
    status_t u = drv_vmo_unpin(e->vmo, e->dma, pin);
    if (st != OK)
        return st;
    if (u != OK)
        return u;
    for (uint32_t i = 0; i < len; i++)
        if (e->buf[PAGE + i] != e->buf[i]) {
            drv_log("DMA round trip: byte %u came back %#x, sent %#x", i, e->buf[PAGE + i],
                    e->buf[i]);
            return ERR_INTERNAL;   /* the data did not survive */
        }
    return OK;
}

static status_t do_dma_start(void *ctx, uint32_t len, uint64_t *out_addr)
{
    struct edu *e = ctx;
    if (len == 0 || len > BUF_SIZE)
        return ERR_INVALID_ARGS;
    status_t st = finish_held(e);
    if (st != OK)
        return st;
    fill(e, len);
    uint64_t addrs[2], pin;
    st = drv_vmo_pin(e->vmo, e->dma, 0, 2 * PAGE, addrs, &pin);
    if (st != OK)
        return st;
    st = dma_run(e, addrs[0], BUF_ADDR, len, 0);
    if (st != OK) {
        drv_vmo_unpin(e->vmo, e->dma, pin);
        return st;
    }
    e->held = true;
    e->held_pin = pin;
    *out_addr = addrs[0];
    return OK;
}

/* ---- the protocol --------------------------------------------------------------- */

static status_t do_factorial(void *ctx, uint32_t n, uint32_t *out)
{
    struct edu *e = ctx;
    status_t st = finish_held(e);
    return st == OK ? fact_irq(e, n, out) : st;
}

static status_t do_raise_irq(void *ctx, uint64_t *out_latency_ns)
{
    struct edu *e = ctx;
    status_t st = finish_held(e);
    if (st != OK)
        return st;
    e->seen &= ~IRQ_TEST;
    uint64_t t0 = drv_clock_ns(), t1 = 0;
    wr(e, R_IRQ_RAISE, IRQ_TEST);
    st = wait_irq(e, IRQ_TEST, t0 + OP_TIMEOUT, &t1);
    if (st == OK)
        *out_latency_ns = t1 - t0;
    return st;
}

static const struct edu_ops ops = {
    .factorial = do_factorial,
    .dma_roundtrip = do_dma_roundtrip,
    .raise_irq = do_raise_irq,
    .dma_start = do_dma_start,
};

/* ---- start ------------------------------------------------------------------------ */

static int setup(const struct driver_start *s, struct edu *e)
{
    handle_t bar = drv_handle(s, DR_BAR(0)), dev = drv_handle(s, DR_PCIDEV);
    e->irq = drv_handle(s, DR_IRQ(0));
    e->dma = drv_handle(s, DR_DMA);
    if (bar == HANDLE_INVALID || e->irq == HANDLE_INVALID || e->dma == HANDLE_INVALID) {
        drv_log("missing handles (BAR 0 %#x, IRQ 0 %#x, DMA %#x)", bar, e->irq, e->dma);
        return 2;
    }
    uint32_t ids = 0;
    if (dev != HANDLE_INVALID && drv_pci_config_read(dev, 0, 4, &ids) == OK &&
        ids != 0x11e81234u) {
        drv_log("not an edu device: %04x:%04x", ids & 0xffff, ids >> 16);
        return 2;
    }
    status_t st = drv_mmio_map(bar, 0, PAGE, VMO_CACHE_UC, &e->regs);
    if (st != OK) {
        drv_log("can't map BAR 0 (%s)", status_str(st));
        return 3;
    }
    uint32_t id = rd(e, R_ID);
    if ((id & 0xff) != 0xed) {
        drv_log("identification %#x: not an edu", id);
        return 4;
    }
    wr(e, R_LIVE, 0x12345678u);
    uint32_t live = rd(e, R_LIVE);
    if (live != ~0x12345678u) {
        drv_log("liveness: wrote 0x12345678, read %#x", live);
        return 4;
    }
    uint32_t f = 0;
    st = fact_poll(e, 12, &f);
    if (st != OK || f != fact_expected(12)) {
        drv_log("factorial(12) by polling: %u (%s)", f, status_str(st));
        return 4;
    }
    /* Quiesce before bus mastering goes on. A driver before us may
     * have died in the middle of a transfer: the device finishes it by
     * itself, reaching no memory while Bus Master Enable is off (it went
     * off with that driver's dma_cap, and our new cap starts with it off).
     * Wait for it, clear any stale interrupt status (it would look like a
     * completion), and only then turn bus mastering on: with it on, that
     * transfer would have written into the dead driver's pages. */
    uint64_t deadline = drv_clock_ns() + OP_TIMEOUT;
    while (rd(e, R_DMA_CMD) & DMA_RUN) {
        if (drv_clock_ns() > deadline) {
            drv_log("the device's DMA engine stays busy");
            return 4;
        }
        drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    }
    wr(e, R_IRQ_ACK, 0xffffffffu);
    st = drv_dma_bus_master(e->dma, 1);   /* DMA, and MSI delivery */
    if (st != OK) {
        drv_log("can't turn bus mastering on (%s)", status_str(st));
        return 4;
    }

    st = drv_port_create(&e->port);
    if (st == OK)
        st = drv_port_bind(e->port, e->irq, IRQ_KEY, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_log("can't bind the interrupt to a port (%s)", status_str(st));
        return 5;
    }
    /* Two pages below 4 GiB for DMA (the call passes our DR_DMA cap). */
    st = drv_vmo_create(2 * PAGE, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &e->vmo);
    void *m = NULL;
    if (st == OK)
        st = drv_vmo_map(e->vmo, 0, 2 * PAGE, VMAR_READ | VMAR_WRITE, &m);
    if (st != OK) {
        drv_log("no DMA buffer (%s)", status_str(st));
        return 6;
    }
    e->buf = m;
    drv_log("edu rev %u.%u: liveness ok, factorial(12) = %u by polling; serving", id >> 24,
            (id >> 16) & 0xff, f);
    return 0;
}

int driver_main(const struct driver_start *s)
{
    struct edu *e = drv_malloc(sizeof(*e));
    if (!e)
        return 1;
    *e = (struct edu){ 0 };
    int r = setup(s, e);
    if (r)
        return r;
    handle_t ch = drv_handle(s, DR_SERVE);
    if (ch == HANDLE_INVALID) {
        drv_log("no DR_SERVE channel: nothing to serve");
        return 0;
    }
    status_t st = edu_serve(ch, &ops, e);
    status_t f = finish_held(e);
    drv_log("client gone after %lu interrupt(s) (%s)", (unsigned long)e->irqs,
            st == OK ? "closed" : status_str(st));
    return st == OK && f == OK ? 0 : 1;
}
