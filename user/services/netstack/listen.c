/* netstack: the listen permissions (listen.h has the model): the shared
 * channels whose openers may listen, /svc/net-listen and
 * /svc/net-low, and the rules that ask. Each is served as
 * clients.c serves /svc/net's, a budget a turn; their openers are the same
 * table's (progs_connect), counted in the same NET_OPENERS. */
#include <idl/svc.h>
#include "listen.h"


enum { PLAIN, LOW, CHANNELS };

/* One shared channel whose openers may listen. */
struct lchan {
    uint64_t                  key;       /* its port key (main.c's 1, clients.c's 2 and 4) */
    handle_t                  ch;        /* its server end (0: none, or its clients gone) */
    bool                      pending;   /* requests may be queued on it */
    const struct opener_kind *kind;      /* progs_connect's ctx: what its openers may do */
    const char               *name;      /* for the log */
};

/* Ordinary openers that may listen, and that may on ports below 1024 too. */
static const struct opener_kind listening = { .cls = CLASS_PROG, .listen = true };
static const struct opener_kind listening_low = { .cls = CLASS_PROG, .listen = true, .low = true };
static struct lchan chans[CHANNELS] = {
    { .key = 3, .kind = &listening, .name = "/svc/net-listen" },
    { .key = 5, .kind = &listening_low, .name = "/svc/net-low" },
};

static status_t chan_init(handle_t port, unsigned i, handle_t ch)
{
    struct lchan *c = &chans[i];
    if (!ch) {
        nstack_log("started without %s's channel: no program may listen%s", c->name,
                   i == LOW ? " on a port below 1024" : "");
        return OK;
    }
    c->ch = ch;
    c->pending = true;   /* connects may be queued from before a restart */
    return jam_port_bind(port, ch, c->key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
}

status_t listen_init(handle_t port, handle_t shared, handle_t shared_low)
{
    status_t st = chan_init(port, PLAIN, shared);
    return st == OK ? chan_init(port, LOW, shared_low) : st;
}

bool listen_packet(const struct port_packet *p)
{
    for (unsigned i = 0; i < CHANNELS; i++) {
        if (p->key == chans[i].key) {
            chans[i].pending = chans[i].ch != 0;
            return true;
        }
    }
    return false;
}

static void chan_serve(struct lchan *c)
{
    if (!c->ch || !c->pending)
        return;
    c->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = svc_serve_request(c->ch, progs_shared_dispatch, progs_connect,
                                        (void *)c->kind);
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT) {
            /* Every holder is gone, init's duplicate too: no new listeners. */
            nstack_log("%s's channel is closed (%s): no new listeners there", c->name,
                       status_str(st));
            jam_handle_close(c->ch);   /* its binding goes with our only handle */
            c->ch = HANDLE_INVALID;
        }
        return;
    }
    c->pending = true;
}

void listen_serve(void)
{
    for (unsigned i = 0; i < CHANNELS; i++)
        chan_serve(&chans[i]);
}

bool listen_pending(void)
{
    for (unsigned i = 0; i < CHANNELS; i++)
        if (chans[i].ch && chans[i].pending)
            return true;
    return false;
}

/* A port below NET_PORT_LOW: a low opener's, but never the DHCP ports. */
static bool low_ok(const struct opener *o, uint16_t port)
{
    return o->low && port != NET_PORT_DHCP_SERVER && port != NET_PORT_DHCP_CLIENT;
}

bool listen_may_bind(const struct opener *o, uint16_t port)
{
    if (!port || port >= NET_PORT_EPHEMERAL)
        return true;   /* netstack's pick, or a port no server is known by */
    return port >= NET_PORT_LOW ? o->listen : low_ok(o, port);
}

bool listen_may_accept(const struct opener *o, uint16_t port)
{
    return o->listen && (!port || port >= NET_PORT_LOW || low_ok(o, port));
}
