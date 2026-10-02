/* netstack's control channel (abi/idl/netctl.idl): set and clear the
 * interface's address and the DNS servers, and say what netstack knows.
 * Each request is checked before anything changes (fail closed: an
 * address that can't be a host's is refused, not "fixed"), answered at
 * once (nothing here waits), and a change is logged once, as a state
 * change (never per request that changes nothing). */
#include <idl/netctl.h>
#include "ctl.h"

static uint32_t dns[2];   /* the DNS servers set, 0: none */

bool ctl_unicast(uint32_t a)
{
    uint32_t top = a >> 24;
    return top != 0 && top != 127 && top < 224;
}

bool ctl_ipv4_valid(const struct stack_ipv4 *ip)
{
    uint32_t m = ip->mask, host = ~m;
    if ((host & (host + 1)) != 0 || m < 0x80000000u || m > 0xfffffffcu)
        return false;   /* not 1 to 30 leading ones */
    uint32_t a = ip->address, g = ip->gateway;
    if (!ctl_unicast(a) || (a & host) == 0 || (a & host) == host)
        return false;
    if (g == 0)
        return true;
    return ctl_unicast(g) && g != a && (g & m) == (a & m) && (g & host) != 0 &&
           (g & host) != host;
}

const char *ctl_fmt_ip(char *buf, uint32_t a)
{
    snprintf(buf, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

static unsigned prefix_len(uint32_t mask)
{
    return mask ? 32u - (unsigned)__builtin_ctz(mask) : 0;
}

static status_t op_set_ipv4(void *ctx, uint32_t address, uint32_t mask, uint32_t gateway)
{
    (void)ctx;
    struct stack_ipv4 ip = { .address = address, .mask = mask, .gateway = gateway };
    if (!ctl_ipv4_valid(&ip))
        return ERR_INVALID_ARGS;
    struct stack_state s;
    stack_get(&s);
    if (s.ip.address == address && s.ip.mask == mask && s.ip.gateway == gateway)
        return OK;
    stack_set_ipv4(&ip);
    char a[16], g[16];
    nstack_log("address %s/%u, gateway %s", ctl_fmt_ip(a, address), prefix_len(mask),
               gateway ? ctl_fmt_ip(g, gateway) : "none");
    return OK;
}

static status_t op_set_dns(void *ctx, uint32_t first, uint32_t second)
{
    (void)ctx;
    if ((first && !ctl_unicast(first)) || (second && !ctl_unicast(second)))
        return ERR_INVALID_ARGS;
    if (dns[0] == first && dns[1] == second)
        return OK;
    dns[0] = first;
    dns[1] = second;
    char a[16], b[16];
    nstack_log("DNS servers %s, %s", first ? ctl_fmt_ip(a, first) : "none",
               second ? ctl_fmt_ip(b, second) : "none");
    return OK;
}

static status_t op_clear(void *ctx)
{
    (void)ctx;
    struct stack_state s;
    stack_get(&s);
    bool had = s.ip.address || dns[0] || dns[1];
    stack_clear();
    dns[0] = dns[1] = 0;
    if (had)
        nstack_log("address and DNS servers cleared");
    return OK;
}

static status_t op_info(void *ctx, uint32_t *out_address, uint32_t *out_mask,
                        uint32_t *out_gateway, uint32_t *out_dns1, uint32_t *out_dns2,
                        uint8_t out_mac[6], uint8_t *out_device, uint8_t *out_link)
{
    (void)ctx;
    struct stack_state s;
    stack_get(&s);
    *out_address = s.ip.address;
    *out_mask = s.ip.mask;
    *out_gateway = s.ip.gateway;
    *out_dns1 = dns[0];
    *out_dns2 = dns[1];
    memcpy(out_mac, s.mac, STACK_MAC_LEN);
    *out_device = s.device;
    *out_link = s.link;
    return OK;
}

static status_t op_stats(void *ctx, uint64_t *out_rx_frames, uint64_t *out_rx_refused,
                         uint64_t *out_tx_frames, uint64_t *out_tx_dropped,
                         uint64_t *out_echo_replies, uint64_t *out_icmp_errors,
                         uint64_t *out_icmp_limited, uint32_t *out_link_dropped,
                         uint32_t *out_arp_dropped, uint32_t *out_ip_dropped,
                         uint32_t *out_icmp_dropped, uint32_t *out_udp_dropped,
                         uint32_t *out_bad_checksums, uint32_t *out_rx_buffers_used,
                         uint32_t *out_heap_used)
{
    (void)ctx;
    struct stack_counts c;
    stack_get_counts(&c);
    *out_rx_frames = c.rx_frames;
    *out_rx_refused = c.rx_refused;
    *out_tx_frames = c.tx_frames;
    *out_tx_dropped = c.tx_dropped;
    *out_echo_replies = c.echo_replies;
    *out_icmp_errors = c.icmp_errors;
    *out_icmp_limited = c.icmp_limited;
    *out_link_dropped = c.link_dropped;
    *out_arp_dropped = c.arp_dropped;
    *out_ip_dropped = c.ip_dropped;
    *out_icmp_dropped = c.icmp_dropped;
    *out_udp_dropped = c.udp_dropped;
    *out_bad_checksums = c.bad_checksums;
    *out_rx_buffers_used = c.rx_buffers_used;
    *out_heap_used = c.heap_used;
    return OK;
}

static const struct netctl_ops ops = {
    .set_ipv4 = op_set_ipv4,
    .set_dns = op_set_dns,
    .clear = op_clear,
    .info = op_info,
    .stats = op_stats,
};

status_t ctl_serve(handle_t ch)
{
    for (unsigned i = 0; i < CTL_BUDGET; i++) {
        status_t st = netctl_serve_one(ch, &ops, NULL);
        if (st != OK)
            return st;
    }
    return OK;
}
