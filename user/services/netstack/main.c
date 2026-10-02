/* netstack: the network stack, lwIP in one process (docs/M9-PLAN.md
 * "netstack: lwIP, single-threaded"). It is started with
 *   SR_USER + 0  the server end of its control channel (abi/idl/netctl.idl)
 * and nothing else yet. Not built yet: the NIC's devmgr device channel and
 * its netdev rings (stage 3b plugs them into stack.h's edge), init
 * starting it (planned user/services/init/net.c), and /svc/net for
 * programs. Until then it runs with no device: link down, nothing sent.
 *
 * This file is the loop: one thread, one port, and lwIP's timers as the
 * port wait's deadline. Nothing in it blocks (ARCHITECTURE.md "How a
 * service waits"): every request is answered at once, and frames are
 * handled to the end as they arrive, so lwIP needs no locks. The control
 * channel is bound PERSISTENT and served a budget at a time, with a flag
 * saying more may be queued (a binding fires on edges only). */
#include <os.h>
#include "ctl.h"
#include "stack.h"

#define SR_NETCTL (SR_USER + 0)
#define KEY_CTL   1u

struct loop {
    handle_t port;
    handle_t ctl;           /* netctl's server end, 0 once its clients are all gone */
    bool     ctl_pending;   /* it may have requests queued */
};

static void serve_ctl(struct loop *l)
{
    status_t st = ctl_serve(l->ctl);
    l->ctl_pending = st == OK;   /* the budget was spent: more may be queued */
    if (st == OK || st == ERR_SHOULD_WAIT)
        return;
    /* Its clients are gone (or the channel broke): the network keeps the
     * address it has; nobody can change it until netstack starts again. */
    nstack_log("its control channel is closed (%s): going on with the address it has",
               status_str(st));
    (void)jam_port_unbind(l->port, l->ctl, KEY_CTL);   /* nothing left to undo if it fails */
    jam_handle_close(l->ctl);
    l->ctl = 0;
}

static status_t setup(struct loop *l)
{
    l->ctl = startup_handle(SR_NETCTL);
    if (!l->ctl) {
        printf("netstack: started without its control channel (SR_USER + 0): ending\n");
        return ERR_BAD_HANDLE;
    }
    status_t st = jam_port_create(&l->port);
    if (st == OK)
        st = jam_port_bind(l->port, l->ctl, KEY_CTL, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = stack_start(&stack_no_device);
    if (st != OK) {
        printf("netstack: can't set up (%s)\n", status_str(st));
        return st;
    }
    l->ctl_pending = true;   /* requests may be queued from before a restart */
    return OK;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct loop l = { 0 };
    if (setup(&l) != OK)
        return 1;
    nstack_log("serving; no network device yet: the link is down and nothing is sent");
    for (;;) {
        if (l.ctl_pending)
            serve_ctl(&l);
        uint64_t deadline = stack_poll();
        if (l.ctl_pending)
            continue;
        struct port_packet p;
        status_t st = jam_port_wait(l.port, deadline, &p);
        if (st == OK && p.key == KEY_CTL && l.ctl)
            l.ctl_pending = true;
        else if (st != OK && st != ERR_TIMED_OUT)
            break;
    }
    printf("netstack: its port failed: ending\n");
    return 1;
}
