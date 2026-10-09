/* volumes' protocol: the disks and their filesystem services (bin/fat),
 * and the mounts they make (user/services/volumes). init starts volumes
 * right after devmgr and holds its control channel; tests reach it as the
 * service /svc/volumes (<os.h> SVC_VOLUMES: a channel per opener, the svc
 * protocol's connect; tools/checkwants.py lets only user/tests/ ask for
 * it).
 *
 * The wire is devmgr's (<devmgr.h>), as it was before the storage split
 * (M12 S1b): the same protocol id, ordinals, request and reply, so a
 * method that moved here kept its number and its format. Hand-written,
 * like devmgr's; it moves to an .idl file with M12's track S2a.
 *
 * Trust: what a channel may ask is fixed by which channel it is. volumes
 * serves
 *   the CONTROL channel (its startup role VOLUMES_SR_CTL; init holds its
 *     client end and publishes it as /svc/volumes, and each opener gets a
 *     control channel of its own): everything but ESP_WRITE. volumes runs
 *     until every client end of init's control channel is gone (the
 *     openers' own channels don't count), or until SHUTDOWN;
 *   the ESP channel (VOLUMES_SR_ESP; init made it, holds the client end
 *     and never hands it on): ESP_WRITE and nothing else, and no other
 *     channel may ask for that;
 *   the DISKS channel (VOLUMES_SR_DISKS, then SET_DISKS): the disks
 *     devmgr's drivers serve, told by devmgr (below).
 *
 * Supervision of the filesystem services, by the rule of a service that
 * outlives its process (docs/M11.6-PLAN.md, Q5; <supervise.h>): exit 0 is
 * the end of it; a deliberate kill (KILL) is not counted and is restarted
 * at once; a crash or any other exit is counted, the first in a minute
 * restarted at once, later ones with backoff (100 ms doubling to 5 s), and
 * the 6th counted death in a minute gives up. Each restart gets a new
 * `block` channel and the same `fs` channel. A restart is the warm spare,
 * promoted: one fat started with nothing but SR_STANDBY (process name
 * "fat-spare"), waiting for whichever mount needs it first; a new spare
 * starts shortly after. Without one (the boot word `nospare`, or none
 * ready yet) a new process is started.
 *
 * volumes itself: init restarts it when it dies, with its whole job
 * (every filesystem service), so every mount goes and comes back fresh
 * once the new volumes has looked at the disks again (devmgr hands it
 * each disk again): what the services held and had not written is lost,
 * as when devmgr died before the split. A client that needs it for good
 * opens /svc/volumes again. */
#pragma once

#include <os.h>

#define VOLUMES_PROTOCOL_ID 3u   /* devmgr's: the wire of the methods that moved */

/* A filesystem service is named by VOLUMES_FS_SVC as the vendor, the
 * partition (storage.idl's index, 0 to 3; VOLUMES_PART_* on the boot
 * disk) as the device and the disk's id as the instance (a usb-storage
 * disk: usb-bus's device id; a test disk: TEST_DISK's result); or by its
 * mount: VOLUMES_FS_MOUNT as the vendor, VOLUMES_MOUNT_ESP,
 * VOLUMES_MOUNT_DATA (the boot disk's /esp, /data) or VOLUMES_MOUNT_USB +
 * N (/usbN) as the device, and 0 or VOLUMES_MOUNT_TEST (a test disk's:
 * /esp-test, /data-test, /usbN-test) as the instance. A promoted spare
 * keeps its process name, "fat-spare", so init's `kill fat-data` asks by
 * the mount. Any other vendor: ERR_NOT_FOUND. */
#define VOLUMES_FS_SVC      0xfffdu
#define VOLUMES_PART_ESP    0u   /* partition 1 of the boot disk: the ESP */
#define VOLUMES_PART_DATA   1u   /* partition 2: the data partition */
#define VOLUMES_FS_MOUNT    0xfffau
#define VOLUMES_MOUNT_ESP   0u
#define VOLUMES_MOUNT_DATA  1u
#define VOLUMES_MOUNT_USB   0x100u
#define VOLUMES_MOUNT_TEST  1u

/* (fs) -> ERR_ACCESS_DENIED, always: a filesystem's channel is MOUNTS's
 * to give, never GET_SERVICE's. */
#define VOLUMES_GET_SERVICE 0x00030002u
/* (fs) -> 2 handles, read-only views of its running instance: its process
 * (RIGHTS_BASIC: wait, info) and its job; u32 a is 0. ERR_BAD_STATE while
 * no instance runs (a restart is due); ERR_NOT_FOUND: no such service. */
#define VOLUMES_GET_DRIVER  0x00030003u
/* (fs) -> (): kill its instance's job and answer once it is dead: a
 * deliberate kill, not counted toward giving up, and restarted at once
 * (the warm spare promoted). OK at once if no instance runs. */
#define VOLUMES_KILL        0x00030004u
/* (fs) -> u32 state (VOLUMES_SUP_*), restarts (since boot), the last
 * backoff in ms, the restarts that promoted the warm spare, and 1 while a
 * spare waits. */
#define VOLUMES_SUPERVISION 0x00030007u

/* Mounts (control channel only): the boot disk's data partition at /data
 * (read-write) and its ESP at /esp (read-only; while init has it
 * writable, ESP_WRITE, it is no mount at all), and each FAT partition of
 * any other disk at /usb0, /usb1, ... (read-only until REMOUNT), each
 * served by a fat service volumes started over usb-storage's `block`
 * channel for that partition.
 *
 * Request: a struct volumes_req whose `instance` is `known`, the
 * generation the caller has (0: none). Reply: a struct
 * volumes_mounts_rep (all of it when the status is OK) carrying `count`
 * handles: each mount's `fs` channel (abi/idl/fs.idl), in the order of
 * mounts[]. They are duplicates of volumes' client end, to pass on as
 * they are.
 *
 * volumes answers at once when its generation differs from `known`;
 * otherwise as soon as it changes, or with ERR_TIMED_OUT after
 * VOLUMES_MOUNTS_WAIT, so a caller waits in a loop and a call never
 * outlives its caller by more than that. Give each call a deadline past
 * VOLUMES_MOUNTS_WAIT (volumes_mounts() adds a second): a call that gives
 * up earlier leaves its answer, handles included, queued on the channel.
 * ERR_NO_RESOURCES: too many calls are waiting already.
 *
 * The generation changes whenever the list does: a mount appears, its
 * disk goes away (unplugged, or its usb-storage died), or it is remounted
 * (REMOUNT, ESP_WRITE: a new channel, calls on the old one fail
 * ERR_PEER_CLOSED). A fat service started again after a death is not a
 * change: volumes keeps its channel while no instance runs (the mount
 * stays listed, and calls on it wait for the next instance), and the next
 * instance carries on with the views and files the dead one made. /esp
 * and /data are the disk Jam OS booted from: partition 1 of type 0xEF
 * holding boot/jamos.elf, partition 2 of type 0x0C. Any other disk's FAT
 * partitions (MBR types 01 04 06 0B 0C 0E EF, or a disk with no table that
 * is one FAT volume) take the lowest free /usbN when they are found and
 * give it back when the stick goes; a partition with no FAT volume is
 * never listed, and never written: only the boot disk's blank data
 * partition is ever formatted. A mount that isn't listed has no service.
 * A volumes that init started again starts its generations somewhere else
 * (from the clock), so the old one's are never mistaken for its own;
 * asking a new volumes with known 0 is still the simple rule. */
#define VOLUMES_MOUNTS      0x0003000au
#define VOLUMES_MAX_MOUNTS  8u
#define VOLUMES_MOUNTS_WAIT (2 * NS_PER_S)
struct volumes_mount {
    char path[16];   /* "/data", "/esp": NUL-terminated */
};
struct volumes_mounts_rep {
    uint32_t txid;             /* the request's */
    int32_t  status;           /* OK: the rest follows */
    uint32_t generation;       /* never 0 */
    uint32_t count;            /* mounts, and handles with the reply */
    struct volumes_mount mounts[VOLUMES_MAX_MOUNTS];
} __attribute__((packed));
/* (1 handle: a client end of a `storage` channel, abi/idl/storage.idl) ->
 * u32 id. A software disk for tests, which volumes treats like a
 * usb-storage disk: the same questions, the same boot-disk check, the same
 * supervision of its filesystem services (bin/fat). One difference keeps
 * it apart from the real one: its mounts are /data-test and /esp-test, and
 * "the boot disk" is counted among test disks only, so a test never takes
 * /data away. It is gone once the channel's server end is closed.
 * ERR_NO_RESOURCES: too many disks. */
#define VOLUMES_TEST_DISK   0x0003000bu
/* (VOLUMES_USB_MOUNT, N, flags) -> (): the mount /usbN read-write (flags
 * VOLUMES_REMOUNT_WRITE) or read-only again (0); with VOLUMES_REMOUNT_TEST
 * a test disk's /usbN-test. Its fat service is synced and stopped, and
 * started again on a new `block` channel opened the new way (read-only:
 * usb-storage refuses every write on it), so the mount's channel is a new
 * one and files open on the old one fail ERR_PEER_CLOSED; it is listed
 * again once its volume is mounted (a new generation). Nothing is ever
 * formatted, in either mode. OK at once if it is that way already.
 * ERR_NOT_FOUND: no such mount (only /usbN can be asked for: /esp and
 * /data are what they are); ERR_BAD_STATE: its service isn't serving. */
#define VOLUMES_REMOUNT      0x0003000du
#define VOLUMES_USB_MOUNT    0xfffcu
#define VOLUMES_REMOUNT_WRITE 1u
#define VOLUMES_REMOUNT_TEST  2u
/* () -> (), control channel only: stop, as when the last control client
 * leaves (every writable mount synced, the warm spare and every
 * filesystem service stopped), and exit. The answer comes first; the
 * caller waits for volumes' process to end. For a reboot, before devmgr's
 * own DEVMGR_SHUTDOWN, while the shell still holds copies of the
 * mounts. */
#define VOLUMES_SHUTDOWN     0x0003000eu
/* (vendor, device 0; instance flags) -> with VOLUMES_ESP_WRITABLE, 1
 * handle: the boot disk's ESP made writable for init's stick write
 * (`update -w`, init's update.c). Asked on the ESP channel only; on any
 * other channel, the control channel included, it is ERR_ACCESS_DENIED,
 * and the ESP channel may ask nothing else. VOLUMES_ESP_WRITABLE: the
 * ESP's filesystem service stops in order and starts again on a `block`
 * channel opened read-write; the handle is a channel to it, the caller's
 * alone: /esp leaves the mounts while it is writable, so no namespace ever
 * gets it. Flags 0: it stops in order (everything on the stick, the
 * volume clean) and starts read-only again, and /esp is a mount again (a
 * new generation). OK at once if it is that way already (writable:
 * another handle to the same channel). A crash meanwhile restarts it the
 * way it was; a pulled stick ends it (the stick's next mount is
 * read-only). ERR_NOT_FOUND: no boot disk with an ESP service;
 * ERR_BAD_STATE: it isn't serving; ERR_INVALID_ARGS: another flag, or a
 * device named. */
#define VOLUMES_ESP_WRITE    0x00030010u
#define VOLUMES_ESP_WRITABLE 1u
/* (1 handle: the server end of a new DISKS channel) -> (), control
 * channel only: devmgr was started again, and this is the channel the new
 * one tells its disks on (init made the pair, and gave devmgr the other
 * end: DEVMGR_SET_VOLUMES). The old channel's disks are forgotten, without
 * notices. */
#define VOLUMES_SET_DISKS    0x00030012u

#define VOLUMES_SUP_NONE       0u   /* never started (yet) */
#define VOLUMES_SUP_RUNNING    1u
#define VOLUMES_SUP_RESTARTING 2u   /* died; its restart is due */
#define VOLUMES_SUP_GAVE_UP    3u   /* died too often: no more restarts */
#define VOLUMES_SUP_FINISHED   4u   /* exited 0 by itself */

/* volumes' startup roles: the server ends of its control, ESP and DISKS
 * channels, and a duplicate of /svc/notify's shared channel (none without
 * a compositor), for the desktop's notices of sticks: "USB stick added",
 * "USB stick removed", "Jam OS stick removed" and "... is back"
 * (<notice.h>). Its fats' jobs are children of its own (SR_JOB). */
#define VOLUMES_SR_CTL    (SR_USER + 0)
#define VOLUMES_SR_ESP    (SR_USER + 1)
#define VOLUMES_SR_DISKS  (SR_USER + 2)
#define VOLUMES_SR_NOTIFY (SR_USER + 3)

/* ---- the DISKS channel: devmgr to volumes ------------------------------------------
 * init makes a new pair whenever devmgr or volumes starts and hands one
 * end to each (devmgr: DEVMGR_SET_VOLUMES; volumes: VOLUMES_SR_DISKS, or
 * SET_DISKS). Messages are struct disks_msg:
 *   ATTACH (txid 0, 1 handle): disk `id` is there, its driver runs; the
 *     handle is a `storage` channel of volumes' own (devmgr asked the
 *     driver's DR_SERVE for it: svc.connect). `id` names the disk while
 *     the device is there (usb-bus's device id; `bus` DISKS_BUS_USB); a
 *     driver started again after a crash is announced again with the same
 *     id. ERR_PEER_CLOSED on the `storage` channel alone means the driver
 *     ended, not that the device went;
 *   GONE (txid 0): the device is gone (unplugged, or its bus driver
 *     ended): the disk will not come back under this id;
 *   SYNC (a call, txid not 0): every writable mount synced, before devmgr
 *     stops a disk's driver (DEVMGR_RELEASE); answered with a struct
 *     volumes_rep's header once they all have (or failed).
 * Reconnect: when the channel's peer closes, the other side drops it and
 * waits for the next: devmgr announces every disk again on a new channel;
 * volumes forgets every disk the old one announced, without notices. */
#define DISKS_ATTACH  0x00030013u
#define DISKS_GONE    0x00030014u
#define DISKS_SYNC    0x00030015u
#define DISKS_BUS_USB 1u
#define DISKS_SYNC_WAIT (30 * NS_PER_S)   /* a SYNC: every writable mount's, 5 s each */
struct disks_msg {
    uint32_t txid;      /* 0 but for SYNC */
    uint32_t ordinal;   /* DISKS_* */
    uint32_t id;        /* the disk */
    uint8_t  bus;       /* ATTACH: DISKS_BUS_USB */
} __attribute__((packed));

struct volumes_req {
    uint32_t txid;             /* stamped by channel_call */
    uint32_t ordinal;          /* VOLUMES_* */
    uint16_t vendor, device;   /* a filesystem service's name (above) */
    uint32_t instance;
} __attribute__((packed));

struct volumes_rep {
    uint32_t txid;             /* the request's */
    int32_t  status;           /* OK: the results follow */
    uint32_t a, b, c;          /* TEST_DISK: id; SUPERVISION: state, restarts, backoff_ms */
    uint32_t d, e;             /* SUPERVISION: restarts that promoted the spare, a spare waits */
} __attribute__((packed));

#define VOLUMES_REP_HDR     8u
#define VOLUMES_MAX_HANDLES 8u

/* One call. *nh (may be NULL) gets how many handles came back into hs
 * (hcap of them at most). Results: rep->a.. when the status is OK. */
static inline status_t volumes_call(handle_t ch, uint32_t ordinal, uint16_t vendor,
                                    uint16_t device, uint32_t instance, struct volumes_rep *rep,
                                    handle_t *hs, uint32_t hcap, uint32_t *nh,
                                    uint64_t deadline_ns)
{
    struct volumes_req q = { 0, ordinal, vendor, device, instance };
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
    if (n < VOLUMES_REP_HDR || rep->status > 0)
        return ERR_INTERNAL;
    return rep->status;
}

/* One call that hands volumes a handle (TEST_DISK, SET_DISKS): h goes
 * with the request whatever happens. */
static inline status_t volumes_call_h(handle_t ch, uint32_t ordinal, handle_t h,
                                      struct volumes_rep *rep, uint64_t deadline_ns)
{
    struct volumes_req q = { 0, ordinal, 0, 0, 0 };
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = ch, .wr_len = sizeof(q), .wr_bytes = (uint64_t)(uintptr_t)&q,
        .wr_handles = (uint64_t)(uintptr_t)&h, .wr_nhandles = 1, .rd_cap = sizeof(*rep),
        .rd_bytes = (uint64_t)(uintptr_t)rep, .rd_actual = (uint64_t)(uintptr_t)&n,
        .rd_hactual = (uint64_t)(uintptr_t)&got, .deadline_ns = deadline_ns,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (n < VOLUMES_REP_HDR || rep->status > 0)
        return ERR_INTERNAL;
    return rep->status;
}

/* One VOLUMES_MOUNTS call on the control channel ch. OK: *out holds the
 * generation and the mounts, and hs[i] (VOLUMES_MAX_MOUNTS slots) is
 * out->mounts[i]'s `fs` channel, the caller's to close. ERR_TIMED_OUT:
 * nothing changed from `known` within VOLUMES_MOUNTS_WAIT; ask again.
 * Blocks for VOLUMES_MOUNTS_WAIT plus a second at most. */
static inline status_t volumes_mounts(handle_t ch, uint32_t known, struct volumes_mounts_rep *out,
                                      handle_t *hs)
{
    struct volumes_req q = { 0, VOLUMES_MOUNTS, 0, 0, known };
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = ch,
        .wr_len = sizeof(q),
        .wr_bytes = (uint64_t)(uintptr_t)&q,
        .rd_cap = sizeof(*out),
        .rd_bytes = (uint64_t)(uintptr_t)out,
        .rd_actual = (uint64_t)(uintptr_t)&n,
        .rd_handles = (uint64_t)(uintptr_t)hs,
        .rd_hcap = VOLUMES_MAX_MOUNTS,
        .rd_hactual = (uint64_t)(uintptr_t)&got,
        .deadline_ns = now() + VOLUMES_MOUNTS_WAIT + NS_PER_S,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (n >= VOLUMES_REP_HDR && out->status < 0 && !got)
        return out->status;
    if (n != sizeof(*out) || out->status != OK || out->count != got)
        st = ERR_INTERNAL;   /* not volumes' format: nothing of it is used */
    for (uint32_t i = 0; st != OK && i < got; i++)
        jam_handle_close(hs[i]);
    return st;
}
