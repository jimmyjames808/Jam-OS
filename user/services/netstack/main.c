/* netstack: the network stack, lwIP in one process (docs/M9-PLAN.md
 * "netstack: lwIP, single-threaded"). init starts it (its net.c) with
 *   SR_USER + 0       the server end of its control channel
 *                     (abi/idl/netctl.idl); init keeps a duplicate, so a
 *                     restarted netstack serves the same channel
 *   SR_DEVMGR_DEVICE  devmgr's device channel for each network card
 *                     (client ends, one handle each, at most DEV_CARDS;
 *                     none without a card: the link stays down). Through
 *                     them netstack alone reaches the cards' drivers.
 *                     The first one's closing (devmgr died, with every
 *                     driver) ends netstack, and init starts it again
 *                     with the new devmgr's
 *   SR_USER + 1       the server end of /svc/net's shared channel
 *                     (abi/idl/net.idl), programs' sockets and pings
 *                     (progs.h); init keeps a duplicate and publishes the
 *                     client end
 *
 * This file is the loop: one port, and lwIP's timers, the programs'
 * timeouts and the next reconnect as the port wait's deadline (and
 * connect.c's thread, which makes the calls that may wait). Nothing in the loop blocks
 * (ARCHITECTURE.md "How a service waits"): every request is answered at
 * once, frames are handled to the end as they arrive, and lwIP needs no
 * locks. The control channel is bound PERSISTENT and served a budget at a
 * time, with a flag saying more may be queued (a binding fires on edges
 * only); the card's keys are netif.c's (dev.h). */
#include <os.h>
#include "ctl.h"
#include "dev.h"
#include "progs.h"
#include "stack.h"

#define SR_NETCTL (SR_USER + 0)
#define SR_NET    (SR_USER + 1)
#define KEY_CTL   1u
#define RX_TICK   (10 * NS_PER_S)   /* rx_tick's line: at most one in 10 s */
#define PACKETS_PER_TURN 32u        /* port packets taken a turn (each notes work) */

struct loop {
    handle_t   port;
    handle_t   ctl;           /* netctl's server end, 0 once its clients are all gone */
    bool       ctl_pending;   /* it may have requests queued */
    struct dev dev;           /* the network card */
    uint64_t   rx_tick_at;    /* when rx_tick last logged (ns) */
    uint64_t   rx_tick_taken; /* frames taken off the ring then */
};

static struct loop l;

static void report(struct dev_report *out)
{
    dev_get_report(&l.dev, out);
}

/* "rx so far", next to the driver's: frames taken off the rx ring, what
 * lwIP did with them, and its receive buffers (a leak shows as buffers in
 * use that never go back, then frames refused for want of one). At most
 * every 10 s, and only while frames come. Next to the deadline it is
 * looked at once a turn (a frame's arrival is a turn). */
static void rx_tick(void)
{
    struct dev_report r;
    dev_get_report(&l.dev, &r);
    uint64_t t = now();
    if (t - l.rx_tick_at < RX_TICK || r.rx_taken == l.rx_tick_taken)
        return;
    l.rx_tick_at = t;
    l.rx_tick_taken = r.rx_taken;
    struct stack_counts c;
    stack_get_counts(&c);
    /* printf, not nstack_log: the line is longer than its 160 characters */
    printf("netstack: rx so far: %lu off the ring (%lu bad), %lu into lwIP (%lu refused); "
           "buffers %u in use, %u at most, %u times none; dropped link %u arp %u ip %u icmp %u "
           "udp %u; %lu pings answered\n", (unsigned long)r.rx_taken, (unsigned long)r.rx_bad,
           (unsigned long)c.rx_frames, (unsigned long)c.rx_refused, c.rx_buffers_used,
           c.rx_buffers_most, c.rx_buffers_none, c.link_dropped, c.arp_dropped, c.ip_dropped,
           c.icmp_dropped, c.udp_dropped, (unsigned long)c.echo_replies);
}

static void serve_ctl(void)
{
    status_t st = ctl_serve(l.ctl);
    l.ctl_pending = st == OK;   /* the budget was spent: more may be queued */
    if (st == OK || st == ERR_SHOULD_WAIT)
        return;
    /* Its clients are gone (or the channel broke): the network keeps the
     * address it has; nobody can change it until netstack starts again. */
    nstack_log("its control channel is closed (%s): going on with the address it has",
               status_str(st));
    (void)jam_port_unbind(l.port, l.ctl, KEY_CTL);   /* nothing left to undo if it fails */
    jam_handle_close(l.ctl);
    l.ctl = 0;
}

/* Wait for the port until `deadline` (0: don't wait), then take what else
 * is queued without waiting, PACKETS_PER_TURN at most: each packet only
 * notes work, which the next turn does. OK, or the port's failure. */
static status_t take_packets(uint64_t deadline)
{
    for (unsigned k = 0; k < PACKETS_PER_TURN; k++) {
        struct port_packet p;
        status_t st = jam_port_wait(l.port, k ? 0 : deadline, &p);
        if (st == ERR_TIMED_OUT)
            return OK;
        if (st != OK)
            return st;
        if (p.key == KEY_CTL)
            l.ctl_pending = l.ctl != 0;
        else if (!progs_packet(&p))
            dev_packet(&l.dev, &p);
    }
    return OK;
}

static status_t setup(void)
{
    l.ctl = startup_handle(SR_NETCTL);
    if (!l.ctl) {
        printf("netstack: started without its control channel (SR_USER + 0): ending\n");
        return ERR_BAD_HANDLE;
    }
    status_t st = jam_port_create(&l.port);
    if (st == OK)
        st = jam_port_bind(l.port, l.ctl, KEY_CTL, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = stack_start(&stack_no_device);
    if (st == OK)
        st = dev_init(&l.dev, l.port);
    if (st == OK)
        st = progs_init(l.port, startup_handle(SR_NET), &l.dev);
    if (st != OK) {
        printf("netstack: can't set up (%s)\n", status_str(st));
        return st;
    }
    ctl_device_report = report;
    l.ctl_pending = true;   /* requests may be queued from before a restart */
    return OK;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (setup() != OK)
        return 1;
    nstack_log("serving");
    for (;;) {
        if (l.ctl_pending)
            serve_ctl();
        progs_serve();
        uint64_t deadline = stack_poll();
        uint64_t t = progs_tick();
        if (t < deadline)
            deadline = t;
        uint64_t retry = dev_work(&l.dev);   /* last: it sends what the others queued */
        rx_tick();
        if (retry < deadline)
            deadline = retry;
        /* Work left over (a budget ran out): look at the port without
         * sleeping rather than skip it, so a channel that always has more
         * never keeps the other channels' packets, or the card's, unread
         * (the service-loop rule: a busy client delays only itself). */
        if (l.ctl_pending || dev_pending(&l.dev) || progs_pending())
            deadline = 0;
        if (take_packets(deadline) != OK)
            break;
    }
    printf("netstack: its port failed: ending\n");
    return 1;
}
