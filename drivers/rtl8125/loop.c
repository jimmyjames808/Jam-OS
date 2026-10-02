/* rtl8125: the driver's one event loop (drv/rtl8125).
 *
 * One port carries everything the driver waits for: the chip's MSI-X
 * vector (received frames, finished transmits and link changes, all on
 * the one vector devmgr made), devmgr closing our channel (it is
 * stopping), and in full mode the netdev server's packets
 * (<jam/netserver.h>: requests on DR_SERVE and the session channel,
 * netstack's transmit signal), whose work srv_work does at the top of each
 * step. After the chip's work the server is told: the rx ring published
 * once per batch, transmit descriptors freed, the link changed. Every
 * wait has a deadline of at most a second, so a lost interrupt costs time,
 * never a stall, and the link and the rings are polled then too. Nothing
 * waits inside a step but the short, bounded register waits (the
 * service-loop rule, CODING-GUIDE.md).
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
        (void)chip_link_poll(t);
        return;
    }
    (void)rx_harvest(t);
    unsigned freed = tx_reap(t);
    (void)chip_link_poll(t);
    if (!t->srv)
        return;
    srv_rx_done(t->srv);   /* the rx ring published once per batch */
    if (freed)
        srv_tx_room(t->srv);
    if (t->link_seq != t->link_told) {
        t->link_told = t->link_seq;
        srv_link(t->srv);
    }
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

/* The netdev server's work, if there is a server; true while it waits for
 * more (the next port wait then doesn't sleep). */
static bool server_work(struct rtl *t)
{
    return t->srv && srv_work(t->srv);
}

bool loop_step(struct rtl *t, uint64_t deadline)
{
    uint64_t now = drv_clock_ns(), cap = now + POLL_MAX_NS;
    if (server_work(t))
        cap = now;   /* more is waiting: look at the port without sleeping */
    if (t->srv && t->srv->stopping)
        return false;
    struct port_packet p;
    status_t st = drv_port_wait(t->port, deadline < cap ? deadline : cap, &p);
    if (st == OK && p.key == KEY_SERVE)
        return false;
    if (st == OK && p.key == KEY_IRQ) {
        interrupt(t, p.signal.count);
        return true;
    }
    if (st == OK && t->srv && srv_packet(t->srv, &p))
        return true;   /* srv_work does it, at the next step */
    if (st != OK && st != ERR_TIMED_OUT) {
        drv_log("port wait failed (%s): polling", status_str(st));
        delay_us(1000);
    }
    t->ev.polls += cap != now;   /* a wait that didn't sleep is not a poll */
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
