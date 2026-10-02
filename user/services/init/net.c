/* init's shell mode: the network services (docs/M9-PLAN.md). Today
 * netstack and netlog; the DHCP client and DNS are planned here too.
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
 *             (planned) gets a duplicate. Also the server end of
 *             /svc/net's shared channel (SR_USER + 1, abi/idl/net.idl),
 *             made once and kept the same way; services.c publishes its
 *             client end (a channel per opener). netstack ends when devmgr
 *             does (its device channels close) and is started again with
 *             the new devmgr's.
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
 *
 * The address: `net.address` in /data/etc/settings (<ipv4.h>
 * ipv4_config_parse: "10.2.21.50/24 10.2.21.1 10.2.21.1": address/prefix,
 * gateway, DNS servers) is given to netstack whenever it starts (again)
 * and whenever /data comes, with a short deadline: init's loop never waits
 * long for a service. Without it netstack has no address until the DHCP
 * client (planned) gives it one. */
#include <idl/netctl.h>
#include <ipv4.h>
#include <settings.h>
#include "init.h"

#define CALL_WAIT NS_PER_S

static handle_t ctl_srv, ctl_cli;   /* netctl's two ends, made once (0: none, or given up) */
static handle_t net_srv, net_cli;   /* /svc/net's two ends, the same */
static handle_t crashlog;           /* SR_CRASHLOG read-only, for netlog (0: no panic before) */
static uint64_t boot_id;            /* netlog's boot id, fixed at its first start */
static bool     boot_id_known;

void net_init(void)
{
    handle_t log = startup_handle(SR_CRASHLOG);
    if (!log || jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &crashlog) != OK)
        crashlog = HANDLE_INVALID;   /* none: netlog sends only this boot's log */
    if (jam_channel_create(&ctl_cli, &ctl_srv) != OK)
        ctl_cli = ctl_srv = HANDLE_INVALID;
    if (jam_channel_create(&net_cli, &net_srv) != OK)
        net_cli = net_srv = HANDLE_INVALID;
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

void net_settings(void)
{
    char v[SETTINGS_VALUE_MAX];
    struct ipv4_config c;
    if (!ctl_cli || !svcs[NETSTACK].running ||
        settings_get(SETTINGS_FILE, "net.address", v, sizeof(v)) != OK)
        return;
    if (!ipv4_config_parse(v, &c)) {
        printf("init: settings: net.address = %s is not <address>/<prefix> [<gateway> [<dns> "
               "[<dns>]]]\n", v);
        return;
    }
    status_t st = netctl_set_ipv4_until(ctl_cli, now() + CALL_WAIT, c.address, c.mask,
                                        c.gateway);
    if (st == OK)
        st = netctl_set_dns_until(ctl_cli, now() + CALL_WAIT, c.dns[0], c.dns[1]);
    if (st != OK)
        printf("init: settings: netstack didn't take net.address = %s (%s)\n", v,
               status_str(st));
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
