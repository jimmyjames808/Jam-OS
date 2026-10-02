/* netstack: the listen permission (listen.h has the model): the second
 * shared channel, whose openers may listen, and the rules that ask. It is
 * served as clients.c serves /svc/net's, a budget a turn; its openers are
 * the same table's (progs_connect), counted in the same NET_OPENERS. */
#include <idl/svc.h>
#include "listen.h"

#define KEY_NET_LISTEN 3u   /* the shared channel (main.c's keys are below 0x10) */

static handle_t shared;      /* /svc/net-listen's server end (0: none, or its clients gone) */
static bool     pending;     /* requests may be queued on it */
static bool     listening = true;   /* progs_connect's ctx: its openers may listen */

status_t listen_init(handle_t port, handle_t ch)
{
    if (!ch) {
        nstack_log("started without /svc/net-listen's channel (SR_USER + 2): no program may "
                   "listen");
        return OK;
    }
    shared = ch;
    pending = true;   /* connects may be queued from before a restart */
    return jam_port_bind(port, ch, KEY_NET_LISTEN, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_PERSISTENT);
}

bool listen_packet(const struct port_packet *p)
{
    if (p->key != KEY_NET_LISTEN)
        return false;
    pending = shared != 0;
    return true;
}

void listen_serve(void)
{
    if (!shared || !pending)
        return;
    pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = svc_serve_request(shared, progs_shared_dispatch, progs_connect, &listening);
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT) {
            /* Every holder is gone, init's duplicate too: no new listeners. */
            nstack_log("/svc/net-listen's channel is closed (%s): no new listeners",
                       status_str(st));
            jam_handle_close(shared);   /* its binding goes with our only handle */
            shared = HANDLE_INVALID;
        }
        return;
    }
    pending = true;
}

bool listen_pending(void)
{
    return shared && pending;
}

bool listen_may_bind(const struct opener *o, uint16_t port)
{
    if (!port || port >= NET_PORT_EPHEMERAL)
        return true;   /* netstack's pick, or a port no server is known by */
    return port >= NET_PORT_LOW && o->listen;
}

bool listen_may_accept(const struct opener *o)
{
    return o->listen;
}
