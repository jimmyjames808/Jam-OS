/* devmgr's protocol: how a program finds a driver devmgr
 * bound. init starts devmgr and publishes client ends of its two channels
 * as the services /svc/devmgr (queries) and /svc/devmgr-ctl (control)
 * (<os.h> SVC_DEVMGR, SVC_DEVMGR_CTL).
 *
 * The wire format is the IDL's (tools/genidl.py): a request is u32 txid,
 * u32 ordinal = (DEVMGR_PROTOCOL_ID << 16) | method, then the arguments; a
 * reply is u32 txid, i32 status, then the results only if status is OK.
 * Hand-written rather than an abi/idl file. Handle results are no longer
 * the reason (genidl has them, over drv_channel_call_h); what genidl
 * still can't express here: handle arguments (SET_CONSOLE, SET_VOLUMES; it
 * refuses them on purpose). The protocol moves to IDL with the interface
 * review (M12, docs/ROADMAP.md). These calls use libos's jam_channel_call
 * directly. The disks' filesystems and mounts were devmgr's until M12's
 * storage split: they are volumes' now (<volumes.h>, the same wire for the
 * methods that moved).
 *
 * Devices are named by vendor, device and instance (the n-th function with
 * those ids, from 0); 0xffff/0xffff in DRIVER_VIEW means "the first
 * function with MSI-X that isn't a bridge or the boot display".
 *
 * Trust: what a channel may ask is fixed by which channel it is, never by
 * who asks. devmgr serves
 *   the QUERY channel (/svc/devmgr; devmgr's own server end is its startup
 *     role SR_DEVMGR): STATUS, GET_SERVICE, GET_DRIVER (read-only views)
 *     and SUPERVISION; anything else is ERR_ACCESS_DENIED, and so is
 *     GET_SERVICE of a device that has a device channel (below). It also
 *     answers the svc protocol's connect (abi/idl/svc.idl) with a query
 *     channel of the caller's own, and init marks /svc/devmgr so, so each
 *     opener (libos's svc_open) has one: a reply that comes after its
 *     caller stopped waiting is queued for that caller alone;
 *   DEVICE channels, each scoped to one device: made by DEVICE_CHANNEL on
 *     the control channel, at most one per device at a time. One answers
 *     GET_SERVICE, GET_DRIVER and SUPERVISION about its own device and
 *     nothing else (another device, or any other call, is
 *     ERR_ACCESS_DENIED). Holding a device's channel is the right to use
 *     the device: init makes one for each HD Audio controller and gives
 *     them to the mixer (whose output streams they are), and will do the
 *     same with the NIC for netstack. While the channel has a client, the query
 *     channel won't hand that device's service out;
 *   the CONTROL channel (/svc/devmgr-ctl; SR_DEVMGR_CTL): everything, every
 *     device's service included (the tests' hdatest takes hda's output
 *     this way): SET_CONSOLE, SET_VOLUMES, KILL, REBIND, RELEASE,
 *     DRIVER_VIEW (a driver's hardware handles), TEST_DRIVER and
 *     DEVICE_CHANNEL too. devmgr runs until every client end of its
 *     control channel is gone.
 * Who holds what: init the control and query channels (it hands devmgr
 * new consoles and DISKS channels, and makes the device channels) and the
 * sound cards'
 * device channels, duplicates of which the mixer gets (SR_DEVMGR_DEVICE);
 * the programs
 * init runs from init.cfg (the test suites utest and usbtest) QUERY and
 * CONTROL; in shell mode the shell QUERY and CONTROL, and it passes them
 * on only to the programs whose lists ask for them (<wants.h>; CONTROL:
 * test programs only, which the build checks). GET_SERVICE's channels
 * reach drivers (usb-bus hands out USB interfaces), so even QUERY is for
 * trusted programs only.
 *
 * Supervision: devmgr restarts a driver that dies unexpectedly
 * (crashes, is killed by anyone, KILL included, or exits with an error;
 * one that exits 0 by itself is finished, not restarted). Backoff 100 ms,
 * doubling per restart within the last 60 s, at most 5 s; the 6th death
 * within 60 s gives up (a log line and a RESULTS line; a real driver's
 * crash or give-up also makes devmgr's exit code 1). Each restart makes
 * the device's handles from scratch: a new dma_cap (Bus Master Enable
 * off until the new driver has quiesced the device), a new interrupt
 * object, the function woken to D0.
 *
 * THE RECONNECT RULE for clients of any driver devmgr runs: when a call
 * on the service channel fails with ERR_PEER_CLOSED (the driver died),
 * ask devmgr again (GET_SERVICE) and retry on the new channel. As soon
 * as the driver has died, GET_SERVICE hands out the channel its restart
 * will serve; calls on it wait (bounded by the caller's deadline) until
 * the new driver reads them. ERR_BAD_STATE: devmgr gave up on it (or it
 * finished). A driver keeps no state across a restart: whatever a client
 * had set up through the old channel must be set up again.
 *
 * devmgr itself: in shell mode init restarts it when it dies,
 * with its whole job (every driver it ran), so every devmgr client end
 * sees ERR_PEER_CLOSED. The new devmgr binds everything again from
 * scratch. A client that needs devmgr for good opens /svc/devmgr again:
 * init publishes the new devmgr's channels under the same names and sends
 * every namespace that follows its own (the shell's) the change, so
 * libos's svc_get hands out the new one once the old one's peer is gone. */
#pragma once

#include <os.h>

/* init -> the boot's first shell, on its SR_USER + 2 channel, queued before it
 * starts: the u32, then the one line it prints under its own, without a
 * newline or a NUL (at most INIT_SHELL_NOTE_MAX bytes): what happened to
 * the boot before, if it panicked (init's lastboot.c). */
#define INIT_SHELL_NOTE     2u
#define INIT_SHELL_NOTE_MAX 256u

#define DEVMGR_PROTOCOL_ID  3u   /* next to the IDL's null (1) and edu (2) */
/* -> u32 bound, failed, skipped. Answered once the first binding pass is
 * done, so init can wait for it. */
#define DEVMGR_STATUS       0x00030001u
/* (dev) -> 1 handle: a channel to the driver's DR_SERVE end (a duplicate
 * of devmgr's client end). ERR_NOT_FOUND: no such device, or no driver
 * bound to it (ERR_BAD_STATE: bound, but the driver is gone for good;
 * ERR_ACCESS_DENIED: asked on the query channel for a device that has a
 * device channel, or on a device channel for another device).
 * While a restart is due, the channel the restarted driver will serve
 * (see the reconnect rule above). Vendor and device 0xffff name the
 * instance-th function that has a driver bound, whatever it is (a client
 * asks each in turn which protocol it serves: tests find usb-bus this way
 * on QEMU and on the PC). The channel is for calls: a driver's own
 * messages go on a channel of their own (usb-bus's DR_EVENTS). */
#define DEVMGR_GET_SERVICE  0x00030002u
/* (dev) -> 3 handles, read-only views of a bound driver: its process
 * (RIGHTS_BASIC: wait, info), its job (RIGHTS_BASIC: wait, job_get_info)
 * and its function (RIGHTS_BASIC | RIGHT_READ: config reads), plus u32
 * pci_index. ERR_BAD_STATE while no driver process runs (a restart is
 * due, or it is gone). Vendor and device 0xffff name the instance-th PCI
 * function that has a driver bound, running or not (KILL takes the same
 * numbering). */
#define DEVMGR_GET_DRIVER   0x00030003u
/* (dev) -> (): kill the driver's job and answer once it is dead. That is
 * a death like any other: supervision restarts it (not counted as a
 * problem). */
#define DEVMGR_KILL         0x00030004u
/* (dev) -> (): kill the driver if it still runs, then bind the device
 * again from scratch (new interrupt object, dma_cap, channel), with a
 * fresh restart history. */
#define DEVMGR_REBIND       0x00030005u
/* (dev) -> handles: its function with a driver's rights and each memory
 * BAR as a driver gets it, plus u32 bar_mask (bit n: BAR n, in handle
 * order after the function). The rights a driver has, without the
 * interrupt object and the dma_cap, for tests of what drivers can't do. */
#define DEVMGR_DRIVER_VIEW  0x00030006u
/* (dev) -> u32 state (DEVMGR_SUP_*), restarts (since boot), the last
 * backoff in ms, and for a PCI function its DMA quarantine: pages held now
 * and pages found written while held since boot. */
#define DEVMGR_SUPERVISION  0x00030007u
/* () -> (): start the crash-test driver (drv/crasher) as a supervised
 * driver of the software device DEVMGR_TEST_VENDOR:DEVMGR_TEST_DEVICE
 * (no hardware), unless it runs already; after a give-up it starts again
 * with a fresh restart history. Its deaths and give-up are expected (not
 * problems). ERR_NOT_FOUND: no drv/crasher in bootfs. */
#define DEVMGR_TEST_DRIVER  0x00030008u
/* (1 handle: a client end of the console's channel) -> (): the console to
 * connect class drivers to. init sends it after it restarted the
 * console (the first one comes as SR_CONSOLE); the class drivers that
 * ended because the old console went away start again connected to it.
 * A devmgr started with the argument "comp" takes an INPUT-level compctl
 * channel here and as SR_CONSOLE instead (the compositor's: init sends a
 * new one after it restarted the compositor). */
#define DEVMGR_SET_CONSOLE  0x00030009u

/* (dev) -> (): stop the device's driver and leave the device without one
 * until DEVMGR_REBIND, for a test that drives the device itself (usbtest
 * and the disks). A disk's filesystems are synced first (volumes is asked
 * on the DISKS channel: <volumes.h> DISKS_SYNC), then go with the driver:
 * its mounts are gone until the rebind. For a USB class driver the
 * reply carries 1 handle: a duplicate of the interface's `usb` channel the
 * driver had (usb-bus gives an interface's bulk endpoints to that channel
 * and no other). */
#define DEVMGR_RELEASE      0x0003000cu

/* () -> (), control channel only: stop, as when the last control client
 * leaves (the USB class drivers, then the bus drivers with their final
 * halt and reset), and exit. The answer comes first; the caller waits for
 * devmgr's process to end. For a kexec reboot, which must leave no device
 * writing memory, while the shell still holds copies of the control
 * channel. (The filesystems are volumes': init stops it first.) */
#define DEVMGR_SHUTDOWN     0x0003000eu

/* (dev) -> 1 handle, control channel only: a new DEVICE channel scoped to
 * that device (see "Trust" above), the caller's to give to the device's
 * one user. On it, a request whose vendor, device and instance are all 0
 * names its own device; naming it by its ids works too. It lasts until
 * its clients are all gone (or devmgr ends: init makes a new one for a new
 * devmgr). ERR_NOT_FOUND: no such device, or no driver for it;
 * ERR_NOT_SUPPORTED: a USB interface (only a PCI function or the
 * crash-test device can have one); ERR_BAD_STATE: it has
 * one already; ERR_NO_RESOURCES: devmgr serves as many channels as it can. */
#define DEVMGR_DEVICE_CHANNEL 0x0003000fu
/* A PCI function named by its class rather than its ids: DEVMGR_PCI_CLASS
 * as the vendor, n as the device and class << 16 | subclass << 8 | prog_if
 * as the instance: the n-th function of that class that has a driver (in
 * devmgr's enumeration order), whatever its vendor. init finds the
 * devices it gives away this way. */
#define DEVMGR_PCI_CLASS      0xfffbu
/* (1 handle: the client end of a DISKS channel, <volumes.h>) -> (): what
 * devmgr tells volumes its disks on. init makes the pair once both run,
 * and again whenever either starts again; devmgr then asks every running
 * disk's driver for a `storage` channel of volumes' own again, and hands
 * each over (DISKS_ATTACH). Until the first, a disk waits. */
#define DEVMGR_SET_VOLUMES    0x00030011u
/* devmgr's startup role: a duplicate of /svc/notify's shared channel, for
 * the desktop's notices of drivers that crash or
 * are given up on (<notice.h>). SR_USER + 0 was the ESP channel's until
 * the storage split. */
#define DEVMGR_SR_NOTIFY      (SR_USER + 1)
#define DEVMGR_CLASS_HDA      0x040300u   /* HD Audio (04 03 00): the mixer's */
#define DEVMGR_CLASS_NET      0x020000u   /* Ethernet (02 00 00): netstack's (netdev.idl) */

/* USB class drivers are named by DEVMGR_USB_IFACE as the vendor, the
 * interface number as the device and usb-bus's device id (usbbus.device's
 * `id`) as the instance, for GET_DRIVER, KILL, REBIND, RELEASE and
 * SUPERVISION. Such a binding exists from the interface's attach until it
 * is gone. */
#define DEVMGR_USB_IFACE    0xfffeu

#define DEVMGR_SUP_NONE       0u   /* no driver started (yet) */
#define DEVMGR_SUP_RUNNING    1u
#define DEVMGR_SUP_RESTARTING 2u   /* died; its restart is due */
#define DEVMGR_SUP_GAVE_UP    3u   /* died too often: no more restarts */
#define DEVMGR_SUP_FINISHED   4u   /* exited 0 by itself (a one-shot driver) */

/* The software device of the crash-test driver, and its protocol (a
 * request is u32 txid, u32 ordinal; drivers/test/crasher/crasher.c has the
 * same numbers). PING -> u32 txid, i32 status, u64 started_ns (when this
 * instance of the driver started); CRASH and EXIT(u32 code) get no reply:
 * the driver faults, or exits with the code. */
#define DEVMGR_TEST_VENDOR 0x0000u
#define DEVMGR_TEST_DEVICE 0xc4a5u
#define CRASHER_PING       0xc4a50001u
#define CRASHER_CRASH      0xc4a50002u
#define CRASHER_EXIT       0xc4a50003u

/* What devmgr gives each driver (and DRIVER_VIEW hands out). None of a
 * driver's hardware handles can be duplicated or passed on (no
 * RIGHT_DUPLICATE, no RIGHT_TRANSFER; a physical VMO made from such a BAR
 * inherits that): a driver can't smuggle its function, registers,
 * interrupt or dma_cap out through DR_SERVE to outlive it. devmgr makes
 * them with RIGHT_TRANSFER and hands
 * them over with channel_write_rights. */
#define DEVMGR_DRV_KEEP       (RIGHT_WAIT | RIGHT_INSPECT)
#define DEVMGR_DRV_DEV_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_BAR_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_MAP)
#define DEVMGR_DRV_IRQ_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_DMA_RIGHTS DEVMGR_DRV_KEEP
/* A class driver's channels (DR_USB, DR_INPUT). */
#define DEVMGR_DRV_CHAN_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)

struct devmgr_req {
    uint32_t txid;             /* stamped by channel_call */
    uint32_t ordinal;          /* DEVMGR_* */
    uint16_t vendor, device;   /* STATUS: 0 */
    uint32_t instance;         /* the n-th function with those ids, from 0 */
} __attribute__((packed));

struct devmgr_rep {
    uint32_t txid;             /* the request's */
    int32_t  status;           /* OK: the results follow */
    uint32_t a, b, c;          /* STATUS: bound, failed, skipped; GET_DRIVER: pci_index;
                                * DRIVER_VIEW: bar_mask; SUPERVISION: state, restarts,
                                * backoff_ms */
    uint32_t d, e;             /* SUPERVISION: dma_quarantined, dma_changed (pages) */
} __attribute__((packed));

#define DEVMGR_REP_HDR     8u
#define DEVMGR_MAX_HANDLES 8u

/* One call. *nh (may be NULL) gets how many handles came back into hs
 * (hcap of them at most). Results: rep->a.. when the status is OK. */
static inline status_t devmgr_call(handle_t ch, uint32_t ordinal, uint16_t vendor,
                                   uint16_t device, uint32_t instance, struct devmgr_rep *rep,
                                   handle_t *hs, uint32_t hcap, uint32_t *nh,
                                   uint64_t deadline_ns)
{
    struct devmgr_req q = { 0, ordinal, vendor, device, instance };
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = ch,
        .wr_len = sizeof(q),
        .wr_bytes = (uint64_t)(uintptr_t)&q,
        .rd_cap = sizeof(*rep),
        .rd_bytes = (uint64_t)(uintptr_t)rep,
        .rd_actual = (uint64_t)(uintptr_t)&n,
        .rd_handles = (uint64_t)(uintptr_t)hs,
        .rd_hcap = hs ? hcap : 0,
        .rd_hactual = (uint64_t)(uintptr_t)&got,
        .deadline_ns = deadline_ns,
    };
    status_t st = jam_channel_call(&a);
    if (nh)
        *nh = st == OK ? got : 0;
    if (st != OK)
        return st;
    if (n < DEVMGR_REP_HDR || rep->status > 0)
        return ERR_INTERNAL;
    return rep->status;
}

/* One DEVMGR_DEVICE_CHANNEL call on the control channel ch: *out (the
 * caller's) is the new device channel, on OK only. */
static inline status_t devmgr_device_channel(handle_t ch, uint16_t vendor, uint16_t device,
                                             uint32_t instance, uint64_t deadline_ns,
                                             handle_t *out)
{
    struct devmgr_rep r;
    handle_t h = HANDLE_INVALID;
    uint32_t nh = 0;
    status_t st = devmgr_call(ch, DEVMGR_DEVICE_CHANNEL, vendor, device, instance, &r, &h, 1,
                              &nh, deadline_ns);
    if (st == OK && nh != 1)
        st = ERR_INTERNAL;   /* not devmgr's format */
    if (st != OK) {
        if (nh)
            jam_handle_close(h);
        return st;
    }
    *out = h;
    return OK;
}
