/* e1000e: the driver's one loop (drv/e1000e). One thread, one port;
 * everything it waits for arrives there:
 *   KEY_IRQ      the chip's MSI-X vector 0 (receive, transmit written
 *                back, link change: chip.c routes every cause to it),
 *                bound PERSISTENT: acked, then ICR read and cleared
 *   KEY_SERVE    DR_SERVE, the channel devmgr shares among everyone it
 *                hands the service to: info, stats, open; its peer closing
 *                means devmgr is stopping us
 *   KEY_SESSION  the session's channel (| generation << 8): info, stats;
 *                its peer closing ends the session
 *   KEY_TX       the session's to_driver event (| generation << 8):
 *                NETDEV_SIG_TX, netstack put frames into the tx ring
 * Every wait has a deadline of at most a second, so a lost interrupt
 * costs time, never a stall: the rings and the link are looked at then
 * too, and the chip's 32-bit counters are added up before they can wrap.
 * Nothing waits inside a request (the service-loop rule, CODING-GUIDE.md):
 * a full rx ring drops and counts, and a busy channel is served
 * DRAIN_MAX messages per turn. */
#include "e1000e.h"

#define POLL_NS NS_PER_S

/* The rings and the link: whatever the chip has done since the last look. */
static void service(struct e1k *t)
{
    (void)rx_harvest(t);
    if (tx_reap(t) && t->tx_blocked)
        t->s.tx_wake = true;   /* descriptors came back: take the frames left waiting */
    if (chip_link_poll(t))
        session_signal(t, NETDEV_SIG_LINK);
}

static void interrupt(struct e1k *t, uint64_t fires)
{
    t->irqs += fires;
    (void)drv_interrupt_ack(t->irq);   /* before the cause: a new fire is not lost */
    uint32_t icr = rd32(t, E1K_ICR);
    if (icr == 0xffffffffu)
        icr = 0;   /* the chip is gone: its registers read all ones */
    if (icr)
        wr32(t, E1K_ICR, icr);   /* write 1 to clear (with MSI-X a read doesn't) */
    t->icr_seen |= icr;
    service(t);
}

static void packet(struct e1k *t, const struct port_packet *p)
{
    if (p->key == KEY_IRQ)
        interrupt(t, p->signal.count);
    else if (p->key == KEY_SERVE)
        t->serve_pending = true;
    else if (t->s.ch != HANDLE_INVALID && p->key == session_key(t, KEY_SESSION))
        t->s.pending = true;
    else if (t->s.ch != HANDLE_INVALID && p->key == session_key(t, KEY_TX))
        t->s.tx_wake = true;
    /* else an ended session's: stale */
}

static status_t loop_init(struct e1k *t)
{
    status_t st = drv_port_bind(t->port, t->serve, KEY_SERVE, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK && t->irq != HANDLE_INVALID)
        st = drv_port_bind(t->port, t->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    t->serve_pending = true;   /* something may be queued already */
    return st;
}

status_t loop_run(struct e1k *t)
{
    status_t st = loop_init(t);
    if (st != OK) {
        drv_log("can't bind the port (%s)", status_str(st));
        return st;
    }
    uint64_t next_poll = drv_clock_ns() + POLL_NS;
    while (st == OK) {
        if (t->serve_pending)
            serve_some(t, false);
        if (t->serve_closed)
            break;
        if (t->s.pending)
            serve_some(t, true);
        if (t->s.tx_wake)
            tx_pass(t);
        bool busy = t->serve_pending || t->s.pending || t->s.tx_wake;
        struct port_packet p;
        st = drv_port_wait(t->port, busy ? 0 : next_poll, &p);
        if (st == OK) {
            packet(t, &p);
        } else if (st == ERR_TIMED_OUT) {
            st = OK;
        } else {
            drv_log("port wait failed (%s)", status_str(st));
            break;
        }
        if (drv_clock_ns() >= next_poll) {
            t->polls++;
            service(t);
            chip_counters(t);
            next_poll = drv_clock_ns() + POLL_NS;
        }
    }
    return st;
}
