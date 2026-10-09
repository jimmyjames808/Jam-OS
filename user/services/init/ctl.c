/* init's control channels: the initctl protocol (abi/idl/initctl.idl).
 *
 * A channel per holder: the shell's answers everything; the compositor's
 * `reboot` (Ctrl+Alt+Del), `terminal` (Super+Enter, the search box's
 * "Run ... in a terminal") and `launch` (the search box's apps); each
 * terminal's console's only `reboot` and `terminal` (without a compositor
 * nobody's Super+Enter). Which requests a channel takes is a property of
 * the channel, never of who is asking.
 *
 * terminal opens another terminal (terms.c): a console in a window of
 * its own with a shell of its own, if a compositor runs; its shell runs
 * the request's command first, if it has one. launch starts one of the
 * desktop's apps (apps.c, <deskapps.h>): only those, from the boot
 * image.
 *
 * kill <name> reaches the processes init has authority over: its own
 * services (shell.c: their jobs are init's), through devmgrctl.kill_process
 * what devmgr runs (a PCI function's driver, "usb-bus" or "hda", and for a
 * USB device a class driver per interface, "hid-6.1:0", "usb-storage-1:0"),
 * and through volumes.kill a filesystem service per mount ("fat-data" is
 * /data's, "fat-usb0-test" /usb0-test's), whatever its process is called
 * now (a restarted one is volumes' warm spare promoted, which keeps the
 * name "fat-spare", and `kill fat-spare` names the first of those: volumes
 * knows which). For devmgr's, the kernel's process list says which process
 * has the name and devmgr finds the binding that runs it; it restarts a
 * driver that dies, like any.
 *
 * reboot is a kexec into a fresh copy of the system: the kernel's stored
 * one, or the files on /esp if they changed (reboot.c); if that can't be
 * done, and for reboot_firmware (`reboot -f`), it is a reset
 * through the firmware. Either way /data and every /usbN are flushed first,
 * for at most 2 s (mounts_sync), logd saves the log's last lines, that
 * one included (shell_flush_log), so a boot log ends with its own
 * shutdown, volumes stops its filesystem services and devmgr every driver
 * in order. sync flushes the same way.
 *
 * mount (the shell's `mount -w /usb0`, `mount -r /usb0`) is passed on to
 * volumes (volumes.remount) for /usbN and refused for every other path:
 * /boot, /esp and /data are what they are.
 *
 * Answered later: mount, sync and the kill of a filesystem service wait
 * for volumes (a remount stops a service in order and starts it again,
 * seconds on a slow stick), so init doesn't wait inside the request: it
 * sends volumes the call (volumes_<method>_send) and answers the shell
 * when volumes' answer comes to the loop, or at its deadline (`laters`).
 * The answers come to the shell's channel's port key, as does a timer at
 * the earliest deadline, so ctl_serve takes them first. (The kill of a
 * name volumes doesn't know, and of everything devmgr runs, still waits
 * for devmgr: M12's track D.)
 *
 * shell_ready (the shell, once it is up) lets the boot splash give the
 * screen back (splash.c). kernel_load (the shell's `kernel load`) makes
 * /esp's kernel and boot image the stored copy now (reboot.c);
 * update_offer hands out a channel for a fetched build (update.c). */
#include <idl/devmgr.h>
#include <idl/initctl.h>
#include <os.h>
#include <volumes.h>
#include "init.h"

#define NAME_MAX    32                /* initctl.kill's name field */
#define MAX_PROCS   512               /* processes read from the kernel's list */
#define MOUNT_WAIT  (25 * NS_PER_S)   /* volumes.remount: its service's stop, then a start */
#define KILL_WAIT   (15 * NS_PER_S)   /* a KILL: devmgr or volumes waits for it to die */
#define SYNC_WAIT   (6 * NS_PER_S)    /* volumes.sync: it gives each mount 5 s */
#define ROUND       16                /* requests answered before the main loop gets a turn */
#define MAX_LATER   8                 /* requests waiting for volumes' answer at once */

struct ctl {
    handle_t ch;      /* our end (0: none) */
    bool     admin;   /* every method; else reboot and terminal (and launch: */
    bool     launch;  /* ... the compositor's) */
    uint64_t key;     /* its port key */
};

static struct ctl ctls[CTL_COUNT] = {
    [CTL_SHELL] = { .admin = true, .launch = true },
    [CTL_COMPOSITOR] = { .launch = true },
};
static handle_t ctl_port;

/* ---- kill: what devmgr runs for a USB device, and volumes for a mount --------------- */

/* The koid of the first process called name in the kernel's list. */
static bool koid_named(const char *name, uint64_t *koid)
{
    struct proc_stat *ps = malloc(MAX_PROCS * sizeof(*ps));
    if (!ps)
        return false;
    int64_t n = jam_proc_list(shell_root(), ps, MAX_PROCS);
    bool found = false;
    for (int64_t i = 0; i < n && i < MAX_PROCS && !found; i++) {
        found = !strncmp(ps[i].name, name, sizeof(ps[i].name));
        if (found)
            *koid = ps[i].koid;
    }
    free(ps);
    return found;
}

/* Kill the process called name if devmgr runs it: a PCI function's
 * driver ("hda"), or for a USB device a class driver ("hid-6.1:0",
 * "usb-storage-1:0"). The kernel's process list says which process has the
 * name; devmgr finds it among its drivers (devmgrctl.kill_process).
 * (volumes' filesystem services are volumes.kill's: later_kill.) */
static status_t kill_devmgr_process(const char *name, uint64_t *koid)
{
    handle_t dm = shell_devmgr();
    if (!dm || !koid_named(name, koid))
        return ERR_NOT_FOUND;
    return devmgrctl_kill_process_until(dm, now() + KILL_WAIT, *koid);
}

/* ---- answered later: what waits for volumes -------------------------------------- */

enum later_kind { LATER_KILL, LATER_SYNC, LATER_MOUNT };

/* A request of ours (initctl's) waiting for volumes' answer to the call it
 * made. */
struct later {
    bool            used;
    enum later_kind kind;
    struct idl_txn  txn;              /* the initctl request: a duplicate of its channel */
    handle_t        vm;               /* the volumes channel the call went on (not ours) */
    uint32_t        txid;             /* the call's, on vm */
    uint64_t        t0, deadline;     /* sent; given up then */
    char            what[NAME_MAX];   /* the name or path, for the log */
    uint8_t         writable;         /* LATER_MOUNT's */
};

static struct later laters[MAX_LATER];
static uint32_t     later_txids;      /* idl_txid_next's */
static handle_t     later_timer;      /* fires at the earliest deadline (0: not made yet) */
static uint64_t     later_key;        /* the port key the answers and the timer come under */

/* l is over: answered (st, koid), said in the log as the request was. */
static void later_finish(struct later *l, status_t st, uint64_t koid)
{
    if (l->kind == LATER_KILL) {
        printf("init: kill %s: %s\n", l->what, status_str(st));
        (void)initctl_reply_kill(l->txn, st, koid);
    } else if (l->kind == LATER_SYNC) {
        mounts_synced(st, l->t0);
        (void)initctl_reply_sync(l->txn, st);
    } else {
        printf("init: mount %s %s: %s\n", l->writable ? "-w" : "-r", l->what, status_str(st));
        (void)initctl_reply_mount(l->txn, st);
    }
    jam_handle_close(l->txn.ch);
    *l = (struct later){ 0 };
}

/* volumes' answer to l (rep, m) has come. */
static void later_answer(struct later *l, const void *rep, struct idl_msg *m)
{
    uint64_t koid = 0;
    status_t st;
    if (l->kind == LATER_KILL) {
        st = volumes_kill_result(rep, m, &koid);
        if (st == ERR_NOT_FOUND || st == ERR_INVALID_ARGS)
            st = kill_devmgr_process(l->what, &koid);   /* not volumes': devmgr's, maybe */
    } else if (l->kind == LATER_SYNC) {
        st = volumes_sync_result(rep, m);
    } else {
        st = volumes_remount_result(rep, m);
    }
    later_finish(l, st, koid);
}

/* Watch for what the requests waiting need: each channel they called on
 * (ONCE: it fires again only when bound again) and the timer, at the
 * earliest deadline. */
static void later_arm(void)
{
    uint64_t first = DEADLINE_NEVER;
    for (unsigned i = 0; i < MAX_LATER; i++) {
        struct later *l = &laters[i];
        if (!l->used)
            continue;
        first = l->deadline < first ? l->deadline : first;
        (void)jam_port_unbind(ctl_port, l->vm, later_key);   /* none: nothing to undo */
        (void)jam_port_bind(ctl_port, l->vm, later_key, SIG_READABLE | SIG_PEER_CLOSED,
                            PORT_BIND_ONCE);   /* fires at once if an answer is there */
    }
    if (!later_timer)
        return;
    (void)jam_port_unbind(ctl_port, later_timer, later_key);
    if (first == DEADLINE_NEVER) {
        (void)jam_timer_cancel(later_timer);
        return;
    }
    if (jam_timer_set(later_timer, first) == OK)
        (void)jam_port_bind(ctl_port, later_timer, later_key, SIG_SIGNALED, PORT_BIND_ONCE);
}

/* Take volumes' answers to the requests waiting, and answer those whose
 * time is up (or whose volumes is gone). */
static void later_poll(void)
{
    for (unsigned i = 0; i < MAX_LATER; i++) {
        handle_t vm = laters[i].used ? laters[i].vm : HANDLE_INVALID;
        for (int guard = 0; vm && guard < 64; guard++) {
            _Alignas(8) uint8_t rep[VOLUMES_REP_MAX];
            struct idl_msg m;
            status_t st = idl_reply_read(vm, rep, sizeof(rep), &m);
            if (st == ERR_INTERNAL)
                continue;
            if (st != OK) {
                for (unsigned k = 0; st == ERR_PEER_CLOSED && k < MAX_LATER; k++)
                    if (laters[k].used && laters[k].vm == vm)
                        later_finish(&laters[k], ERR_PEER_CLOSED, 0);   /* volumes ended */
                break;
            }
            struct later *l = NULL;
            for (unsigned k = 0; k < MAX_LATER && !l; k++)
                if (laters[k].used && laters[k].vm == vm && laters[k].txid == m.txid)
                    l = &laters[k];
            if (l)
                later_answer(l, rep, &m);
            else
                idl_msg_drop(&m);   /* an answer that came after we gave up */
        }
    }
    uint64_t t = now();
    for (unsigned i = 0; i < MAX_LATER; i++) {
        struct later *l = &laters[i];
        if (l->used && t >= l->deadline)
            later_finish(l, ERR_TIMED_OUT, 0);
        else if (l->used && l->vm != shell_volumes())
            later_finish(l, ERR_PEER_CLOSED, 0);   /* volumes was started again meanwhile */
    }
    later_arm();
}

/* A request txn to be answered once volumes answers (or within wait):
 * its slot, with a txid for the call; NULL: no room (ERR_NO_RESOURCES). */
static struct later *later_new(enum later_kind kind, struct idl_txn txn, handle_t vm,
                               const char *what, uint64_t wait)
{
    struct later *l = NULL;
    for (unsigned i = 0; i < MAX_LATER && !l; i++)
        if (!laters[i].used)
            l = &laters[i];
    if (!l || jam_handle_duplicate(txn.ch, RIGHT_SAME, &l->txn.ch) != OK)
        return NULL;
    if (!later_timer && jam_timer_create(&later_timer) != OK)
        later_timer = HANDLE_INVALID;   /* deadlines are then looked at with each request */
    later_key = ctls[CTL_SHELL].key;   /* mount, sync, kill: the shell's channel's alone */
    l->used = true;
    l->kind = kind;
    l->txn.txid = txn.txid;
    l->vm = vm;
    l->txid = idl_txid_next(&later_txids);
    l->t0 = now();
    l->deadline = l->t0 + wait;
    snprintf(l->what, sizeof(l->what), "%s", what);
    return l;
}

/* The call for l went (st): IDL_LATER, or st if it couldn't (l freed). */
static status_t later_sent(struct later *l, status_t st)
{
    if (st != OK) {
        jam_handle_close(l->txn.ch);
        *l = (struct later){ 0 };
        return st;
    }
    later_arm();
    return IDL_LATER;
}

/* volumes.kill of the filesystem service called name ("fat-data", or
 * "fat-spare": volumes finds it by name), answered later. ERR_NOT_FOUND:
 * no volumes to ask. */
static status_t later_kill(struct idl_txn txn, const char *name)
{
    handle_t vm = shell_volumes();
    if (!vm)
        return ERR_NOT_FOUND;
    struct later *l = later_new(LATER_KILL, txn, vm, name, KILL_WAIT);
    if (!l)
        return ERR_NO_RESOURCES;
    return later_sent(l, volumes_kill_send(vm, l->txid, name, (uint32_t)strlen(name)));
}

/* ---- the protocol ---------------------------------------------------------------- */

static status_t op_kill(void *ctx, struct idl_txn txn, const uint8_t name[32],
                        uint64_t *out_koid)
{
    const struct ctl *c = ctx;
    const char *who = (const char *)name;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    size_t len = strnlen(who, NAME_MAX);
    if (!len || len == NAME_MAX)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < len; i++)
        if (who[i] <= ' ' || who[i] > '~')
            return ERR_INVALID_ARGS;
    if (!strcmp(who, "init"))
        return ERR_ACCESS_DENIED;   /* nobody would restart anything afterwards */
    status_t st = shell_kill_service(who, out_koid);
    if (st == ERR_NOT_FOUND && !strncmp(who, "fat-", 4)) {
        st = later_kill(txn, who);   /* a filesystem service: volumes', answered later */
        if (st == IDL_LATER)
            return st;
        if (st != ERR_NO_RESOURCES)
            st = ERR_NOT_FOUND;   /* volumes can't be asked: devmgr's, as any other name */
    }
    if (st == ERR_NOT_FOUND)
        st = kill_devmgr_process(who, out_koid);
    printf("init: kill %s: %s\n", who, status_str(st));
    return st;
}

static status_t op_sync(void *ctx, struct idl_txn txn)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    handle_t vm = shell_volumes();
    if (!vm)
        return OK;   /* no volumes: nothing was written */
    struct later *l = later_new(LATER_SYNC, txn, vm, "", SYNC_WAIT);
    if (!l)
        return ERR_NO_RESOURCES;
    return later_sent(l, volumes_sync_send(vm, l->txid));
}

static status_t op_reboot(void *ctx)
{
    (void)ctx;
    (void)init_reboot_kexec();   /* comes back only if it failed, having said why */
    printf("init: rebooting through the firmware instead\n");
    return init_reboot_firmware();
}

static status_t op_reboot_firmware(void *ctx)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    return init_reboot_firmware();
}

/* "/usbN" -> N; false for any other path. */
static bool usb_mount(const uint8_t path[16], unsigned *n)
{
    static const char prefix[] = USB_MOUNT;
    const char *p = (const char *)path;
    size_t len = strnlen(p, 16), at = sizeof(prefix) - 1;
    if (len <= at || len == 16 || strncmp(p, prefix, at) != 0)
        return false;
    *n = 0;
    for (; at < len; at++) {
        if (p[at] < '0' || p[at] > '9' || *n > 99)
            return false;
        *n = *n * 10 + (unsigned)(p[at] - '0');
    }
    return true;
}

static status_t op_mount(void *ctx, struct idl_txn txn, const uint8_t path[16],
                         uint8_t writable)
{
    const struct ctl *c = ctx;
    unsigned n = 0;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    if (writable > 1 || strnlen((const char *)path, 16) == 16 || path[0] != '/')
        return ERR_INVALID_ARGS;
    if (!usb_mount(path, &n))
        return ERR_ACCESS_DENIED;   /* /boot, /esp, /data: not ours to change */
    handle_t vm = shell_volumes();
    const char *p = (const char *)path;
    struct later *l = vm ? later_new(LATER_MOUNT, txn, vm, p, MOUNT_WAIT) : NULL;
    status_t st = !vm ? ERR_NOT_FOUND : !l ? ERR_NO_RESOURCES : OK;
    if (l) {
        l->writable = writable;
        st = later_sent(l, volumes_remount_send(vm, l->txid, p, (uint32_t)strnlen(p, 16),
                                                writable));
        if (st == IDL_LATER)
            return st;   /* said and answered when volumes answers */
    }
    printf("init: mount %s %s: %s\n", writable ? "-w" : "-r", p, status_str(st));
    return st;
}

static status_t op_shell_ready(void *ctx)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    printf("init: the shell is up\n");   /* the boot's time to a shell (splash-test.sh) */
    splash_shell_ready();
    lastboot_shell_ready();
    return OK;
}

static status_t op_kernel_load(void *ctx, uint64_t *kernel_bytes, uint64_t *bootfs_bytes,
                               uint32_t *read_ms)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    return init_kernel_load(kernel_bytes, bootfs_bytes, read_ms);
}

static status_t op_update_offer(void *ctx, handle_t *out_offer)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    return update_offer_new(ctl_port, KEY_UPDATE, out_offer);
}

static status_t op_terminal(void *ctx, const uint8_t command[TERM_CMD_MAX], uint8_t *out_number)
{
    (void)ctx;   /* every holder: the shell's, the compositor's and the consoles' */
    const char *cmd = (const char *)command;
    size_t n = strnlen(cmd, TERM_CMD_MAX);
    if (n == TERM_CMD_MAX)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < n; i++)
        if (command[i] < ' ' || command[i] == 0x7f)
            return ERR_INVALID_ARGS;   /* one line, as typed */
    status_t st = terms_open(cmd, out_number);
    if (st != OK)
        printf("init: no new terminal: %s\n", status_str(st));
    return st;
}

static status_t op_launch(void *ctx, const uint8_t app[16], uint64_t *out_koid)
{
    const struct ctl *c = ctx;
    if (!c->launch)
        return ERR_ACCESS_DENIED;
    char name[16];
    memcpy(name, app, sizeof(name));
    if (strnlen(name, sizeof(name)) == sizeof(name))
        return ERR_INVALID_ARGS;
    return apps_launch(name, out_koid);
}

static const struct initctl_ops ops = {
    .kill = op_kill, .sync = op_sync, .reboot = op_reboot, .mount = op_mount,
    .shell_ready = op_shell_ready, .reboot_firmware = op_reboot_firmware,
    .kernel_load = op_kernel_load, .update_offer = op_update_offer, .terminal = op_terminal,
    .launch = op_launch,
};

static void ctl_close(struct ctl *c)
{
    if (!c->ch)
        return;
    jam_port_unbind(ctl_port, c->ch, c->key);
    jam_handle_close(c->ch);
    c->ch = HANDLE_INVALID;
}

status_t ctl_new(unsigned who, handle_t port, uint64_t key, handle_t *client)
{
    struct ctl *c = &ctls[who];
    handle_t mine, theirs;
    ctl_close(c);
    ctl_port = port;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    c->ch = mine;
    c->key = key;
    *client = theirs;
    return OK;
}

void ctl_serve(unsigned who)
{
    struct ctl *c = &ctls[who];
    status_t st = OK;
    later_poll();   /* volumes' answers and the timer come under the shell's key */
    for (unsigned n = 0; st == OK && c->ch && n < ROUND; n++)
        st = initctl_serve_one(c->ch, &ops, c);
    if (st == OK && c->ch) {
        /* More may be queued, and the binding fires only on a new edge. */
        struct port_packet pkt = { .key = c->key, .type = PORT_PACKET_USER };
        (void)jam_port_queue(ctl_port, &pkt);   /* a full port: served with the next request */
    } else if (st != ERR_SHOULD_WAIT) {
        ctl_close(c);   /* its holder is gone */
    }
}
