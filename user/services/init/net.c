/* init's shell mode: the network services (docs/M9-PLAN.md): netstack,
 * the DHCP client, the resolver, netlog and the clock from the network.
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
 *             client end (a channel per opener). The same for
 *             /svc/net-listen's (SR_USER + 2): the same protocol, but its
 *             openers may also take the ports other programs can't (the
 *             listen permission, docs/M9.5-PLAN.md); init publishes it, and
 *             the shell gives it only to a program whose list says `svc net
 *             listen`. netstack ends when devmgr does (its device channels
 *             close) and is started again with the new devmgr's.
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
 *   netlog    bin/netlog, once /data is mounted (its settings say whether):
 *             when `net.host` names the Mac and `netlog` isn't `off`. It
 *             gets the Mac's address and the boot id as arguments, the
 *             root with RIGHT_ROOT_KLOG only (a kernel log reader), a
 *             namespace with /svc/net only (shell.c's grants) and, on the
 *             boot after a panic, a read-only duplicate of SR_CRASHLOG,
 *             taken in net_init, before lastboot.c lets the log go. The
 *             boot id (the kernel's start in UTC ns, from the wall clock
 *             at netlog's first start) is fixed once a boot: a restarted
 *             netlog sends into the same file on the Mac. Without
 *             `net.host`, or with `netlog = off`, it is not started this
 *             boot (said once).
 *   sntp      bin/sntp, once /data is mounted (its settings say whether,
 *             and which server) and netstack runs: unless `ntp = off`,
 *             with `ntp.server` as its argument (none: it asks the
 *             gateway, then pool.ntp.org), the root with RIGHT_ROOT_CLOCK
 *             only (it sets the kernel's clock: no other service has that
 *             power) and a namespace with /svc/net and /svc/dns (shell.c's
 *             grants). It waits for an address itself. A restart gets the
 *             settings as they are then.
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
static handle_t crashlog;           /* SR_CRASHLOG read-only, for netlog (0: no panic before) */
static uint64_t boot_id;            /* netlog's boot id, fixed at its first start */
static bool     boot_id_known;
static handle_t dns_srv, dns_cli;   /* /svc/dns's two ends, the same */
static handle_t listen_srv, listen_cli;   /* /svc/net-listen's two ends, the same */

void net_init(void)
{
    handle_t log = startup_handle(SR_CRASHLOG);
    if (!log || jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &crashlog) != OK)
        crashlog = HANDLE_INVALID;   /* none: netlog sends only this boot's log */
    if (jam_channel_create(&ctl_cli, &ctl_srv) != OK)
        ctl_cli = ctl_srv = HANDLE_INVALID;
    if (jam_channel_create(&net_cli, &net_srv) != OK)
        net_cli = net_srv = HANDLE_INVALID;
    if (jam_channel_create(&listen_cli, &listen_srv) != OK)
        listen_cli = listen_srv = HANDLE_INVALID;   /* no program may listen */
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

handle_t net_listen_channel(void)
{
    return listen_cli;
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
    struct spawn_handle x[3 + INIT_MAX_CLAIMED] = { { SR_USER + 0, HANDLE_INVALID } };
    if (jam_handle_duplicate(ctl_srv, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    unsigned n = 1;
    x[n] = (struct spawn_handle){ SR_USER + 1, HANDLE_INVALID };
    if (net_srv && jam_handle_duplicate(net_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it netstack serves no program: said in its log */
    x[n] = (struct spawn_handle){ SR_USER + 2, HANDLE_INVALID };
    if (listen_srv && jam_handle_duplicate(listen_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it no program may listen */
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
    if (listen_srv)
        jam_handle_close(listen_srv);
    ctl_srv = net_srv = listen_srv = HANDLE_INVALID;
}

/* The kernel's start in UTC ns: the wall clock now less the uptime; 0 if
 * the clock isn't known. Worked out once a boot. */
static uint64_t netlog_boot_id(void)
{
    struct wall_clock w;
    if (!boot_id_known && jam_wallclock_get(&w) == OK && w.utc_ns > 0 &&
        (uint64_t)w.utc_ns > w.uptime_ns)
        boot_id = (uint64_t)w.utc_ns - w.uptime_ns;
    boot_id_known = true;
    return boot_id;
}

/* netlog runs this boot: `net.host` is an address and `netlog` isn't
 * off. *host gets the address as text. Says why not, once. */
static bool netlog_wanted(char host[IPV4_TEXT_MAX])
{
    char v[SETTINGS_VALUE_MAX], off[8];
    uint32_t a;
    const char *end = NULL;
    if (settings_get(SETTINGS_FILE, "netlog", off, sizeof(off)) == OK && !strcmp(off, "off")) {
        printf("init: netlog = off in the settings: the log is not sent over the network\n");
        return false;
    }
    if (settings_get(SETTINGS_FILE, "net.host", v, sizeof(v)) != OK) {
        printf("init: no net.host in the settings: the log is not sent over the network\n");
        return false;
    }
    if (!ipv4_parse(v, &a, &end) || *end) {
        printf("init: settings: net.host = %s is not an IPv4 address: no netlog\n", v);
        return false;
    }
    ipv4_format(a, host);
    return true;
}

status_t net_netlog_start(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    char host[IPV4_TEXT_MAX], boot[17];
    if (!netlog_wanted(host) || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[NETLOG].path, &data, &size) != OK) {
        svcs[NETLOG].given_up = true;   /* not this boot (bootfs without it: a test image) */
        return OK;
    }
    snprintf(boot, sizeof(boot), "%016lx", (unsigned long)netlog_boot_id());
    struct spawn_handle x[2] = { { SR_RESOURCE, HANDLE_INVALID }, { SR_CRASHLOG, HANDLE_INVALID } };
    if (jam_handle_duplicate(shell_root(), RIGHTS_BASIC | RIGHT_ROOT_KLOG, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    unsigned n = 1;
    if (crashlog && jam_handle_duplicate(crashlog, RIGHT_SAME, &x[1].h) == OK)
        n++;   /* without it only this boot's log goes */
    const char *argv[] = { svcs[NETLOG].path, host, boot };
    return svc_start(NETLOG, 3, argv, x, n);   /* consumes them */
}

status_t net_sntp_start(void)
{
    char server[SETTINGS_VALUE_MAX] = "", off[8];
    if (settings_get(SETTINGS_FILE, "ntp", off, sizeof(off)) == OK && !strcmp(off, "off")) {
        printf("init: ntp = off in the settings: the clock is not set from the network\n");
        svcs[SNTP].given_up = true;   /* not this boot */
        return OK;
    }
    if (!in_bootfs(SNTP, "the clock is not set from the network"))
        return OK;
    (void)settings_get(SETTINGS_FILE, "ntp.server", server, sizeof(server));   /* none: "" */
    struct spawn_handle x[] = { { SR_RESOURCE, HANDLE_INVALID } };
    if (jam_handle_duplicate(shell_root(), RIGHTS_BASIC | RIGHT_ROOT_CLOCK, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    const char *argv[] = { svcs[SNTP].path, server };
    return svc_start(SNTP, server[0] ? 2 : 1, argv, x, 1);   /* consumes it */
}
