/* init's control channels: the initctl protocol (abi/idl/initctl.idl).
 *
 * A channel per holder: the shell's answers everything; the compositor's
 * and each terminal's console's only `reboot` (Ctrl+Alt+Del) and
 * `terminal` (Super+Enter: the compositor's, or under `nocomp` nobody's).
 * Which requests a channel takes is a property of the channel, never of
 * who is asking.
 *
 * terminal opens another terminal (terms.c): a console in a window of
 * its own with a shell of its own, if a compositor runs.
 *
 * kill <name> reaches the processes init has authority over: its own
 * services (shell.c: their jobs are init's), and, through devmgr's KILL,
 * what devmgr runs for a USB device: a class driver per interface
 * ("hid-6.1:0", "usb-storage-1:0") and a filesystem service per mounted
 * partition ("fat-data", "fat-usb0"). The kernel's process list says which
 * process has the name; devmgr's bindings on each USB device are then
 * asked for theirs (GET_DRIVER) until one matches, and that binding is
 * killed. devmgr's PCI drivers ("usb-bus", "hda") are found the
 * same way among the functions with a driver bound (GET_DRIVER
 * 0xffff/0xffff); devmgr restarts them like any driver that dies. A
 * filesystem service is asked for by its mount first (DEVMGR_FS_MOUNT:
 * "fat-data" is /data's), whatever its process is called: a restarted one
 * is devmgr's warm spare promoted, which keeps the name "fat-spare".
 *
 * reboot is a kexec into a fresh copy of the system: the kernel's stored
 * one, or the files on /esp if they changed (reboot.c); if that can't be
 * done, and for reboot_firmware (`reboot -f`), it is a reset
 * through the firmware. Either way /data and every /usbN are flushed first,
 * for at most 2 s (mounts_sync), logd saves the log's last lines, that
 * one included (shell_flush_log), so a boot log ends with its own
 * shutdown, and devmgr stops every driver in order. sync flushes the same
 * way.
 *
 * mount (the shell's `mount -w /usb0`, `mount -r /usb0`) is passed on to
 * devmgr (DEVMGR_REMOUNT) for /usbN and refused for every other path:
 * /boot, /esp and /data are what they are.
 *
 * shell_ready (the shell, once it is up) lets the boot splash give the
 * screen back (splash.c). kernel_load (the shell's `kernel load`) makes
 * /esp's kernel and boot image the stored copy now (reboot.c);
 * update_offer hands out a channel for a fetched build (update.c). */
#include <devmgr.h>
#include <idl/initctl.h>
#include <idl/usbbus.h>
#include <os.h>
#include "init.h"

#define NAME_MAX    32                /* initctl.kill's name field */
#define MAX_PROCS   512               /* processes read from the kernel's list */
#define MAX_PCI_DRIVERS 64            /* bound PCI functions looked through */
#define MAX_DEVICES 128               /* USB devices looked at */
#define MAX_IFACES  8                 /* interface numbers tried on each */
#define MAX_PARTS   4                 /* partitions tried on each (an MBR's) */
#define MOUNT_WAIT  (25 * NS_PER_S)   /* devmgr's REMOUNT: a sync, then its service's stop */
#define CALL_WAIT   (5 * NS_PER_S)    /* a devmgr or usb-bus call */
#define KILL_WAIT   (15 * NS_PER_S)   /* devmgr's KILL: it waits for the driver to die */
#define ROUND       16                /* requests answered before the main loop gets a turn */

struct ctl {
    handle_t ch;      /* our end (0: none) */
    bool     admin;   /* every method; else reboot only */
    uint64_t key;     /* its port key */
};

static struct ctl ctls[CTL_COUNT] = { [CTL_SHELL] = { .admin = true } };
static handle_t ctl_port;

/* ---- kill: what devmgr runs for a USB device -------------------------------------- */

/* usb-bus: the driver devmgr bound that answers usbbus.status. */
static handle_t find_usb_bus(handle_t dm)
{
    for (uint32_t n = 0; n < 16; n++) {
        struct devmgr_rep r;
        handle_t h = HANDLE_INVALID;
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &h, 1, &nh,
                                  now() + CALL_WAIT);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        if (usbbus_status_until(h, now() + CALL_WAIT, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL) == OK)
            return h;
        jam_handle_close(h);
    }
    return HANDLE_INVALID;
}

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

/* The koid of the process devmgr runs for this binding. */
static status_t binding_koid(handle_t dm, uint16_t vendor, uint16_t device, uint32_t id,
                             uint64_t *koid)
{
    struct devmgr_rep r;
    handle_t hs[3];
    uint32_t nh = 0;
    status_t st = devmgr_call(dm, DEVMGR_GET_DRIVER, vendor, device, id, &r, hs, 3, &nh,
                              now() + CALL_WAIT);
    struct process_info info;
    if (st == OK)
        st = nh ? jam_process_get_info(hs[0], &info) : ERR_INTERNAL;
    for (uint32_t i = 0; i < nh; i++)
        jam_handle_close(hs[i]);
    if (st == OK)
        *koid = info.koid;
    return st;
}

/* devmgr's bindings that hang off USB device `id`: a class driver per
 * interface, a filesystem service per partition of a disk. Kill the one
 * whose process is `koid`. ERR_NOT_FOUND: none is. */
static status_t kill_on_device(handle_t dm, uint32_t id, uint64_t koid)
{
    static const struct { uint16_t vendor; uint16_t count; } kinds[] = {
        { DEVMGR_USB_IFACE, MAX_IFACES }, { DEVMGR_FS_SVC, MAX_PARTS },
    };
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (uint16_t n = 0; n < kinds[k].count; n++) {
            uint64_t have = 0;
            if (binding_koid(dm, kinds[k].vendor, n, id, &have) != OK || have != koid)
                continue;
            struct devmgr_rep r;
            return devmgr_call(dm, DEVMGR_KILL, kinds[k].vendor, n, id, &r, NULL, 0, NULL,
                               now() + KILL_WAIT);
        }
    }
    return ERR_NOT_FOUND;
}

/* devmgr's PCI drivers: kill the one whose process is `koid`.
 * ERR_NOT_FOUND: none is. */
static status_t kill_pci_driver(handle_t dm, uint64_t koid)
{
    for (uint32_t n = 0; n < MAX_PCI_DRIVERS; n++) {
        uint64_t have = 0;
        status_t st = binding_koid(dm, 0xffff, 0xffff, n, &have);
        if (st == ERR_NOT_FOUND)
            break;   /* past the last bound function */
        if (st != OK || have != koid)
            continue;
        struct devmgr_rep r;
        return devmgr_call(dm, DEVMGR_KILL, 0xffff, 0xffff, n, &r, NULL, 0, NULL,
                           now() + KILL_WAIT);
    }
    return ERR_NOT_FOUND;
}

/* "fat-data", "fat-esp", "fat-usb0", each maybe ending "-test": the
 * filesystem service of that mount, as devmgr names it by mount
 * (DEVMGR_FS_MOUNT). False: not such a name. */
static bool fs_mount_named(const char *name, uint16_t *which, uint32_t *flags)
{
    char m[NAME_MAX];
    if (strncmp(name, "fat-", 4))
        return false;
    snprintf(m, sizeof(m), "%s", name + 4);
    size_t n = strlen(m);
    *flags = n > 5 && !strcmp(m + n - 5, "-test") ? DEVMGR_MOUNT_TEST : 0;
    if (*flags)
        m[n - 5] = '\0';
    if (!strcmp(m, "data"))
        *which = DEVMGR_MOUNT_DATA;
    else if (!strcmp(m, "esp"))
        *which = DEVMGR_MOUNT_ESP;
    else if (!strncmp(m, "usb", 3) && m[3] >= '0' && m[3] <= '9' && !m[4])   /* /usb0../usb9 */
        *which = (uint16_t)(DEVMGR_MOUNT_USB + (unsigned)(m[3] - '0'));
    else
        return false;
    return true;
}

/* Kill the filesystem service of the mount `name` names ("fat-data"),
 * found by its mount rather than its process name: a restarted one is a
 * promoted spare, still called "fat-spare". ERR_NOT_FOUND: not such a
 * name, or no such mount. */
static status_t kill_fs_mount(handle_t dm, const char *name, uint64_t *koid)
{
    uint16_t which;
    uint32_t flags;
    if (!fs_mount_named(name, &which, &flags))
        return ERR_NOT_FOUND;
    status_t st = binding_koid(dm, DEVMGR_FS_MOUNT, which, flags, koid);
    if (st == ERR_BAD_STATE || st == ERR_NOT_FOUND)
        return ERR_NOT_FOUND;   /* none runs */
    struct devmgr_rep r;
    return st != OK ? st : devmgr_call(dm, DEVMGR_KILL, DEVMGR_FS_MOUNT, which, flags, &r, NULL,
                                       0, NULL, now() + KILL_WAIT);
}

/* Kill the process called name if devmgr runs it: a PCI function's
 * driver ("hda"), or for a USB device a class driver ("hid-6.1:0",
 * "usb-storage-1:0") or a disk's filesystem service ("fat-data"). */
static status_t kill_devmgr_process(const char *name, uint64_t *koid)
{
    handle_t dm = shell_devmgr();
    if (!dm)
        return ERR_NOT_FOUND;
    status_t st = kill_fs_mount(dm, name, koid);
    if (st != ERR_NOT_FOUND || !koid_named(name, koid))
        return st;
    st = kill_pci_driver(dm, *koid);
    if (st != ERR_NOT_FOUND)
        return st;
    handle_t bus = find_usb_bus(dm);
    if (!bus)
        return ERR_NOT_FOUND;
    for (uint32_t i = 0; i < MAX_DEVICES && st == ERR_NOT_FOUND; i++) {
        uint32_t id = 0;
        if (usbbus_device_until(bus, now() + CALL_WAIT, i, &id, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) != OK)
            break;   /* past the last device, or usb-bus is gone */
        st = kill_on_device(dm, id, *koid);
    }
    jam_handle_close(bus);
    return st;
}

/* ---- the protocol ---------------------------------------------------------------- */

static status_t op_kill(void *ctx, const uint8_t name[32], uint64_t *out_koid)
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
    if (st == ERR_NOT_FOUND)
        st = kill_devmgr_process(who, out_koid);
    printf("init: kill %s: %s\n", who, status_str(st));
    return st;
}

static status_t op_sync(void *ctx)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    mounts_sync();
    return OK;
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

static status_t op_mount(void *ctx, const uint8_t path[16], uint8_t writable)
{
    const struct ctl *c = ctx;
    unsigned n = 0;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    if (writable > 1 || strnlen((const char *)path, 16) == 16 || path[0] != '/')
        return ERR_INVALID_ARGS;
    if (!usb_mount(path, &n))
        return ERR_ACCESS_DENIED;   /* /boot, /esp, /data: not ours to change */
    handle_t dm = shell_devmgr();
    struct devmgr_rep r;
    status_t st = dm ? devmgr_call(dm, DEVMGR_REMOUNT, DEVMGR_USB_MOUNT, (uint16_t)n,
                                   writable ? DEVMGR_REMOUNT_WRITE : 0, &r, NULL, 0, NULL,
                                   now() + MOUNT_WAIT)
                     : ERR_NOT_FOUND;
    printf("init: mount %s %s: %s\n", writable ? "-w" : "-r", (const char *)path, status_str(st));
    return st;
}

static status_t op_shell_ready(void *ctx)
{
    const struct ctl *c = ctx;
    if (!c->admin)
        return ERR_ACCESS_DENIED;
    printf("init: the shell is up\n");   /* the boot's time to a shell (splash-test.sh) */
    splash_shell_ready();
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

static status_t op_terminal(void *ctx, uint8_t *out_number)
{
    (void)ctx;   /* every holder: the shell's, the compositor's and the consoles' */
    status_t st = terms_open(out_number);
    if (st != OK)
        printf("init: no new terminal: %s\n", status_str(st));
    return st;
}

static const struct initctl_ops ops = {
    .kill = op_kill, .sync = op_sync, .reboot = op_reboot, .mount = op_mount,
    .shell_ready = op_shell_ready, .reboot_firmware = op_reboot_firmware,
    .kernel_load = op_kernel_load, .update_offer = op_update_offer, .terminal = op_terminal,
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
