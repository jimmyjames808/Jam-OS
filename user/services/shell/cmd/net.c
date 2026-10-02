/* net: the network as netstack sees it (abi/idl/net.idl, <net.h>): the
 * address, gateway and DNS servers, the link (speed, VLAN, MAC) and the
 * main counts. `net stats`: every count netstack keeps and the network
 * card's own (the driver's netdev.stats). */
#include <ipv4.h>
#include <jam/netdev.h>
#include <net.h>
#include "sh.h"

_Static_assert(NET_VLAN_UNTAGGED == NETFRAME_MODE_UNTAGGED,
               "net.iface passes the driver's mode on as it is");

static unsigned prefix_of(uint32_t mask)
{
    return mask ? 32u - (unsigned)__builtin_ctz(mask) : 0;
}

static void show_iface(const struct net_info *i)
{
    char a[IPV4_TEXT_MAX], b[IPV4_TEXT_MAX];
    if (i->address)
        sh_say("address  %s/%u, gateway %s\n", ipv4_format(i->address, a), prefix_of(i->mask),
               i->gateway ? ipv4_format(i->gateway, b) : "none");
    else
        sh_say("address  none yet (net.address in /data/etc/settings, or DHCP)\n");
    if (i->dns[0] || i->dns[1])
        sh_say("DNS      %s%s%s\n", i->dns[0] ? ipv4_format(i->dns[0], a) : "none",
               i->dns[1] ? ", " : "", i->dns[1] ? ipv4_format(i->dns[1], b) : "");
    else
        sh_say("DNS      none\n");
    if (!i->device) {
        sh_say("link     no network card (or its driver is restarting)\n");
        return;
    }
    sh_say("link     %s", i->link ? "up" : "down");
    if (i->link)
        sh_say(", %u Mb/s", i->speed);
    if (i->vlan == NET_VLAN_UNTAGGED)
        sh_say(", untagged");
    else
        sh_say(", VLAN %u", i->vlan);
    sh_say(", MAC %02x:%02x:%02x:%02x:%02x:%02x\n", i->mac[0], i->mac[1], i->mac[2], i->mac[3],
           i->mac[4], i->mac[5]);
}

static void show_brief(const struct net_counters *c)
{
    sh_say("frames   %lu in (%lu refused), %lu out (%lu dropped); pings answered %lu\n",
           (unsigned long)c->rx_frames, (unsigned long)c->rx_refused,
           (unsigned long)c->tx_frames, (unsigned long)c->tx_dropped,
           (unsigned long)c->echo_replies);
    sh_say("programs %u openers, %u sockets, %u datagrams queued\n", c->openers, c->sockets,
           c->queued);
}

#define ROW(name, v) sh_say("  %-22s %lu\n", name, (unsigned long)(v))

static void show_stats(const struct net_counters *c)
{
    sh_say("netstack:\n");
    ROW("frames in", c->rx_frames);
    ROW("  refused", c->rx_refused);
    ROW("frames out", c->tx_frames);
    ROW("  dropped", c->tx_dropped);
    ROW("pings answered", c->echo_replies);
    ROW("ICMP errors sent", c->icmp_errors);
    ROW("  held back", c->icmp_limited);
    ROW("dropped: link", c->link_dropped);
    ROW("dropped: ARP", c->arp_dropped);
    ROW("dropped: IP", c->ip_dropped);
    ROW("dropped: ICMP", c->icmp_dropped);
    ROW("dropped: UDP", c->udp_dropped);
    ROW("bad checksums", c->bad_checksums);
    ROW("rx buffers in use", c->rx_buffers_used);
    ROW("heap bytes in use", c->heap_used);
    sh_say("the card's session:\n");
    ROW("sessions opened", c->sessions);
    ROW("ring errors", c->ring_errors);
    ROW("rx slots refused", c->rx_bad);
    ROW("tx ring full", c->tx_full);
    sh_say("programs:\n");
    ROW("openers", c->openers);
    ROW("sockets", c->sockets);
    ROW("datagrams queued", c->queued);
    ROW("requests waiting", c->later);
    ROW("datagrams in", c->dgrams_in);
    ROW("  dropped (queue full)", c->dgrams_dropped);
    ROW("datagrams out", c->dgrams_out);
    ROW("pings sent", c->echoes_sent);
    ROW("  answered", c->echoes_answered);
}

static void show_chip(handle_t net)
{
    struct netdev_stats s;
    status_t st = net_get_chip_counters(net, &s);
    if (st != OK) {
        sh_say("the card: %s\n", st == ERR_NOT_FOUND ? "none (no session)" : status_str(st));
        return;
    }
    sh_say("the card's driver:\n");
    ROW("rx frames", s.rx_frames);
    ROW("rx bytes", s.rx_bytes);
    ROW("rx untagged (dropped)", s.rx_untagged);
    ROW("rx VLAN 0 (dropped)", s.rx_priority);
    ROW("rx other VLAN (dropped)", s.rx_other_vlan);
    ROW("rx bad", s.rx_bad);
    ROW("rx ring full", s.rx_ring_full);
    ROW("rx without a session", s.rx_no_session);
    ROW("tx frames", s.tx_frames);
    ROW("tx bytes", s.tx_bytes);
    ROW("tx bad length", s.tx_bad_len);
    ROW("tx tagged (refused)", s.tx_bad_tag);
    ROW("tx bad flags", s.tx_bad_flags);
    ROW("tx done", s.tx_done);
    ROW("ring errors", s.ring_errors);
    ROW("link changes", s.link_changes);
    ROW("sessions", s.sessions);
    static const char *const chip[] = { "chip tx ok", "chip rx ok", "chip tx errors",
                                        "chip rx errors", "chip rx missed" };
    const uint64_t v[] = { s.chip_tx_ok, s.chip_rx_ok, s.chip_tx_err, s.chip_rx_err,
                           s.chip_rx_missed };
    for (unsigned k = 0; k < 5; k++)
        if (s.chip_counted & (1u << k))
            ROW(chip[k], v[k]);
    if (!s.tx_wait_n && !s.tx_stalls && !s.tx_kicks)
        return;   /* a driver that doesn't time its transmit ring (the e1000e) */
    ROW("tx timed", s.tx_wait_n);
    ROW("tx wait min (us)", s.tx_wait_min_ns / 1000);
    ROW("tx wait avg (us)", s.tx_wait_avg_ns / 1000);
    ROW("tx wait max (us)", s.tx_wait_max_ns / 1000);
    ROW("tx stalls", s.tx_stalls);
    ROW("tx doorbells again", s.tx_kicks);
}

SH_CMD(net)
{
    bool stats = argc == 2 && !strcmp(argv[1], "stats");
    if (argc > 2 || (argc == 2 && !stats)) {
        sh_tty("usage: net [stats]\n");
        return 2;
    }
    handle_t net = net_svc();
    struct net_info i;
    struct net_counters c;
    status_t st = net ? net_info(net, &i) : ERR_NOT_FOUND;
    if (st == OK)
        st = net_get_counters(net, &c);
    if (st != OK) {
        sh_say("net: netstack doesn't answer (%s)\n", status_str(st));
        return 1;
    }
    show_iface(&i);
    if (!stats) {
        show_brief(&c);
        return 0;
    }
    show_stats(&c);
    show_chip(net);
    return 0;
}
