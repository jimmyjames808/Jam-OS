/* devmgr's protocol: how a program finds a driver devmgr
 * bound. init starts devmgr and hands every program it runs a client end
 * of devmgr's channel (startup role SR_DEVMGR).
 *
 * The wire format is the IDL's (tools/genidl.py): a request is u32 txid,
 * u32 ordinal = (DEVMGR_PROTOCOL_ID << 16) | method, then the arguments; a
 * reply is u32 txid, i32 status, then the results only if status is OK.
 * Hand-written rather than an abi/idl file because three replies carry
 * handles, which the generator can't express: its client stubs sit on
 * <jam/driver.h>'s drv_channel_call, which carries no handles in either
 * build. Teaching both (genidl.py and drv_channel_call in the kernel and
 * libos builds) would change the driver surface for one protocol whose
 * clients are ordinary programs; these few calls use libos's
 * jam_channel_call directly instead.
 *
 * Devices are named by vendor, device and instance (the n-th function with
 * those ids, from 0); 0xffff/0xffff in DRIVER_VIEW means "the first
 * function with MSI-X that isn't a bridge or the boot display".
 *
 * Trust: devmgr serves two channels. The QUERY channel (startup role
 * SR_DEVMGR) answers STATUS, GET_SERVICE, GET_DRIVER (read-only views) and
 * SUPERVISION; anything else gets ERR_ACCESS_DENIED. The CONTROL channel
 * (SR_DEVMGR_CTL) answers everything: SET_CONSOLE, KILL, REBIND, RELEASE,
 * DRIVER_VIEW (a driver's hardware handles), TEST_DRIVER, MOUNTS (the
 * filesystems' channels), TEST_DISK and REMOUNT too. devmgr
 * runs until every client end of its control channel is gone.
 * Who holds what: init both (it hands devmgr new consoles); the programs
 * init runs from init.cfg (the test suites utest and usbtest) both; in
 * shell mode the shell both, but it passes CONTROL only to the test
 * programs its `utest` and `usbtest` commands start, and nothing of
 * devmgr's to what `run` starts (a program that needs it gets it by
 * name, as those two do). GET_SERVICE's channels reach drivers (usb-bus
 * hands out USB interfaces), so even QUERY is for trusted programs only.

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
 * scratch. A client that needs devmgr for good gets the new client end
 * from whoever gave it the old ones: init sends the shell each new pair on
 * the shell's SR_USER + 2 channel (a message of one u32
 * INIT_SHELL_DEVMGR carrying two handles: query, then control); the test
 * programs the shell runs get the current ones when they start. */
#pragma once

#include <os.h>

/* init -> shell, on the shell's SR_USER + 2 channel: a new devmgr's
 * client ends (the message's two handles: query, control). */
#define INIT_SHELL_DEVMGR   1u
/* init -> the boot's first shell, on the same channel, queued before it
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
 * bound to it (ERR_BAD_STATE: bound, but the driver is gone for good).
 * While a restart is due, the channel the restarted driver will serve
 * (see the reconnect rule above). Vendor and device 0xffff name the
 * instance-th function that has a driver bound, whatever it is (a client
 * asks each in turn which protocol it serves: tests find usb-bus this way
 * on QEMU and on the PC). Don't read from the channel: messages a driver
 * writes by itself are devmgr's (usb-bus's interface_attached); only
 * channel_call it. */
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
 * ended because the old console went away start again connected to it. */
#define DEVMGR_SET_CONSOLE  0x00030009u

/* Mounts (control channel only): the boot disk's data partition at /data
 * (read-write) and its ESP at /esp (read-only), and each FAT partition of
 * any other disk at /usb0, /usb1, ... (read-only until DEVMGR_REMOUNT),
 * each served by a fat service devmgr started over usb-storage's `block`
 * channel for that partition (devmgr's storage side: disk.c, mounts.c).
 *
 * Request: a struct devmgr_req whose `instance` is `known`, the generation
 * the caller has (0: none). Reply: a struct devmgr_mounts_rep (all of it
 * when the status is OK) carrying `count` handles: each mount's `fs`
 * channel (abi/idl/fs.idl), in the order of mounts[]. They are duplicates
 * of devmgr's client end, to pass on as they are.
 *
 * devmgr answers at once when its generation differs from `known`;
 * otherwise as soon as it changes, or with ERR_TIMED_OUT after
 * DEVMGR_MOUNTS_WAIT, so a caller waits in a loop and a call never outlives
 * its caller by more than that. Give each call a deadline past
 * DEVMGR_MOUNTS_WAIT (devmgr_mounts() adds a second): a call that gives up
 * earlier leaves its answer, handles included, queued on the channel.
 * ERR_NO_RESOURCES: too many calls are waiting already.
 *
 * The generation changes whenever the list does: a mount appears, its disk
 * goes away (unplugged, or its usb-storage died), its fat service dies
 * (the mount is gone until the restart) or is restarted (it is back, with
 * a new channel: calls on the old one fail ERR_PEER_CLOSED; REMOUNT is
 * such a restart). /esp and /data are the disk Jam OS booted from:
 * partition 1 of type 0xEF holding boot/jamos.elf, partition 2 of type
 * 0x0C. Any other disk's FAT partitions (MBR types 01 04 06 0B 0C 0E EF,
 * or a disk with no table that is one FAT volume) take the lowest free
 * /usbN when they are found and give it back when the stick goes; a
 * partition with no FAT volume is never listed, and never written: only
 * the boot disk's blank data partition is ever formatted. A mount that
 * isn't listed has no service. A devmgr that init started again starts its generations
 * somewhere else (from the clock), so the old one's are never mistaken for
 * its own; asking a new devmgr with known 0 is still the simple rule. */
#define DEVMGR_MOUNTS       0x0003000au
#define DEVMGR_MAX_MOUNTS   8u
#define DEVMGR_MOUNTS_WAIT  (2 * NS_PER_S)
struct devmgr_mount {
    char path[16];   /* "/data", "/esp": NUL-terminated */
};
struct devmgr_mounts_rep {
    uint32_t txid;             /* the request's */
    int32_t  status;           /* OK: the rest follows */
    uint32_t generation;       /* never 0 */
    uint32_t count;            /* mounts, and handles with the reply */
    struct devmgr_mount mounts[DEVMGR_MAX_MOUNTS];
} __attribute__((packed));
/* (1 handle: a client end of a `storage` channel, abi/idl/storage.idl) ->
 * u32 id. A software disk for tests, which devmgr treats like a
 * usb-storage disk: the same questions, the same boot-disk check, the same
 * supervision of its filesystem services (bin/fat). One difference keeps
 * it apart from the real one: its mounts are /data-test and /esp-test, and
 * "the boot disk" is counted among test disks only, so a test never takes
 * /data away. It is gone once the channel's server end is closed.
 * ERR_NO_RESOURCES: too many disks. */
#define DEVMGR_TEST_DISK    0x0003000bu
/* (dev) -> (): stop the device's driver and leave the device without one
 * until DEVMGR_REBIND, for a test that drives the device itself (usbtest
 * and the disks). A disk's filesystems are synced first, then go with the
 * driver: its mounts are gone until the rebind. For a USB class driver the
 * reply carries 1 handle: a duplicate of the interface's `usb` channel the
 * driver had (usb-bus gives an interface's bulk endpoints to that channel
 * and no other). */
#define DEVMGR_RELEASE      0x0003000cu

/* (DEVMGR_USB_MOUNT, N, flags) -> (): the mount /usbN read-write (flags
 * DEVMGR_REMOUNT_WRITE) or read-only again (0); with DEVMGR_REMOUNT_TEST
 * a test disk's /usbN-test. Its fat service is synced and stopped, and
 * started again on a new `block` channel opened the new way (read-only:
 * usb-storage refuses every write on it), so the mount's channel is a new
 * one and files open on the old one fail ERR_PEER_CLOSED; it is listed
 * again once its volume is mounted (a new generation). Nothing is ever
 * formatted, in either mode. OK at once if it is that way already.
 * ERR_NOT_FOUND: no such mount (only /usbN can be asked for: /esp and
 * /data are what they are); ERR_BAD_STATE: its service isn't serving. */
#define DEVMGR_REMOUNT      0x0003000du
#define DEVMGR_USB_MOUNT    0xfffcu
#define DEVMGR_REMOUNT_WRITE 1u
#define DEVMGR_REMOUNT_TEST  2u

/* () -> (), control channel only: stop, as when the last control client
 * leaves (filesystems synced and stopped clean, the USB class drivers,
 * then the bus drivers with their final halt and reset), and exit. The
 * answer comes first; the caller waits for devmgr's process to end. For a
 * kexec reboot, which must leave no device writing memory, while the
 * shell still holds copies of the control channel. */
#define DEVMGR_SHUTDOWN     0x0003000eu

/* A disk's filesystem services are named by DEVMGR_FS_SVC as the vendor,
 * the partition (storage.idl's index, 0 to 3; DEVMGR_PART_* on the boot
 * disk) as the device and the
 * disk's id as the instance (a usb-storage disk: usb-bus's device id; a
 * test disk: TEST_DISK's result), for GET_DRIVER, KILL and SUPERVISION.
 * GET_SERVICE is refused (ERR_ACCESS_DENIED) for them and for a disk's
 * driver: a disk's channel is devmgr's own, and a filesystem's comes from
 * DEVMGR_MOUNTS. */
#define DEVMGR_FS_SVC       0xfffdu
#define DEVMGR_PART_ESP     0u   /* partition 1 of the boot disk: the ESP */
#define DEVMGR_PART_DATA    1u   /* partition 2: the data partition */

/* A filesystem service gets what <fatsvc.h> says and nothing else (no
 * devmgr channel, no namespace, no root resource: its file times are
 * fixed): FAT_SR_BLOCK, a `block` channel from storage.open_partition
 * (opened read-only for the ESP and for every /usbN not remounted),
 * FAT_SR_SERVE, its mount point as argv[1], and FAT_ARG_FORMAT as argv[2]
 * for the boot disk's data partition only. It is supervised like a driver: exit 0 is the end of it; a
 * crash, a kill or any other exit is restarted with backoff, each time
 * with a new `block` channel and a new `fs` channel, and given up on after
 * 5 restarts in a minute. */

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
        .wn = sizeof(q),
        .wbytes = (uint64_t)(uintptr_t)&q,
        .rcap = sizeof(*rep),
        .rbytes = (uint64_t)(uintptr_t)rep,
        .ractual = (uint64_t)(uintptr_t)&n,
        .rh = (uint64_t)(uintptr_t)hs,
        .rhcap = hs ? hcap : 0,
        .rhactual = (uint64_t)(uintptr_t)&got,
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

/* One DEVMGR_MOUNTS call on the control channel ch. OK: *out holds the
 * generation and the mounts, and hs[i] (DEVMGR_MAX_MOUNTS slots) is
 * out->mounts[i]'s `fs` channel, the caller's to close. ERR_TIMED_OUT:
 * nothing changed from `known` within DEVMGR_MOUNTS_WAIT; ask again.
 * Blocks for DEVMGR_MOUNTS_WAIT plus a second at most. */
static inline status_t devmgr_mounts(handle_t ch, uint32_t known, struct devmgr_mounts_rep *out,
                                     handle_t *hs)
{
    struct devmgr_req q = { 0, DEVMGR_MOUNTS, 0, 0, known };
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = ch,
        .wn = sizeof(q),
        .wbytes = (uint64_t)(uintptr_t)&q,
        .rcap = sizeof(*out),
        .rbytes = (uint64_t)(uintptr_t)out,
        .ractual = (uint64_t)(uintptr_t)&n,
        .rh = (uint64_t)(uintptr_t)hs,
        .rhcap = DEVMGR_MAX_MOUNTS,
        .rhactual = (uint64_t)(uintptr_t)&got,
        .deadline_ns = now() + DEVMGR_MOUNTS_WAIT + NS_PER_S,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (n >= DEVMGR_REP_HDR && out->status < 0 && !got)
        return out->status;
    if (n != sizeof(*out) || out->status != OK || out->count != got)
        st = ERR_INTERNAL;   /* not devmgr's format: nothing of it is used */
    for (uint32_t i = 0; st != OK && i < got; i++)
        jam_handle_close(hs[i]);
    return st;
}
