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
 *             listen`; and /svc/net-low's (SR_USER + 4), whose
 *             openers may also listen on ports below 1024, which the shell
 *             gives only to a program whose list says `svc net listen low`
 *             (bin/serve; never one on /data). And /svc/net-sys's
 *             (SR_USER + 3): the same protocol for the network's own
 *             services (dns, netlog, sntp, and
 *             bin/update through the shell), whose openers may use a
 *             reserve of netstack's openers, sockets and ring bytes that
 *             programs can't take (the fair shares, <net.h>); no program
 *             from /data may ask for it. netstack ends when devmgr does
 *             (its device channels close) and is started again with the
 *             new devmgr's.
 *   dhcp      bin/dhcp, once netstack runs: a duplicate of netctl's client
 *             end (SR_USER + 0) and nothing else. Only without a static
 *             address: it waits for /data's settings (DHCP_DATA_WAIT at
 *             most, for a machine without /data), and isn't started (or
 *             is stopped, if /data came after it) when they have one.
 *   dns       bin/dns, once netstack runs: the server end of /svc/dns's
 *             shared channel (SR_USER + 0, abi/idl/dns.idl), made once
 *             and kept as /svc/net's is, the same for /svc/dns-sys's
 *             (SR_USER + 1: the network's own services, whose openers may
 *             use a reserve of the resolver's that programs can't take),
 *             and /svc/net-sys in its namespace (shell.c's grants). Both
 *             client ends are published here (a channel per opener); only
 *             sntp is granted /svc/dns-sys, and no program from /data may
 *             ask for it. It ends when netstack does, and is started
 *             again once netstack runs.
 *
 *   netlog    bin/netlog, once /data is mounted (its settings say whether):
 *             when `net.host` names the Mac and `netlog` isn't `off`. It
 *             gets the Mac's address and the boot id as arguments, the
 *             root with RIGHT_ROOT_KLOG only (a kernel log reader), a
 *             namespace with /svc/net-sys only (shell.c's grants) and, on the
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
 *             power) and a namespace with /svc/net-sys and /svc/dns-sys (shell.c's
 *             grants). It waits for an address itself. A restart gets the
 *             settings as they are then.
 *
 * The address: `net.address` in /data/etc/settings (<ipv4.h>
 * ipv4_config_parse: "10.2.21.50/24 10.2.21.1 10.2.21.1": address/prefix,
 * gateway, DNS servers) is given to netstack whenever it starts (again)
 * and whenever /data comes. Without it the DHCP client gets one. init's
 * loop never waits for netstack or the DHCP client: set_ipv4 and then
 * set_dns are sent without waiting, and each answer is taken from the
 * loop's port (KEY_NETCTL, net_netctl_event); an answer that doesn't come
 * within CALL_WAIT is said once (net_due), and the next start of netstack
 * or the next /data sends the address again. A DHCP client that runs when
 * a static address appears is killed, and the address goes once its end
 * has come to the loop (net_ended): the client shares netctl's client end,
 * whose answers only one reader may take. */
#include <idl/netctl.h>
#include <ipv4.h>
#include <settings.h>
#include "init.h"

#define CALL_WAIT      NS_PER_S          /* for netstack's answer to a netctl call */
#define DHCP_DATA_WAIT (10 * NS_PER_S)   /* the DHCP client waits this long for /data */

static handle_t ctl_srv, ctl_cli;   /* netctl's two ends, made once (0: none, or given up) */
static handle_t net_srv, net_cli;   /* /svc/net's two ends, the same */
static handle_t crashlog;           /* SR_CRASHLOG read-only, for netlog (0: no panic before) */
static uint64_t boot_id;            /* netlog's boot id, fixed at its first start */
static bool     boot_id_known;
static handle_t dns_srv, dns_cli;   /* /svc/dns's two ends, the same */
static handle_t dsys_srv, dsys_cli; /* /svc/dns-sys's two ends, the same */
static handle_t listen_srv, listen_cli;   /* /svc/net-listen's two ends, the same */
static handle_t low_srv, low_cli;         /* /svc/net-low's two ends, the same */
static handle_t sys_srv, sys_cli;         /* /svc/net-sys's two ends, the same */
static handle_t loop_port;                /* init's loop's port: netctl's answers */

/* The static address on its way to netstack (above, "The address"). */
static struct {
    bool               want;        /* to be sent: net_settings found one */
    struct ipv4_config c;           /* it */
    char               text[SETTINGS_VALUE_MAX];   /* as the settings say it */
    uint32_t           last_txid;   /* idl_txid_next's counter */
    uint32_t           txid;        /* the call whose answer is awaited (0: none) */
    bool               dns;         /* that call is set_dns (else set_ipv4) */
    uint64_t           deadline;    /* its answer by then (uptime ns) */
} addr;

void net_init(handle_t port)
{
    loop_port = port;
    handle_t log = startup_handle(SR_CRASHLOG);
    if (!log || jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &crashlog) != OK)
        crashlog = HANDLE_INVALID;   /* none: netlog sends only this boot's log */
    if (jam_channel_create(&ctl_cli, &ctl_srv) != OK)
        ctl_cli = ctl_srv = HANDLE_INVALID;
    if (jam_channel_create(&net_cli, &net_srv) != OK)
        net_cli = net_srv = HANDLE_INVALID;
    if (jam_channel_create(&listen_cli, &listen_srv) != OK)
        listen_cli = listen_srv = HANDLE_INVALID;   /* no program may listen */
    if (jam_channel_create(&low_cli, &low_srv) != OK)
        low_cli = low_srv = HANDLE_INVALID;   /* no program may listen below 1024 */
    if (jam_channel_create(&sys_cli, &sys_srv) != OK)
        sys_cli = sys_srv = HANDLE_INVALID;   /* the services share /svc/net with programs */
    handle_t d;
    if (jam_channel_create(&dns_cli, &dns_srv) != OK)
        dns_cli = dns_srv = HANDLE_INVALID;
    else if (jam_handle_duplicate(dns_cli, RIGHT_SAME, &d) != OK ||
             ns_svc_set(SVC_DNS, d, true) != OK)   /* a channel per opener */
        printf("init: /svc/dns isn't published: no names for programs\n");
    if (jam_channel_create(&dsys_cli, &dsys_srv) != OK)
        dsys_cli = dsys_srv = HANDLE_INVALID;   /* sntp shares the programs' openers */
    else if (jam_handle_duplicate(dsys_cli, RIGHT_SAME, &d) != OK ||
             ns_svc_set(SVC_DNS_SYS, d, true) != OK)
        printf("init: /svc/dns-sys isn't published: no resolver reserve for sntp\n");
}

handle_t net_svc_channel(void)
{
    return net_cli;
}

handle_t net_listen_channel(void)
{
    return listen_cli;
}

handle_t net_listen_low_channel(void)
{
    return low_cli;
}

handle_t net_sys_channel(void)
{
    return sys_cli;
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
    struct spawn_handle x[5 + INIT_MAX_CLAIMED] = { { SR_USER + 0, HANDLE_INVALID } };
    if (jam_handle_duplicate(ctl_srv, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    unsigned n = 1;
    x[n] = (struct spawn_handle){ SR_USER + 1, HANDLE_INVALID };
    if (net_srv && jam_handle_duplicate(net_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it netstack serves no program: said in its log */
    x[n] = (struct spawn_handle){ SR_USER + 2, HANDLE_INVALID };
    if (listen_srv && jam_handle_duplicate(listen_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it no program may listen */
    x[n] = (struct spawn_handle){ SR_USER + 3, HANDLE_INVALID };
    if (sys_srv && jam_handle_duplicate(sys_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it the services have no reserve: said in netstack's log */
    x[n] = (struct spawn_handle){ SR_USER + 4, HANDLE_INVALID };
    if (low_srv && jam_handle_duplicate(low_srv, RIGHT_SAME, &x[n].h) == OK)
        n++;   /* without it no program may listen below 1024 */
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
    jam_job_kill(s->job);   /* its end comes to the loop: then the address goes (net_ended) */
    printf("init: net.address is set: the DHCP client is stopped, the static address wins\n");
}

/* No answer is awaited any more. */
static void addr_idle(void)
{
    if (addr.txid)
        (void)jam_port_unbind(loop_port, ctl_cli, KEY_NETCTL);   /* fired already: nothing to do */
    addr.txid = 0;
}

/* Watch netctl's client end for the awaited answer: one ONCE binding (an
 * older one replaced), which fires at once if the answer is there. */
static status_t watch(void)
{
    (void)jam_port_unbind(loop_port, ctl_cli, KEY_NETCTL);   /* none: nothing to do */
    return jam_port_bind(loop_port, ctl_cli, KEY_NETCTL, SIG_READABLE, PORT_BIND_ONCE);
}

static void addr_failed(status_t st)
{
    printf("init: settings: netstack didn't take net.address = %s (%s)\n", addr.text,
           status_str(st));
    addr_idle();
    addr.want = false;   /* sent again by the next start of netstack, or the next /data */
}

/* Send the static address's next call (set_dns once set_ipv4 is taken), if
 * netstack runs and no DHCP client does. */
static void addr_send(bool dns)
{
    if (!addr.want || !ctl_cli || !svcs[NETSTACK].running || svcs[DHCP].running)
        return;
    addr_idle();   /* an older call's answer, if it still comes, is nobody's */
    uint32_t txid = idl_txid_next(&addr.last_txid);
    const struct ipv4_config *c = &addr.c;
    status_t st = dns ? netctl_set_dns_send(ctl_cli, txid, c->dns[0], c->dns[1])
                      : netctl_set_ipv4_send(ctl_cli, txid, c->address, c->mask, c->gateway);
    if (st == OK)
        st = watch();
    if (st != OK) {
        addr_failed(st);
        return;
    }
    addr.txid = txid;
    addr.dns = dns;
    addr.deadline = now() + CALL_WAIT;
}

void net_settings(void)
{
    if (!static_address(addr.text, &addr.c))
        return;
    no_dhcp();
    addr.want = true;
    addr_send(false);
}

/* One answer off netctl's client end: the awaited call's, or nobody's. */
static void addr_answer(const void *rep, struct idl_msg *m)
{
    if (!addr.txid || m->txid != addr.txid) {
        idl_msg_drop(m);   /* an older call's, or the DHCP client's from before it ended */
        return;
    }
    bool dns = addr.dns;
    status_t st = dns ? netctl_set_dns_result(rep, m) : netctl_set_ipv4_result(rep, m);
    addr.txid = 0;   /* its binding fired: none left */
    if (st != OK) {
        addr_failed(st);
    } else if (!dns) {
        addr_send(true);
    } else {
        addr.want = false;   /* both taken */
    }
}

void net_netctl_event(void)
{
    for (unsigned k = 0; k < 8 && addr.txid; k++) {   /* a few stale answers at most */
        _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ctl_cli, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st != OK) {
            idl_msg_drop(&m);
            if (st == ERR_PEER_CLOSED)
                addr_failed(st);
            continue;
        }
        addr_answer(rep, &m);
    }
    if (addr.txid && watch() != OK)
        addr_failed(ERR_NO_RESOURCES);   /* nothing would see the answer */
}

uint64_t net_due(uint64_t t)
{
    if (!addr.txid)
        return DEADLINE_NEVER;
    if (t < addr.deadline)
        return addr.deadline;
    addr_failed(ERR_TIMED_OUT);
    return DEADLINE_NEVER;
}

void net_ended(unsigned i)
{
    if (i == NETSTACK)
        addr_idle();   /* its answers went with it: its next start sends again */
    if (i == DHCP)
        addr_send(false);   /* a stopped client's end came: the static address may go */
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
    struct spawn_handle x[] = { { SR_USER + 0, HANDLE_INVALID }, { SR_USER + 1, HANDLE_INVALID } };
    if (jam_handle_duplicate(dns_srv, RIGHT_SAME, &x[0].h) != OK)
        return ERR_NO_RESOURCES;
    unsigned n = 1;
    if (dsys_srv && jam_handle_duplicate(dsys_srv, RIGHT_SAME, &x[1].h) == OK)
        n++;   /* without it the resolver has no reserve: said in its log */
    return svc_start1(DNS, x, n);   /* consumes them */
}

void net_service_given_up(unsigned i)
{
    if (i != DNS || !dns_srv)
        return;
    jam_handle_close(dns_srv);   /* resolves waiting for a resolver fail now */
    dns_srv = HANDLE_INVALID;
    (void)ns_svc_remove(SVC_DNS);   /* nobody new gets it (none there: nothing to do) */
    if (dsys_srv)
        jam_handle_close(dsys_srv);
    dsys_srv = HANDLE_INVALID;
    (void)ns_svc_remove(SVC_DNS_SYS);   /* the same */
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
    if (low_srv)
        jam_handle_close(low_srv);
    if (sys_srv)
        jam_handle_close(sys_srv);
    ctl_srv = net_srv = listen_srv = low_srv = sys_srv = HANDLE_INVALID;
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
