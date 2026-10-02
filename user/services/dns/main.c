/* dns: the resolver process (docs/M9-PLAN.md, "DHCP and DNS: processes
 * of their own"; dnsd.h says how its files split the work). init starts
 * it in shell mode once netstack runs, with
 *   SR_USER + 0   the server end of /svc/dns's shared channel
 *                 (abi/idl/dns.idl); init keeps both ends, so a restarted
 *                 resolver serves the same channel, and publishes the
 *                 client end (a channel per opener)
 *   SR_NS         /svc/net and nothing else: its sockets, and the DNS
 *                 servers netstack has (from the settings or DHCP)
 * It parses what strangers on the network send (msg.c), so it holds
 * nothing else.
 *
 * The servers: net.iface's DNS servers go to dns_set_servers at the
 * start and again each time net.wait_change answers (a new lease, a new
 * static address); both calls are written without waiting, on the
 * resolver's own opener channel, which also carries the sockets' opens
 * (socks.c). The loop's wait ends at the earliest of the resolver's
 * deadline (dns_deadline) and the askers' timeouts. */
#include "dnsd.h"

#define RETRY (1 * NS_PER_S)   /* a failed iface or wait_change is asked again after this */
#define PACKETS_PER_TURN 32u   /* port packets taken a turn (each notes work) */

struct dnsd D;

/* Ask for the interface (its DNS servers) without waiting. */
static void ask_iface(void)
{
    D.iface_txid = idl_txid_next(&D.net_txid);
    if (net_iface_send(D.net, D.iface_txid) != OK) {
        D.iface_txid = 0;
        D.iface_retry = now() + RETRY;
    }
}

static void got_iface(const void *rep, struct idl_msg *m)
{
    uint32_t addr, mask, gw, dns[2], speed, version;
    uint8_t mac[6], device, link;
    uint16_t vlan;
    D.iface_txid = 0;
    status_t st = net_iface_result(rep, m, &addr, &mask, &gw, &dns[0], &dns[1], mac, &device,
                                   &link, &vlan, &speed, &version);
    if (st != OK) {
        D.iface_retry = now() + RETRY;
        return;
    }
    uint32_t had[DNS_MAX_SERVERS];
    unsigned nhad = D.r.nservers;
    memcpy(had, D.r.servers, sizeof(had));
    dns_set_servers(&D.r, dns, 2);
    if (D.r.nservers != nhad || memcmp(had, D.r.servers, nhad * sizeof(uint32_t)))
        printf("dns: %u DNS server(s) from netstack\n", D.r.nservers);   /* once per change */
    D.version = version;
    D.wait_txid = idl_txid_next(&D.net_txid);
    if (net_wait_change_send(D.net, D.wait_txid, version, NET_WAIT_FOREVER) != OK) {
        D.wait_txid = 0;
        D.iface_retry = now() + RETRY;
    }
}

/* What came on the opener channel. ERR_PEER_CLOSED: netstack is gone. */
static status_t serve_net(void)
{
    D.net_pending = false;
    for (unsigned k = 0; k < BUDGET; k++) {
        _Alignas(8) uint8_t rep[NET_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(D.net, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return OK;
        if (st == ERR_PEER_CLOSED || st == ERR_NO_MEMORY)
            return st;
        if (st != OK) {
            idl_msg_drop(&m);
            continue;
        }
        if (m.txid && m.txid == D.iface_txid) {
            got_iface(rep, &m);
        } else if (m.txid && m.txid == D.wait_txid) {
            uint32_t v;
            D.wait_txid = 0;
            if (net_wait_change_result(rep, &m, &v) == OK)
                ask_iface();
            else
                D.iface_retry = now() + RETRY;
        } else if (!socks_open_reply(rep, &m)) {
            idl_msg_drop(&m);   /* nobody's (a call that went with a restart) */
        }
    }
    D.net_pending = true;
    return OK;
}

static status_t setup(void)
{
    handle_t shared = startup_handle(SR_DNS);
    if (!shared) {
        printf("dns: started without /svc/dns's channel (SR_USER + 0): ending\n");
        return ERR_BAD_HANDLE;
    }
    /* Before anything is served: nobody waits on us yet. */
    status_t st = net_svc_open(&D.net);
    if (st != OK) {
        printf("dns: no /svc/net (%s): ending\n", status_str(st));
        return st;
    }
    socks_init();
    dns_init(&D.r, &D.io);
    st = jam_port_create(&D.port);
    if (st == OK)
        st = jam_port_bind(D.port, D.net, KEY_NET, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = askers_init(shared);
    if (st != OK) {
        printf("dns: can't set up (%s)\n", status_str(st));
        return st;
    }
    ask_iface();
    return OK;
}

/* One turn's work; the next deadline. ERR_PEER_CLOSED in *st: netstack is gone. */
static uint64_t turn(status_t *st)
{
    if (D.net_pending)
        *st = serve_net();
    if (*st == OK)
        *st = socks_serve();
    askers_serve();
    uint64_t t = now();
    if (D.iface_retry && t >= D.iface_retry && !D.iface_txid && !D.wait_txid) {
        D.iface_retry = 0;
        ask_iface();
    }
    dns_tick(&D.r, t);
    uint64_t deadline = askers_tick(t), d = dns_deadline(&D.r);
    if (d < deadline)
        deadline = d;
    if (D.iface_retry && D.iface_retry < deadline)
        deadline = D.iface_retry;
    return deadline;
}

/* Wait for the port until `deadline` (0: don't wait), then take what else
 * is queued without waiting, PACKETS_PER_TURN at most: each packet only
 * notes work, which the next turn does. OK, or the port's failure. */
static status_t take_packets(uint64_t deadline)
{
    for (unsigned k = 0; k < PACKETS_PER_TURN; k++) {
        struct port_packet p;
        status_t st = jam_port_wait(D.port, k ? 0 : deadline, &p);
        if (st == ERR_TIMED_OUT)
            return OK;
        if (st != OK)
            return st;
        uint32_t low = (uint32_t)(p.key & 0xff);
        if (p.key == KEY_NET)
            D.net_pending = true;
        else if (low >= KEY_SOCK && low < KEY_SOCK + SOCK_SLOTS)
            socks_packet(low - KEY_SOCK, (uint32_t)(p.key >> 8));
        else
            askers_packet(p.key);
    }
    return OK;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (setup() != OK)
        return 1;
    printf("dns: serving /svc/dns\n");
    for (;;) {
        status_t st = OK;
        uint64_t deadline = turn(&st);
        if (st != OK)
            break;
        /* Work left over (a budget ran out): look at the port without
         * sleeping rather than skip it, so an asker that always has more
         * never keeps the sockets' replies or the other askers unread (the
         * service-loop rule: a busy client delays only itself). */
        if (D.net_pending || socks_pending() || askers_pending())
            deadline = 0;
        st = take_packets(deadline);
        if (st != OK) {
            printf("dns: its port failed (%s): ending\n", status_str(st));
            return 1;
        }
    }
    /* Its askers see ERR_PEER_CLOSED and ask the next resolver (svc_get). */
    printf("dns: netstack is gone: ending (init starts it again)\n");
    return 1;
}
