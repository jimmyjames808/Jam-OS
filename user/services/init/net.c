/* init's shell mode: the network services (docs/M9-PLAN.md): netstack,
 * the DHCP client and the resolver.
 *
 *   netstack  bin/netstack, once devmgr runs: the server end of its
 *             control channel (SR_USER + 0, abi/idl/netctl.idl) and a
 *             duplicate of each network card's device channel
 *             (SR_DEVMGR_DEVICE; services.c claims them before it
 *             publishes /svc/devmgr, so netstack is the only program that
 *             reaches the cards' drivers; none without a card: the link
 *             stays down). init makes the control channel once and keeps
 *             both ends, as for the mixer, so a restarted netstack serves
 *             the same channel; it is never published: init uses its
 *             client end for the static address, and the DHCP client
 *             gets a duplicate. Also the server end of
 *             /svc/net's shared channel (SR_USER + 1, abi/idl/net.idl),
 *             made once and kept the same way; services.c publishes its
 *             client end (a channel per opener). netstack ends when devmgr
 *             does (its device channels close) and is started again with
 *             the new devmgr's.
 *   dhcp      bin/dhcp, once netstack runs: a duplicate of netctl's client
 *             end (SR_USER + 0) and nothing else. Only without a static
 *             address: it waits for /data's settings (DHCP_DATA_WAIT at
 *             most, for a machine without /data), and isn't started (or
 *             is stopped, if /data came after it) when they have one.
 *   dns       bin/dns, once netstack runs: the server end of /svc/dns's
 *             shared channel (SR_USER + 0, abi/idl/dns.idl), made once
 *             and kept as /svc/net's is, and /svc/net in its namespace
 *             (shell.c's grants). Its client end is published here as
 *             /svc/dns (a channel per opener). It ends when netstack
 *             does, and is started again once netstack runs.
 *
 * The address: `net.address` in /data/etc/settings (<ipv4.h>
 * ipv4_config_parse: "10.2.21.50/24 10.2.21.1 10.2.21.1": address/prefix,
 * gateway, DNS servers) is given to netstack whenever it starts (again)
 * and whenever /data comes, with a short deadline: init's loop never waits
 * long for a service. Without it the DHCP client gets one. */
#include <idl/netctl.h>
#include <ipv4.h>
#include <settings.h>
#include "init.h"

#define CALL_WAIT      NS_PER_S
#define DHCP_DATA_WAIT (10 * NS_PER_S)   /* the DHCP client waits this long for /data */
#define DHCP_STOP_WAIT NS_PER_S          /* for a stopped DHCP client to be gone */

static handle_t ctl_srv, ctl_cli;   /* netctl's two ends, made once (0: none, or given up) */
static handle_t net_srv, net_cli;   /* /svc/net's two ends, the same */
static handle_t dns_srv, dns_cli;   /* /svc/dns's two ends, the same */

void net_init(void)
{
    if (jam_channel_create(&ctl_cli, &ctl_srv) != OK)
        ctl_cli = ctl_srv = HANDLE_INVALID;
    if (jam_channel_create(&net_cli, &net_srv) != OK)
        net_cli = net_srv = HANDLE_INVALID;
    handle_t d;
    if (jam_channel_create(&dns_cli, &dns_srv) != OK)
        dns_cli = dns_srv = HANDLE_INVALID;
    else if (jam_handle_duplicate(dns_cli, RIGHT_SAME, &d) != OK ||
             ns_svc_set(SVC_DNS, d, true) != OK)   /* a channel per opener */
        printf("init: /svc/dns isn't published: no names for programs\n");
}

handle_t net_svc_channel(void)
{
    return net_cli;
}

status_t net_start(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (!ctl_srv || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[NETSTACK].path, &data, &size) != OK) {
        printf("init: no %s (or no channel for it): no network\n", svcs[NETSTACK].path);
        svcs[NETSTACK].given_up = true;
        return OK;
    }
    struct spawn_handle x[2 + INIT_MAX_CLAIMED] = { { SR_USER + 0, HANDLE_INVALID },
                                                    { SR_USER + 1, HANDLE_INVALID } };
    if (jam_handle_duplicate(ctl_srv, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    unsigned n = 1;
    if (net_srv && jam_handle_duplicate(net_srv, RIGHT_SAME, &x[1].h) == OK)
        n++;   /* without it netstack serves no program: said in its log */
    handle_t cards[INIT_MAX_CLAIMED];
    unsigned nc = services_net_devices(cards, INIT_MAX_CLAIMED);
    for (unsigned k = 0; k < nc; k++)
        x[n + k] = (struct spawn_handle){ SR_DEVMGR_DEVICE, cards[k] };
    return svc_start1(NETSTACK, x, n + nc);   /* consumes them */
}

/* /data's settings have a static address: *c (v: its text). */
static bool static_address(char v[SETTINGS_VALUE_MAX], struct ipv4_config *c)
{
    if (settings_get(SETTINGS_FILE, "net.address", v, SETTINGS_VALUE_MAX) != OK)
        return false;
    if (ipv4_config_parse(v, c))
        return true;
    printf("init: settings: net.address = %s is not <address>/<prefix> [<gateway> [<dns> "
           "[<dns>]]]\n", v);
    return false;
}

/* The static address wins: the DHCP client never starts, and one already
 * running (/data came after it started) is stopped first, so it can't set
 * an address after ours. */
static void no_dhcp(void)
{
    struct svc *s = &svcs[DHCP];
    if (s->given_up)
        return;
    s->given_up = true;   /* its end still comes to the loop: no restart */
    if (!s->running) {
        printf("init: net.address is set: the static address, no DHCP client\n");
        return;
    }
    signals_t seen;
    jam_job_kill(s->job);
    (void)jam_object_wait_one(s->proc, SIG_TERMINATED, now() + DHCP_STOP_WAIT, &seen);
    printf("init: net.address is set: the DHCP client is stopped, the static address wins\n");
}

void net_settings(void)
{
    char v[SETTINGS_VALUE_MAX];
    struct ipv4_config c;
    if (!static_address(v, &c))
        return;
    no_dhcp();
    if (!ctl_cli || !svcs[NETSTACK].running)
        return;
    status_t st = netctl_set_ipv4_until(ctl_cli, now() + CALL_WAIT, c.address, c.mask,
                                        c.gateway);
    if (st == OK)
        st = netctl_set_dns_until(ctl_cli, now() + CALL_WAIT, c.dns[0], c.dns[1]);
    if (st != OK)
        printf("init: settings: netstack didn't take net.address = %s (%s)\n", v,
               status_str(st));
}

uint64_t net_dhcp_wait(uint64_t t, bool data)
{
    static uint64_t since;   /* the first time it could have started */
    if (!since)
        since = t;
    return data || t >= since + DHCP_DATA_WAIT ? 0 : since + DHCP_DATA_WAIT;
}

/* svc i's program is in bootfs; else it is given up on, said once. */
static bool in_bootfs(unsigned i, const char *without)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) == OK && bootfs_lookup(fs, svcs[i].path, &data, &size) == OK)
        return true;
    printf("init: no %s in bootfs: %s\n", svcs[i].path, without);
    svcs[i].given_up = true;
    return false;
}

status_t net_dhcp_start(void)
{
    char v[SETTINGS_VALUE_MAX];
    struct ipv4_config c;
    if (static_address(v, &c)) {
        no_dhcp();
        return OK;
    }
    if (!ctl_cli || !in_bootfs(DHCP, "no DHCP client")) {
        svcs[DHCP].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = { { SR_USER + 0, HANDLE_INVALID } };
    if (jam_handle_duplicate(ctl_cli, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    return svc_start1(DHCP, x, 1);   /* consumes it */
}

status_t net_dns_start(void)
{
    if (!dns_srv || !in_bootfs(DNS, "no names for programs")) {
        svcs[DNS].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = { { SR_USER + 0, HANDLE_INVALID } };
    if (jam_handle_duplicate(dns_srv, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    return svc_start1(DNS, x, 1);   /* consumes it */
}

void net_service_given_up(unsigned i)
{
    if (i != DNS || !dns_srv)
        return;
    jam_handle_close(dns_srv);   /* resolves waiting for a resolver fail now */
    dns_srv = HANDLE_INVALID;
    (void)ns_svc_remove(SVC_DNS);   /* nobody new gets it (none there: nothing to do) */
    tell_mounts();
}

void net_devmgr_gone(void)
{
    /* Its device channels were the dead devmgr's: it starts again with the
     * new one's (it ends by itself when it had a card; this covers none). */
    if (svcs[NETSTACK].running)
        (void)jam_process_kill(svcs[NETSTACK].proc);   /* its end comes to the loop */
}

void net_given_up(void)
{
    if (ctl_srv)
        jam_handle_close(ctl_srv);   /* calls waiting for a netstack fail now */
    if (net_srv)
        jam_handle_close(net_srv);
    ctl_srv = net_srv = HANDLE_INVALID;
}
