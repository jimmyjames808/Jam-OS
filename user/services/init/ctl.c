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
 * volumes (volumes.remount) for /usbN and /nvmeN (volumes decides: an
 * NVMe drive's mount only if its disk is Jam OS's) and refused for every
 * other path: /boot, /esp and /data are what they are.
 *
 * disks (the shell's `disks`, `disk`) asks volumes for a new channel that
 * speaks voldisks alone (volumes.list_channel): it lists and hashes and
 * changes nothing, so the shell needs no more of volumes than that.
 *
 * prepare_disk (the shell's `disk prepare nvme1`, after the owner typed the
 * last four characters of the drive's serial, and `erase` for a drive that
 * isn't empty) asks volumes on the install channel, init's alone
 * (prepare.c), to make an NVMe drive a Jam OS disk.
 *
 * install (the shell's `install nvme1`, the same confirmations) is the
 * installer (install.c): Jam OS written to the drive on a thread of its own,
 * which answers the shell itself when it is done.
 *
 * Answered later: mount, sync, prepare_disk and the kill of a filesystem service wait
 * for volumes (a remount stops a service in order and starts it again,
 * seconds on a slow stick), so init doesn't wait inside the request: it
 * sends volumes the call (volumes_<method>_send) and answers the shell
 * when volumes' answer comes to the loop, or at its deadline (`laters`).
 * The answers come to the shell's channel's port key, as does a timer at
 * the earliest deadline, so ctl_serve takes them first. The kill of a name
 * volumes doesn't know, or that is no service of init's, goes to devmgr
 * the same way (devmgrctl.kill_process, dm.c: devmgr's answer comes to
 * the loop under its own key, KEY_DEVMGR, and finishes the request).
 *
 * shell_ready (the shell, once it is up) lets the boot splash give the
 * screen back (splash.c). kernel_load (the shell's `kernel load`) makes
 * /esp's kernel and boot image the stored copy now (reboot.c);
 * update_offer hands out a channel for a fetched build (update.c), and
 * pkg_channel one for bin/pkg's approvals of a package's programs (pkg.c). */
#include <idl/devmgr.h>
#include <idl/initctl.h>
#include <os.h>
#include <volumes.h>
#include "init.h"

#define NAME_MAX    (INITCTL_NAME_MAX + 1)      /* initctl.kill's name, with a NUL */
#define MOUNT_NAME_MAX (INITCTL_PATH_MAX + 1)   /* initctl.mount's path, with a NUL */
#define APP_NAME_MAX   (INITCTL_APP_MAX + 1)    /* initctl.launch's app, with a NUL */
_Static_assert(TERM_CMD_MAX == INITCTL_COMMAND_MAX + 1, "initctl.terminal's command and ours");
#define MAX_PROCS   512               /* processes read from the kernel's list */
#define MOUNT_WAIT  (25 * NS_PER_S)   /* volumes.remount: its service's stop, then a start */
#define KILL_WAIT   (15 * NS_PER_S)   /* a KILL: devmgr or volumes waits for it to die */
#define SYNC_WAIT   (6 * NS_PER_S)    /* volumes.sync: it gives each mount 5 s */
#define LIST_WAIT   (2 * NS_PER_S)    /* volumes.list_channel: answered at once */
#define PREPARE_WAIT (660 * NS_PER_S) /* volinstall.prepare: it gives the format 10 minutes */
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

/* ---- answered later: what waits for volumes -------------------------------------- */

enum later_kind { LATER_KILL, LATER_SYNC, LATER_MOUNT, LATER_DEVMGR_KILL, LATER_PREPARE };

/* A request of ours (initctl's) waiting for volumes' answer to the call it
 * made (or devmgr's, for LATER_DEVMGR_KILL: dm.c reads that one). */
struct later {
    bool            used;
    enum later_kind kind;
    struct idl_txn  txn;              /* the initctl request: a duplicate of its channel */
    handle_t        vm;               /* the volumes channel the call went on (not ours) */
    uint32_t        txid;             /* the call's, on vm */
    uint64_t        t0, deadline;     /* sent; given up then */
    uint64_t        koid;             /* LATER_DEVMGR_KILL: the process asked to be killed */
    char            what[NAME_MAX];   /* the name or path, for the log */
    uint8_t         writable;         /* LATER_MOUNT's */
    uint32_t        disk;             /* LATER_PREPARE's: the disk's id */
};

static struct later laters[MAX_LATER];
static uint32_t     later_txids;      /* idl_txid_next's */
static handle_t     later_timer;      /* fires at the earliest deadline (0: not made yet) */
static uint64_t     later_key;        /* the port key the answers and the timer come under */

/* l is over: answered (st, koid), said in the log as the request was. */
static void later_finish(struct later *l, status_t st, uint64_t koid)
{
    if (l->kind == LATER_KILL || l->kind == LATER_DEVMGR_KILL) {
        printf("init: kill %s: %s\n", l->what, status_str(st));
        (void)initctl_reply_kill(l->txn, st, koid);
    } else if (l->kind == LATER_SYNC) {
        mounts_synced(st, l->t0);
        (void)initctl_reply_sync(l->txn, st);
    } else if (l->kind == LATER_PREPARE) {
        prepare_say(l->disk, st, l->t0);
        (void)initctl_reply_prepare_disk(l->txn, st);
    } else {
        printf("init: mount %s %s: %s\n", l->writable ? "-w" : "-r", l->what, status_str(st));
        (void)initctl_reply_mount(l->txn, st);
    }
    jam_handle_close(l->txn.ch);
    *l = (struct later){ 0 };
}

/* devmgr's answer to the kill l asked it for (dm.c): the request is answered. */
static void devmgr_kill_done(void *arg, status_t st, const struct dm_counts *counts)
{
    struct later *l = arg;
    (void)counts;
    later_finish(l, st, st == OK ? l->koid : 0);
}

/* Ask devmgr to kill the process called l->what, if it runs one: the
 * kernel's process list says which process has the name, devmgr finds the
 * driver of it (devmgrctl.kill_process, answered to the loop: dm.c). l is
 * then waiting for devmgr, not volumes. ERR_NOT_FOUND: no devmgr, or no
 * such process (l is as it was). */
static status_t later_devmgr_kill(struct later *l)
{
    handle_t dm = shell_devmgr();
    uint64_t koid;
    if (!dm || !koid_named(l->what, &koid))
        return ERR_NOT_FOUND;
    status_t st = dm_kill_process(dm, koid, KILL_WAIT, devmgr_kill_done, l);
    if (st != OK)
        return st == ERR_NO_RESOURCES ? st : ERR_NOT_FOUND;
    l->kind = LATER_DEVMGR_KILL;
    l->vm = HANDLE_INVALID;
    l->koid = koid;
    l->deadline = DEADLINE_NEVER;   /* dm.c times the call out */
    return OK;
}

/* volumes' answer to l (rep, m) has come. */
static void later_answer(struct later *l, const void *rep, struct idl_msg *m)
{
    uint64_t koid = 0;
    status_t st;
    if (l->kind == LATER_KILL) {
        st = volumes_kill_result(rep, m, &koid);
        if ((st == ERR_NOT_FOUND || st == ERR_INVALID_ARGS) && later_devmgr_kill(l) == OK)
            return;   /* not volumes': devmgr's, maybe; its answer finishes l */
    } else if (l->kind == LATER_SYNC) {
        st = volumes_sync_result(rep, m);
    } else if (l->kind == LATER_PREPARE) {
        st = prepare_result(rep, m);
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
        if (l->kind == LATER_DEVMGR_KILL)
            continue;   /* dm.c's: it watches devmgr's channel and times the call out */
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
        else if (l->used && l->kind != LATER_DEVMGR_KILL &&
                 l->vm != (l->kind == LATER_PREPARE ? shell_volumes_install() : shell_volumes()))
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

/* The kill of a name that is neither init's service nor a filesystem
 * service: devmgr's driver, maybe, answered later (later_devmgr_kill).
 * IDL_LATER, or why not (ERR_NOT_FOUND: devmgr runs no process called
 * name). */
static status_t later_devmgr_name(struct idl_txn txn, const char *name)
{
    struct later *l = later_new(LATER_KILL, txn, HANDLE_INVALID, name, KILL_WAIT);
    if (!l)
        return ERR_NO_RESOURCES;
    status_t st = later_devmgr_kill(l);
    if (st != OK) {
        jam_handle_close(l->txn.ch);
        *l = (struct later){ 0 };
        return st;
    }
    return IDL_LATER;
}

/* ---- the protocol ---------------------------------------------------------------- */

static status_t op_kill(void *ctx, struct idl_txn txn, const char *name, uint32_t name_len,
                        uint64_t *out_koid)
{
    const struct ctl *c = ctx;
    char who[NAME_MAX];   /* name_len is at most NAME_MAX - 1: the protocol's cap */
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    if (!name_len || name_len >= sizeof(who))
        return ERR_INVALID_ARGS;
    memcpy(who, name, name_len);
    who[name_len] = '\0';
    for (size_t i = 0; i < name_len; i++)
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
    if (st == ERR_NOT_FOUND) {
        st = later_devmgr_name(txn, who);   /* a driver of devmgr's, answered later */
        if (st == IDL_LATER)
            return st;
    }
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

/* "/usbN" or "/nvmeN" -> N; false for any other path. */
static bool usb_mount(const char *p, size_t len, unsigned *n)
{
    static const char prefix[] = USB_MOUNT, nvme[] = NVME_MOUNT;
    size_t at = sizeof(prefix) - 1;
    if (len > sizeof(nvme) - 1 && strncmp(p, nvme, sizeof(nvme) - 1) == 0)
        at = sizeof(nvme) - 1;
    else if (len <= at || strncmp(p, prefix, at) != 0)
        return false;
    *n = 0;
    for (; at < len; at++) {
        if (p[at] < '0' || p[at] > '9' || *n > 99)
            return false;
        *n = *n * 10 + (unsigned)(p[at] - '0');
    }
    return true;
}

static status_t op_mount(void *ctx, struct idl_txn txn, const char *path, uint32_t path_len,
                         uint8_t writable)
{
    const struct ctl *c = ctx;
    char p[MOUNT_NAME_MAX];   /* path_len is at most MOUNT_NAME_MAX - 1: the protocol's cap */
    unsigned n = 0;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    if (writable > 1 || !path_len || path_len >= sizeof(p) || path[0] != '/')
        return ERR_INVALID_ARGS;
    memcpy(p, path, path_len);
    p[path_len] = '\0';
    if (!usb_mount(p, path_len, &n))
        return ERR_ACCESS_DENIED;   /* /boot, /esp, /data: not ours to change */
    handle_t vm = shell_volumes();
    struct later *l = vm ? later_new(LATER_MOUNT, txn, vm, p, MOUNT_WAIT) : NULL;
    status_t st = !vm ? ERR_NOT_FOUND : !l ? ERR_NO_RESOURCES : OK;
    if (l) {
        l->writable = writable;
        st = later_sent(l, volumes_remount_send(vm, l->txid, p, path_len, writable));
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

/* initctl.pkg_channel: a channel for bin/pkg's approvals (pkg.c). */
static status_t op_pkg_channel(void *ctx, handle_t *out_pkg)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    return pkg_channel_new(ctl_port, KEY_PKG, out_pkg);
}

static status_t op_terminal(void *ctx, const char *command, uint32_t command_len,
                            uint8_t *out_number)
{
    (void)ctx;   /* every holder: the shell's, the compositor's and the consoles' */
    char cmd[TERM_CMD_MAX];   /* command_len is at most TERM_CMD_MAX - 1: the protocol's cap */
    if (command_len >= sizeof(cmd))
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < command_len; i++)
        if ((uint8_t)command[i] < ' ' || command[i] == 0x7f)
            return ERR_INVALID_ARGS;   /* one line, as typed */
    memcpy(cmd, command, command_len);
    cmd[command_len] = '\0';
    status_t st = terms_open(cmd, out_number);
    if (st != OK)
        printf("init: no new terminal: %s\n", status_str(st));
    return st;
}

/* initctl.prepare_disk: volumes (volinstall.prepare) makes an NVMe drive a
 * Jam OS disk, answered later. The owner's confirmation of an erase goes on
 * as it came: volumes and the driver check it. */
static status_t op_prepare_disk(void *ctx, struct idl_txn txn, uint32_t id, const char *tail,
                                uint32_t tail_len, const char *word, uint32_t word_len)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    if (!shell_volumes_install())
        return ERR_NOT_FOUND;
    if (install_busy())
        return ERR_BAD_STATE;   /* the installer has the install channel */
    char what[NAME_MAX];
    snprintf(what, sizeof(what), "%08x", id);
    struct later *l = later_new(LATER_PREPARE, txn, shell_volumes_install(), what, PREPARE_WAIT);
    if (!l)
        return ERR_NO_RESOURCES;
    l->disk = id;
    printf("init: prepare disk %s%s\n", what, word_len ? " (with an erase's confirmation)" : "");
    return later_sent(l, prepare_send(l->txid, id, tail, tail_len, word, word_len));
}

/* initctl.install: Jam OS written to an internal drive (install.c), answered
 * by its worker thread when it is done. The install channel is one caller's
 * at a time: not while a prepare_disk waits on it. */
static status_t op_install(void *ctx, struct idl_txn txn, uint32_t id, const char *tail,
                           uint32_t tail_len, const char *word, uint32_t word_len)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    for (unsigned i = 0; i < MAX_LATER; i++)
        if (laters[i].used && laters[i].kind == LATER_PREPARE)
            return ERR_BAD_STATE;
    return install_start(txn.ch, txn.txid, id, tail, tail_len, word, word_len);
}

/* initctl.disks: a channel to volumes that lists the disks and asks for a
 * digest and the health log, and changes nothing (voldisks). */
static status_t op_disks(void *ctx, handle_t *out_list)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    handle_t vm = shell_volumes();
    return vm ? volumes_list_channel_until(vm, now() + LIST_WAIT, out_list) : ERR_NOT_FOUND;
}

static status_t op_launch(void *ctx, const char *app, uint32_t app_len, uint64_t *out_koid)
{
    const struct ctl *c = ctx;
    char name[APP_NAME_MAX];   /* app_len is at most APP_NAME_MAX - 1: the protocol's cap */
    if (!c->launch)
        return ERR_ACCESS_DENIED;
    if (app_len >= sizeof(name))
        return ERR_INVALID_ARGS;
    memcpy(name, app, app_len);
    name[app_len] = '\0';
    return apps_launch(name, c->admin, out_koid);   /* the shell's: test programs too */
}

static const struct initctl_ops ops = {
    .kill = op_kill, .sync = op_sync, .reboot = op_reboot, .mount = op_mount,
    .shell_ready = op_shell_ready, .reboot_firmware = op_reboot_firmware,
    .kernel_load = op_kernel_load, .update_offer = op_update_offer, .terminal = op_terminal,
    .launch = op_launch, .disks = op_disks, .prepare_disk = op_prepare_disk,
    .install = op_install,
    .pkg_channel = op_pkg_channel,
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
        /* More may be queued, and the binding fires only on a new edge
         * (and none comes while the channel stays readable). Unwatched,
         * the channel goes: its holder sees it end, not a deaf init. */
        if (port_again(ctl_port, c->ch, c->key, SIG_READABLE | SIG_PEER_CLOSED) != OK)
            ctl_close(c);
    } else if (st != ERR_SHOULD_WAIT) {
        ctl_close(c);   /* its holder is gone */
    }
}
