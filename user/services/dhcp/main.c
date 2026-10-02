/* dhcp: the DHCP client process (docs/M9-PLAN.md, "DHCP and DNS:
 * processes of their own"). init starts it in shell mode once netstack
 * runs, unless /data/etc/settings has a static `net.address` (that wins:
 * init says so in the log), with
 *   SR_USER + 0   a client end of netstack's control channel
 *                 (abi/idl/netctl.idl): the DHCP socket (dhcp_open), the
 *                 address (set_ipv4, set_dns, clear) and the link (info)
 * and nothing else: it parses what strangers on the network send (msg.c).
 *
 * The loop waits on one port for the DHCP socket's datagrams (port 68,
 * each to dhcp_input) and for the earliest of the client's deadline
 * (dhcp_tick at dhcp_deadline) and the next look at the link. It serves
 * nobody, so its calls to netstack may block (each with a short
 * deadline): netctl.info once a second for the link (netctl has no wait
 * for a change), and the edge's set_ipv4/set_dns/clear.
 *
 * The edge (struct dhcp_io): send through the socket without waiting;
 * bound: set_ipv4 and set_dns, the lease logged once (bound is called
 * only for a new lease or one a renewal changed); unbound: clear, logged;
 * no ARP probe (RFC 5227): netstack has no way to send one and hear the
 * answer yet, so an ACKed address is taken as free; random: the kernel's
 * generator.
 *
 * Link up (with the socket open and the card's MAC known): dhcp_start,
 * asking for the last lease's address first (INIT-REBOOT); link down:
 * dhcp_stop (the address is cleared). netstack restarting closes the
 * socket: the client stops, the socket is asked for again every second,
 * and the client starts over with the lease's address once it is back
 * (the new netstack has no address). A restart of this process keeps
 * nothing of its own; if netstack still has an address then (the last
 * dhcp's lease), it is cleared and asked for again. */
#include <idl/netctl.h>
#include <ipv4.h>
#include <net.h>
#include "dhcp.h"

#define SR_NETCTL (SR_USER + 0)
#define KEY_SOCK  1u
#define POLL      NS_PER_S          /* the link is looked at this often */
#define CALL_WAIT (1 * NS_PER_S)    /* each call to netstack */

struct dhcpd {
    handle_t ctl;              /* netctl, client end */
    handle_t port;
    struct net_sock s;         /* the DHCP socket (s.ch 0: none) */
    struct dhcp_client c;
    struct dhcp_io io;
    bool     have_mac;         /* c.mac is the card's */
    bool     link;             /* the link was up at the last look */
    uint32_t last;             /* the last lease's address: asked for first (0: none) */
    uint64_t next_poll;        /* the next look at the link */
    uint64_t next_open;        /* without a socket: the next try to open it */
    bool     more;             /* the socket may have datagrams still queued */
};

static struct dhcpd D;

/* ---- the edge ------------------------------------------------------------------ */

static status_t io_send(void *ctx, uint32_t to, const void *msg, size_t len)
{
    (void)ctx;
    if (!D.s.ch)
        return ERR_BAD_STATE;
    return net_sendto_async(&D.s, to, DHCP_SERVER_PORT, msg, len);
}

static void io_bound(void *ctx, const struct dhcp_lease *l)
{
    (void)ctx;
    char a[IPV4_TEXT_MAX], g[IPV4_TEXT_MAX], s[IPV4_TEXT_MAX], d[IPV4_TEXT_MAX];
    D.last = l->addr;
    uint32_t router = l->router;
    status_t st = netctl_set_ipv4_until(D.ctl, now() + CALL_WAIT, l->addr, l->mask, router);
    if (st == ERR_INVALID_ARGS && router) {
        router = 0;   /* a router outside the subnet: the subnet alone is reachable */
        st = netctl_set_ipv4_until(D.ctl, now() + CALL_WAIT, l->addr, l->mask, 0);
    }
    if (st == OK)
        st = netctl_set_dns_until(D.ctl, now() + CALL_WAIT, l->ndns > 0 ? l->dns[0] : 0,
                                  l->ndns > 1 ? l->dns[1] : 0);
    unsigned prefix = l->mask ? 32u - (unsigned)__builtin_ctz(l->mask) : 0u;
    printf("dhcp: lease %s/%u from %s: gateway %s, DNS %s, %u s%s%s\n", ipv4_format(l->addr, a),
           prefix, ipv4_format(l->server, s), router ? ipv4_format(router, g) : "none",
           l->ndns ? ipv4_format(l->dns[0], d) : "none", (unsigned)l->lease_s,
           st == OK ? "" : ": netstack didn't take it: ", st == OK ? "" : status_str(st));
}

static void io_unbound(void *ctx, enum dhcp_why why)
{
    (void)ctx;
    char a[IPV4_TEXT_MAX];
    status_t st = netctl_clear_until(D.ctl, now() + CALL_WAIT);
    printf("dhcp: the lease of %s ended (%s)%s%s\n", ipv4_format(D.c.lease.addr, a),
           why == DHCP_WHY_EXPIRED ? "it expired, no server answered"
           : why == DHCP_WHY_NAK   ? "the server said no"
                                   : "stopped",
           st == OK || st == ERR_PEER_CLOSED ? "" : ": netstack didn't clear it: ",
           st == OK || st == ERR_PEER_CLOSED ? "" : status_str(st));
}

static uint32_t io_random(void *ctx)
{
    (void)ctx;
    return os_random_u32();
}

/* ---- the socket and the link ------------------------------------------------------ */

static void open_socket(uint64_t t)
{
    handle_t h = HANDLE_INVALID;
    status_t st = netctl_dhcp_open_until(D.ctl, t + CALL_WAIT, &h);
    if (st == OK)   /* its rings: a call that waits, but dhcp serves nobody */
        st = net_sock_adopt(&D.s, h, NET_PORT_DHCP_CLIENT);
    if (st == OK) {
        st = net_sock_bind(&D.s, D.port, KEY_SOCK, PORT_BIND_PERSISTENT);
        if (st != OK)
            net_close(&D.s);
    }
    if (st != OK)
        D.next_open = t + POLL;   /* netstack restarting, or the last socket not closed yet */
}

/* Look at the link (and the card's MAC, the client id). */
static void poll_link(uint64_t t)
{
    uint32_t addr, mask, gw, d1, d2;
    uint8_t mac[6], device = 0, link = 0;
    D.next_poll = t + POLL;
    if (netctl_info_until(D.ctl, t + CALL_WAIT, &addr, &mask, &gw, &d1, &d2, mac, &device,
                          &link) != OK)
        return;   /* netstack restarting: the socket's end says so */
    bool zero = !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    if (device && !zero && (!D.have_mac || memcmp(mac, D.c.mac, 6))) {
        if (D.c.state != DHCP_STOPPED)
            dhcp_stop(&D.c, t, false);   /* another card: start over as it */
        dhcp_init(&D.c, &D.io, mac);
        D.have_mac = true;
    }
    D.link = device && link;
}

/* Start or stop the client as the socket, the card and the link say. */
static void follow(uint64_t t)
{
    bool run = D.s.ch && D.have_mac && D.link;
    char a[IPV4_TEXT_MAX];
    if (run && D.c.state == DHCP_STOPPED) {
        if (D.last)
            printf("dhcp: link up: asking for %s again\n", ipv4_format(D.last, a));
        else
            printf("dhcp: link up: asking for an address\n");
        dhcp_start(&D.c, t, D.last);
    } else if (!run && D.c.state != DHCP_STOPPED) {
        printf("dhcp: %s: stopped\n", D.s.ch ? "link down" : "netstack is gone");
        dhcp_stop(&D.c, t, false);
    }
}

/* The socket's datagrams to the client; its end (netstack is gone). */
static void serve_socket(void)
{
    D.more = false;
    for (unsigned k = 0; k < 32; k++) {   /* the rest at the next turn: its packets are edges */
        struct net_dgram d;
        status_t st = net_sock_take(&D.s, &d);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st == ERR_PEER_CLOSED) {
            net_close(&D.s);
            D.next_open = now() + POLL;
            return;
        }
        if (st == OK && d.port == DHCP_SERVER_PORT)
            dhcp_input(&D.c, now(), d.data, d.len);
    }
    D.more = true;   /* its budget is spent: the binding fires on edges only */
}

/* ---- the start and the loop -------------------------------------------------------- */

/* netstack may still have the address a dhcp before us had: clear it
 * and ask for it again first. */
static void inherit(void)
{
    uint32_t addr = 0, mask, gw, d1, d2;
    uint8_t mac[6], device, link;
    char a[IPV4_TEXT_MAX];
    if (netctl_info_until(D.ctl, now() + CALL_WAIT, &addr, &mask, &gw, &d1, &d2, mac, &device,
                          &link) != OK || !dhcp_unicast(addr))
        return;
    D.last = addr;
    (void)netctl_clear_until(D.ctl, now() + CALL_WAIT);   /* failed: the next lease replaces it */
    printf("dhcp: netstack had %s from before: cleared, asking for it again\n",
           ipv4_format(addr, a));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    D.ctl = startup_handle(SR_NETCTL);
    if (!D.ctl) {
        printf("dhcp: started without netctl (SR_USER + 0): ending\n");
        return 1;
    }
    status_t st = jam_port_create(&D.port);
    if (st != OK) {
        printf("dhcp: no port (%s): ending\n", status_str(st));
        return 1;
    }
    D.io = (struct dhcp_io){ .ctx = &D, .send = io_send, .bound = io_bound,
                             .unbound = io_unbound, .probe = NULL, .random = io_random };
    uint8_t none[6] = { 0 };
    dhcp_init(&D.c, &D.io, none);
    inherit();
    for (;;) {
        uint64_t t = now();
        if (!D.s.ch && t >= D.next_open)
            open_socket(t);
        if (t >= D.next_poll)
            poll_link(t);
        follow(t);
        dhcp_tick(&D.c, t);
        uint64_t deadline = dhcp_deadline(&D.c);
        if (D.next_poll < deadline)
            deadline = D.next_poll;
        if (!D.s.ch && D.next_open < deadline)
            deadline = D.next_open;
        if (D.more && D.s.ch) {
            serve_socket();
            continue;
        }
        struct port_packet p;
        st = jam_port_wait(D.port, deadline, &p);
        if (st == OK && p.key == KEY_SOCK && D.s.ch)
            serve_socket();
        else if (st != OK && st != ERR_TIMED_OUT)
            break;
    }
    printf("dhcp: its port failed (%s): ending\n", status_str(st));
    return 1;
}
