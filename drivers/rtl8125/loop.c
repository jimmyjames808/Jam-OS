/* rtl8125: the driver's one event loop (drv/rtl8125).
 *
 * One port carries everything the driver waits for: the chip's MSI-X
 * vector (received frames, finished transmits and link changes, all on
 * the one vector devmgr made) and devmgr closing our channel (it is
 * stopping). Every wait has a deadline of at most a second, so a lost
 * interrupt costs time, never a stall, and the link and the rings are
 * polled then too. Nothing waits inside a step but the short, bounded
 * register waits (the service-loop rule, CODING-GUIDE.md).
 *
 * An interrupt is handled as rge_intr does: the mask off while the
 * status is read and acknowledged, the work, then the mask on again, so
 * status that came meanwhile fires anew. */
#include "rtl8125.h"

#define KEY_IRQ      1
#define KEY_SERVE    2
#define POLL_MAX_NS  NS_PER_S

status_t loop_init(struct rtl *t)
{
    status_t st = drv_port_create(&t->port);
    if (st == OK)
        st = drv_port_bind(t->port, t->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st == OK && t->serve != HANDLE_INVALID)
        st = drv_port_bind(t->port, t->serve, KEY_SERVE, SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    return st;
}

/* The rings and the link: whatever the chip has done since the last look. */
static void service(struct rtl *t, bool by_irq)
{
    if (t->mode == RTL_MODE_PROBE) {
        (void)census_harvest(t, by_irq);
    } else {
        (void)rx_harvest(t);
        (void)tx_reap(t);
    }
    (void)chip_link_poll(t);
}

static void interrupt(struct rtl *t, uint64_t fires)
{
    t->ev.irqs += (uint32_t)fires;
    (void)drv_interrupt_ack(t->irq);   /* before the status: a new fire is not lost */
    wr32(t, RTL_IMR, 0);
    uint32_t isr = rd32(t, RTL_ISR);
    if (isr == 0xffffffffu)
        isr = 0;   /* the chip is gone: its registers read all ones */
    if (isr)
        wr32(t, RTL_ISR, isr);
    if (t->mode == RTL_MODE_PROBE && (isr & RTL_ISR_TX_ANY) && !(t->ev.isr_seen & RTL_ISR_TX_ANY))
        drv_log("WARNING: transmit status bits in isr %#x", isr);
    t->ev.isr_seen |= isr;
    t->ev.linkchg_irqs += !!(isr & RTL_ISR_LINKCHG);
    service(t, true);
    wr32(t, RTL_IMR, t->mode == RTL_MODE_PROBE ? RTL_IMR_PROBE : RTL_IMR_FULL);
}

bool loop_step(struct rtl *t, uint64_t deadline)
{
    uint64_t cap = drv_clock_ns() + POLL_MAX_NS;
    struct port_packet p;
    status_t st = drv_port_wait(t->port, deadline < cap ? deadline : cap, &p);
    if (st == OK && p.key == KEY_SERVE)
        return false;
    if (st == OK && p.key == KEY_IRQ) {
        interrupt(t, p.signal.count);
        return true;
    }
    if (st != OK && st != ERR_TIMED_OUT) {
        drv_log("port wait failed (%s): polling", status_str(st));
        delay_us(1000);
    }
    t->ev.polls++;
    service(t, false);
    return true;
}

bool loop_until(struct rtl *t, uint64_t deadline, uint64_t poll_ns,
                bool (*done)(const struct rtl *t))
{
    for (uint64_t now = drv_clock_ns(); now < deadline; now = drv_clock_ns()) {
        if (done && done(t))
            return true;
        uint64_t next = now + poll_ns;
        if (!loop_step(t, next < deadline ? next : deadline))
            return false;
    }
    return true;
}
