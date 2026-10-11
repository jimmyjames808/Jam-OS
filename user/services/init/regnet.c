/* init's regression run (the boot word `init`, main.c): the network, so the
 * run's log can reach the Mac like a plain boot's.
 *
 * A plain boot starts netstack, the DHCP client, netlog and logd by itself
 * (net.c, services.c); the regression run starts only bootfs, devmgr and
 * volumes and then init.cfg's programs, so on the owner's PC its results
 * were on the screen and in the RESULTS box alone. This file starts the
 * same four when the stick asks for them, in a simple form (no
 * supervision: the run is short, and a service that ends stays ended), and
 * stops them at the end of the run, in the one order that keeps the end of
 * the log:
 *
 *   regnet_start   after volumes: only when the boot has a network
 *                  (`vlan=` word) and devmgr has a network card (init holds
 *                  its device channel): so a QEMU run without QEMU_NET, and
 *                  a `vlan=off` boot, change not at all. It waits for /data
 *                  (RUN_DATA_WAIT at most), reads the settings and goes on
 *                  only if `net.host` is an address and `netlog` isn't off.
 *                  Then, in this order:
 *       netstack   bin/netstack: its control channel (NETSTACK_SR_CTL, init
 *                  keeps the client end), /svc/net-sys's shared channel
 *                  (NETSTACK_SR_NET_SYS: the reserve of the network's own
 *                  services) and a duplicate of each card's device channel.
 *                  No /svc/net, net-listen, net-low: no program of the
 *                  run may use the network, and netstack says so once.
 *       address    `net.address`, if the settings have one, sent to
 *                  netstack with the two netctl calls (net.c's rules); else
 *       dhcp       bin/dhcp, with a duplicate of netctl's client end.
 *       logd       bin/logd, when /data is there: the log into
 *                  /data/logs/boot-NNNN.txt as in a plain boot (the root
 *                  with RIGHT_ROOT_KLOG, a namespace of /data:w that
 *                  follows init's: regnet_mounts, when usbtest takes the
 *                  stick from volumes and gives it back, and its control
 *                  channel for the flush).
 *       netlog     bin/netlog <net.host> <boot id>, as net.c starts it: the
 *                  root with RIGHT_ROOT_KLOG, a namespace of /svc/net-sys
 *                  alone (published in init's namespace for the length of
 *                  the spawn only: the programs of the run don't get it),
 *                  the panicked boot's log if there was one, and a stop
 *                  channel (NETLOG_SR_STOP). It reads the kernel log from
 *                  its first byte, so every line of the boot goes, those
 *                  from before it started too.
 *   regnet_stop_logd   before volumes stop: logd flushes and ends.
 *   regnet_stop    after volumes stop, before devmgr: closing the stop
 *                  channel asks netlog to send what the log holds by then,
 *                  wait for the Mac's acks (4 s at most) and exit; then
 *                  the DHCP client and netstack are killed (their jobs),
 *                  so the root job is empty again for the kernel's clean
 *                  check. What comes after (devmgr's and bootfs' stop
 *                  lines) goes to the screen and the RESULTS box only, and
 *                  so do the kernel's own lines once init has exited
 *                  ("init: exited with code", the root job's line, the
 *                  RESULTS box itself): the root job has to be empty
 *                  before the kernel prints them, so no process can be
 *                  left to send them.
 *
 * The network never decides a result: every failure here is a line in the
 * log and nothing else (the run's exit code is not touched). */
#include <idl/logctl.h>
#include <idl/netctl.h>
#include <ipv4.h>
#include <settings.h>
#include "init.h"

#define RUN_DATA_WAIT (15 * NS_PER_S)   /* /data, from regnet_start */
#define CALL_WAIT     (5 * NS_PER_S)    /* a netctl call: netstack has to start first */
#define FLUSH_WAIT    (5 * NS_PER_S)    /* logd's flush */
#define NETLOG_WAIT   (7 * NS_PER_S)    /* netlog's own STOP_WAIT is 4 s; then it is killed */

static const char *const logd_grants[] = { DATA_MOUNT ":w", NULL };
static const char *const netlog_grants[] = { "/svc/" SVC_NET_SYS, NULL };

/* One started program: its process and job (0: none). */
struct prog {
    handle_t proc, job;
};

static struct {
    bool        on;                         /* the network services were started */
    handle_t    ctl_cli;                    /* netstack's control channel, our end */
    struct prog netstack, dhcp, netlog, logd;
    handle_t    netlog_stop;                /* our end of netlog's stop channel */
    handle_t    logd_ctl;                   /* logd's control channel, our end */
    handle_t    logd_ns, logd_back;         /* logd's namespace channel: ns_update's */
} rn;

static bool have_mount(const char *path)
{
    char point[NS_NAME_MAX];
    for (unsigned i = 0; ns_mount_at(i, point); i++)
        if (!strcmp(point, path))
            return true;
    return false;
}

static bool in_bootfs(const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    return bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, &size) == OK;
}

/* Start path in a job of its own with the handles in x (consumed, whatever
 * happens). ns: the grants of a namespace for it (NULL: none); ns_out and
 * back_out: where init's ends of it go, for ns_update (NULL: never
 * updated). */
static status_t start(struct prog *p, const char *path, int argc, const char *const *argv,
                      struct spawn_handle *x, unsigned nx, const char *const *ns,
                      handle_t *ns_out, handle_t *back_out)
{
    handle_t job = HANDLE_INVALID, proc = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        for (unsigned k = 0; k < nx; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return st;
    }
    struct spawn_args a = {
        .path = path, .argc = argc, .argv = argv, .job = job, .extra = x, .nextra = nx,
        .ns = ns, .ns_out = ns_out, .ns_back_out = back_out,
    };
    st = spawn(&a, &proc);
    if (st != OK) {
        jam_handle_close(job);
        return st;
    }
    p->proc = proc;
    p->job = job;
    return OK;
}

/* End p: wait up to wait_ns for it to exit by itself, then kill its job
 * (which returns once all of it is dead). Says how it ended. */
static void end(struct prog *p, const char *name, uint64_t wait_ns)
{
    if (!p->proc)
        return;
    struct process_info info;
    status_t st = wait_ns ? spawn_wait(p->proc, wait_ns, &info) : ERR_TIMED_OUT;
    if (st != OK) {
        (void)jam_job_kill(p->job);
        st = spawn_wait(p->proc, 5 * NS_PER_S, &info);
    }
    if (st == OK && !info.killed && info.exit_code != 0)
        printf("init: %s exited with code %ld\n", name, (long)info.exit_code);
    else if (st != OK)
        printf("init: %s: lost track of it (%s)\n", name, status_str(st));
    jam_handle_close(p->proc);
    jam_handle_close(p->job);
    p->proc = p->job = HANDLE_INVALID;
}

/* The kernel's start in UTC ns: the wall clock now less the uptime; 0 if
 * the clock isn't known (net.c's netlog_boot_id). */
static uint64_t boot_id(void)
{
    struct wall_clock w;
    if (jam_wallclock_get(&w) == OK && w.utc_ns > 0 && (uint64_t)w.utc_ns > w.uptime_ns)
        return (uint64_t)w.utc_ns - w.uptime_ns;
    return 0;
}

/* The root with these rights only (a duplicate), or HANDLE_INVALID. */
static handle_t root_with(rights_t rights)
{
    handle_t h = HANDLE_INVALID;
    if (jam_handle_duplicate(startup_handle(SR_RESOURCE), rights, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

/* /data comes (volumes' mounts arrive on the port under KEY_MOUNTS): true
 * once it is in our namespace, false after RUN_DATA_WAIT. */
static bool wait_data(handle_t port, uint64_t deadline)
{
    (void)mounts_event();
    while (!have_mount(DATA_MOUNT)) {
        struct port_packet pkt;
        if (!port || jam_port_wait(port, deadline, &pkt) != OK)
            return false;
        if (pkt.key == KEY_MOUNTS)
            (void)mounts_event();
    }
    return true;
}

/* netstack with the cards' device channels (duplicates) and the shared
 * channel of /svc/net-sys; *sys gets the client end of that one. */
static status_t start_netstack(const handle_t *cards, unsigned ncards, handle_t *sys)
{
    handle_t ctl_srv = HANDLE_INVALID, sys_srv = HANDLE_INVALID;
    status_t st = jam_channel_create(&rn.ctl_cli, &ctl_srv);
    if (st == OK)
        st = jam_channel_create(sys, &sys_srv);
    if (st != OK) {
        handle_t left[] = { rn.ctl_cli, ctl_srv, *sys, sys_srv };
        for (unsigned k = 0; k < 4; k++)
            if (left[k])
                jam_handle_close(left[k]);
        rn.ctl_cli = *sys = HANDLE_INVALID;
        return st;
    }
    struct spawn_handle x[2 + INIT_MAX_CLAIMED] = {
        { NETSTACK_SR_CTL, ctl_srv }, { NETSTACK_SR_NET_SYS, sys_srv },
    };
    unsigned n = 2;
    for (unsigned k = 0; k < ncards && n < 2 + INIT_MAX_CLAIMED; k++)
        if (cards[k] && jam_handle_duplicate(cards[k], RIGHT_SAME, &x[n].h) == OK)
            x[n++].role = SR_DEVMGR_DEVICE;
    const char *argv[] = { "bin/netstack" };
    return start(&rn.netstack, "bin/netstack", 1, argv, x, n, NULL, NULL, NULL);
}

/* The static address of net.address, if the settings have a good one
 * (net.c's rules: the DHCP client otherwise). */
static bool send_static(const char *text)
{
    char v[SETTINGS_VALUE_MAX];
    struct ipv4_config c;
    snprintf(v, sizeof(v), "%s", text);
    if (!ipv4_config_parse(v, &c)) {
        printf("init: settings: net.address = %s is not <address>/<prefix> [<gateway> [<dns> "
               "[<dns>]]]\n", text);
        return false;
    }
    uint64_t until = now() + CALL_WAIT;
    status_t st = netctl_set_ipv4_until(rn.ctl_cli, until, c.address, c.mask, c.gateway);
    if (st == OK)
        st = netctl_set_dns_until(rn.ctl_cli, until, c.dns[0], c.dns[1]);
    if (st != OK)
        printf("init: settings: netstack didn't take net.address = %s (%s)\n", text,
               status_str(st));
    return true;   /* static either way: no DHCP client to set another address after it */
}

static void start_dhcp(void)
{
    if (!in_bootfs("bin/dhcp")) {
        printf("init: no bin/dhcp in bootfs: no DHCP client\n");
        return;
    }
    struct spawn_handle x[] = { { DHCP_SR_NETCTL, HANDLE_INVALID } };
    if (jam_handle_duplicate(rn.ctl_cli, RIGHT_SAME, &x[0].h) != OK)
        return;
    const char *argv[] = { "bin/dhcp" };
    status_t st = start(&rn.dhcp, "bin/dhcp", 1, argv, x, 1, NULL, NULL, NULL);
    if (st != OK)
        printf("init: can't start the DHCP client (%s)\n", status_str(st));
}

static void start_logd(void)
{
    if (!in_bootfs("bin/logd")) {
        printf("init: no bin/logd in bootfs: the boot log is not saved\n");
        return;
    }
    handle_t theirs = HANDLE_INVALID;
    status_t st = jam_channel_create(&rn.logd_ctl, &theirs);
    if (st != OK)
        return;
    struct spawn_handle x[] = { { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_ROOT_KLOG) },
                                { LOGD_SR_CTL, theirs } };
    const char *argv[] = { "bin/logd" };
    st = start(&rn.logd, "bin/logd", 1, argv, x, 2, logd_grants, &rn.logd_ns, &rn.logd_back);
    if (st != OK) {
        printf("init: can't start logd (%s): the boot log is not saved\n", status_str(st));
        jam_handle_close(rn.logd_ctl);
        rn.logd_ctl = HANDLE_INVALID;
    }
}

static void start_netlog(const char *host, handle_t sys_cli)
{
    handle_t d = HANDLE_INVALID, mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    /* /svc/net-sys in our namespace for the spawn only (its grants name it): the
     * programs of the run are spawned later and don't get it. */
    status_t st = jam_handle_duplicate(sys_cli, RIGHT_SAME, &d);
    if (st == OK)
        st = ns_svc_set(SVC_NET_SYS, d, true);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    if (st != OK) {
        printf("init: can't start netlog (%s)\n", status_str(st));
        if (mine)
            jam_handle_close(mine);
        if (theirs)
            jam_handle_close(theirs);
        (void)ns_svc_remove(SVC_NET_SYS);
        return;
    }
    char id[17];
    snprintf(id, sizeof(id), "%016lx", (unsigned long)boot_id());
    struct spawn_handle x[3] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_ROOT_KLOG) },
        { NETLOG_SR_STOP, theirs },
        { SR_CRASHLOG, HANDLE_INVALID },
    };
    unsigned n = 2;
    handle_t log = startup_handle(SR_CRASHLOG);
    if (log && jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &x[2].h) == OK)
        n++;   /* without it only this boot's log goes */
    const char *argv[] = { "bin/netlog", host, id };
    st = start(&rn.netlog, "bin/netlog", 3, argv, x, n, netlog_grants, NULL, NULL);
    (void)ns_svc_remove(SVC_NET_SYS);
    if (st != OK) {
        printf("init: can't start netlog (%s)\n", status_str(st));
        jam_handle_close(mine);
        return;
    }
    rn.netlog_stop = mine;
}

void regnet_start(handle_t port, const handle_t *cards, unsigned ncards)
{
    unsigned have = 0;
    for (unsigned k = 0; k < ncards; k++)
        have += cards[k] != HANDLE_INVALID;
    /* No network on this boot, no card, or no netstack and netlog in the boot
     * image: the run is what it always was, and nothing here is said. */
    if (!init_vlan || !have || !in_bootfs("bin/netstack") || !in_bootfs("bin/netlog"))
        return;
    if (!wait_data(port, now() + RUN_DATA_WAIT)) {
        printf("init: no " DATA_MOUNT " within %d s: the settings can't be read, no network for "
               "this run\n", (int)(RUN_DATA_WAIT / NS_PER_S));
        return;
    }
    char host[SETTINGS_VALUE_MAX], nl[8], addr[SETTINGS_VALUE_MAX];
    status_t host_st = settings_get(SETTINGS_FILE, "net.host", host, sizeof(host));
    status_t nl_st = settings_get(SETTINGS_FILE, "netlog", nl, sizeof(nl));
    status_t addr_st = settings_get(SETTINGS_FILE, "net.address", addr, sizeof(addr));
    uint32_t a;
    const char *endp = NULL;
    char text[IPV4_TEXT_MAX];
    if (nl_st == OK && !strcmp(nl, "off")) {
        printf("init: netlog = off in the settings: the log is not sent over the network\n");
        return;
    }
    if (host_st != OK) {
        printf("init: no net.host in the settings: the log is not sent over the network\n");
        return;
    }
    if (!ipv4_parse(host, &a, &endp) || *endp) {
        printf("init: settings: net.host = %s is not an IPv4 address: no netlog\n", host);
        return;
    }
    ipv4_format(a, text);
    printf("init: net.host = %s: the network, netlog and logd run alongside this run's programs\n",
           text);

    handle_t sys = HANDLE_INVALID;
    status_t st = start_netstack(cards, ncards, &sys);
    if (st != OK) {
        printf("init: can't start netstack (%s): no network for this run\n", status_str(st));
        return;
    }
    rn.on = true;
    if (addr_st != OK || !send_static(addr))
        start_dhcp();
    start_logd();
    start_netlog(text, sys);
    jam_handle_close(sys);
}

/* logd's namespace follows init's, but not at every `settled`: the update
 * makes a view of /data (a call that can wait for the filesystem service),
 * and utest's mock disks and usbtest's sticks make hundreds of changes in
 * seconds, which init must read at once (volumes drops a watcher that falls
 * behind). So each change only postpones the update to MOUNTS_QUIET after
 * the last one; main.c's wait calls regnet_tick at that time. */
#define MOUNTS_QUIET (300 * NS_PER_MS)
static uint64_t mounts_due;   /* uptime ns of the pending update (0: none) */

void regnet_mounts(void)
{
    if (rn.logd_ns)
        mounts_due = now() + MOUNTS_QUIET;
}

uint64_t regnet_due(void)
{
    return mounts_due ? mounts_due : DEADLINE_NEVER;
}

void regnet_tick(void)
{
    if (!mounts_due || now() < mounts_due || !rn.logd_ns)
        return;
    mounts_due = 0;
    status_t st = ns_update(rn.logd_ns, rn.logd_back, logd_grants);
    /* It is gone, or out of memory: the next change sends the whole namespace again. */
    if (st != OK && st != ERR_PEER_CLOSED)
        printf("init: logd didn't get the new mounts (%s)\n", status_str(st));
}

void regnet_stop_logd(void)
{
    if (!rn.logd.proc)
        return;
    mounts_due = 1;   /* the latest mounts first */
    regnet_tick();
    status_t st = rn.logd_ctl ? logctl_flush_until(rn.logd_ctl, now() + FLUSH_WAIT)
                              : ERR_NOT_FOUND;
    if (st != OK && st != ERR_NOT_FOUND && st != ERR_PEER_CLOSED)
        printf("init: the boot log's last lines were not saved (%s)\n", status_str(st));
    end(&rn.logd, "logd", 0);   /* it never ends by itself: its job goes */
    handle_t hs[] = { rn.logd_ctl, rn.logd_ns, rn.logd_back };
    for (unsigned k = 0; k < 3; k++)
        if (hs[k])
            jam_handle_close(hs[k]);
    rn.logd_ctl = rn.logd_ns = rn.logd_back = HANDLE_INVALID;
}

void regnet_stop(bool ok)
{
    if (!rn.on)
        return;
    printf("init: the run is over (%s): netlog sends the last lines, then the network stops\n",
           ok ? "no problems" : "PROBLEMS");
    /* Closing the stop channel asks netlog to flush and exit; one that doesn't
     * (it never got a network) is killed after NETLOG_WAIT. */
    if (rn.netlog_stop)
        jam_handle_close(rn.netlog_stop);
    rn.netlog_stop = HANDLE_INVALID;
    end(&rn.netlog, "netlog", NETLOG_WAIT);
    end(&rn.dhcp, "dhcp", 0);
    end(&rn.netstack, "netstack", 0);
    if (rn.ctl_cli)
        jam_handle_close(rn.ctl_cli);
    rn.ctl_cli = HANDLE_INVALID;
    rn.on = false;
}
