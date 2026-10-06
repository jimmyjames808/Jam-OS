/* devmgr's own pieces: main.c (startup, the match table, the event
 * loop), chans.c (its channels: reading requests, what each channel may
 * ask), request.c (the answers), bind.c (starting and stopping a driver: its handles, its job),
 * supervise.c (what happens when a driver dies: restart with backoff, or
 * give up), usb.c (the USB interfaces usb-bus reports), disk.c (the disks
 * usb-storage serves: which one is the boot disk) and fsvc.c (their
 * filesystem services; disk.h between the two), mounts.c (DEVMGR_MOUNTS: the list of mounts, its generation
 * and the calls waiting for it to change), spare.c (what outlives a
 * filesystem service's process: its keeper and state VMO, and the warm
 * spare fat). The protocol is in <devmgr.h>. */
#pragma once

#include <devmgr.h>
#include <jam/driver.h>
#include <notice.h>
#include <os.h>

#define MAX_DEVS  128   /* PCI functions, the crash-test driver, USB class drivers,
                         * filesystem services */
/* How long a driver gets to end by itself when asked to stop, before its
 * job is killed. usb-bus takes longest: a Disable Slot per device (1 s
 * timeout each), then its final halt and reset (about 3 s of bounded
 * waits). */
#define STOP_WAIT (15 * NS_PER_S)

/* Supervision (supervise.c). */
#define SUP_BACKOFF_FIRST (100 * NS_PER_MS)
#define SUP_BACKOFF_MAX   (5 * NS_PER_S)
#define SUP_WINDOW        (60 * NS_PER_S)
#define SUP_RESTART_LIMIT 5   /* restarts within SUP_WINDOW; the next death gives up */

/* Port keys: a driver process's SIG_TERMINATED (binding index and start
 * generation, so a stale packet is recognised). */
#define KEY_DRIVER         (1ull << 32)
#define KEY_OF(i, gen)     (KEY_DRIVER | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A driver wrote on its DR_SERVE channel by itself (usb-bus:
 * usbbus.interface_attached): binding index and start generation. */
#define KEY_EVENTS         (1ull << 33)
#define KEY_EV_OF(i, gen)  (KEY_EVENTS | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A kept USB interface channel closed (the device went): usb_ifs slot and
 * its generation. */
#define KEY_USBIF          (1ull << 34)
#define KEY_IF_OF(i, gen)  (KEY_USBIF | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A test disk's `storage` channel has something to read, or closed: disks
 * slot and its generation. */
#define KEY_DISK           (1ull << 35)
#define KEY_DISK_OF(i, gen) (KEY_DISK | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* One of devmgr's channels has something to read, or closed (chans.c):
 * its slot and the slot's generation. */
#define KEY_CHAN           (1ull << 36)
#define KEY_CHAN_OF(i, gen) (KEY_CHAN | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* The warm spare fat's process terminated (spare.c). */
#define KEY_SPARE          (1ull << 37)
/* A filesystem service's keep channel has something to read (spare.c):
 * binding index and the keeper's generation. */
#define KEY_KEEP           (1ull << 38)
#define KEY_KEEP_OF(i, gen) (KEY_KEEP | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
#define KEY_INDEX(k)       ((uint32_t)((k) >> 16) & 0xffffu)
#define KEY_GEN(k)         ((uint32_t)(k) & 0xffffu)

/* What a channel of devmgr's may ask (chans.c): the query channel the
 * queries, but never a device that has a device channel; a device channel
 * the queries about its own device alone; the control channel everything
 * but ESP_WRITE; init's ESP channel ESP_WRITE alone (<devmgr.h> "Trust"). */
enum level {
    LEVEL_QUERY,
    LEVEL_DEVICE,
    LEVEL_CONTROL,
    LEVEL_ESP,
};

/* Which channel a request came on, for request.c. */
#define NO_DEVICE UINT32_MAX
struct request_from {
    enum level lv;
    uint32_t   dev;   /* LEVEL_DEVICE: devs index of the channel's device; else NO_DEVICE */
};

enum bind_kind {
    BIND_PCI,    /* a PCI function (pci_enum) */
    BIND_SOFT,   /* no hardware: the crash-test driver */
    BIND_USB,    /* a USB interface usb-bus reported (usb.c); path NULL: a free slot */
    BIND_FS,     /* a filesystem service on one partition of a disk (disk.c); path NULL: a
                  * free slot */
};

struct binding {
    enum bind_kind      kind;       /* what is bound */
    uint32_t            index;      /* BIND_PCI: pci_enum's */
    struct pci_dev_info info;       /* BIND_PCI: pci_enum's; BIND_SOFT, BIND_USB: vendor/device
                                     * only (BIND_USB: the USB ids) */
    const char         *path;       /* the driver; NULL: none for it */
    bool                test;       /* its deaths and giving up are expected (not problems) */
    handle_t            dev;        /* BIND_PCI: ours, with RIGHT_MANAGE (0 until started once) */
    /* The driver while it runs. */
    handle_t            job, proc;  /* its job and process, 0 while none runs */
    handle_t            client;     /* our end of its DR_SERVE channel (GET_SERVICE's) */
    uint64_t            client_key; /* the port watches client for events under it (0: not) */
    handle_t            serve;      /* the driver's end, kept while its restart is due */
    uint32_t            gen;        /* bumped at every start: in its port key */
    bool                killed;     /* ended by DEVMGR_KILL */
    status_t            last;       /* the last start's status */
    /* Supervision. */
    uint32_t            state;      /* DEVMGR_SUP_* */
    uint64_t            restart_at; /* DEVMGR_SUP_RESTARTING: when */
    uint64_t            restarted[SUP_RESTART_LIMIT];   /* when the last restarts were (ring) */
    uint32_t            restarts;   /* since boot */
    uint32_t            backoff_ms; /* the last backoff */
    /* BIND_USB (usb.c). */
    int32_t             usb_if;     /* its usb_ifs slot; -1: the interface is gone */
    uint32_t            usb_id;     /* usb-bus's device id */
    uint8_t             usb_ifnum;  /* the interface number */
    bool                console_wait; /* its restart waits for a (new) console */
    uint32_t            input_gen;  /* its current run got DR_INPUT from console number
                                     * input_gen (0: none) */
    char                name[32];   /* its process name: "hid-<path>:<if>"; BIND_FS:
                                     * "fat-data" */
    /* A disk's driver (a BIND_USB running STORAGE_DRIVER) and BIND_FS (disk.c). */
    uint32_t            disk;       /* its disks slot + 1; 0: none */
    uint8_t             part;       /* BIND_FS: the partition it serves (storage.idl's index) */
    handle_t            ctl;        /* BIND_FS: our end of its `fsctl` channel (0: none) */
    /* BIND_FS on a disk that isn't the boot disk (disk.c): a /usbN mount. */
    bool                other;      /* it is one: never given FAT_ARG_FORMAT */
    bool                rw;         /* its `block` channel is opened read-write (`mount -w`;
                                     * the boot disk's ESP: init's ESP_WRITE) */
    bool                ready;      /* it answered its first fs.stat: a mount */
    uint8_t             usbn;       /* the N of /usbN */
    uint32_t            probe;      /* that fs.stat's transaction id */
    /* BIND_FS: what outlives its process (spare.c). Its `fs` channel is
     * kept too: client and serve stay devmgr's from one instance to the
     * next (fs_serve_end), so a restart is not a new channel. */
    struct fs_kept     *kept;       /* its keeper and state VMO; NULL until its first start */
    bool                deliberate; /* the restart due is a deliberate kill's: not counted */
    uint64_t            kill_at;    /* when DEVMGR_KILL asked for its end (uptime ns; 0: none) */
    uint64_t            ended_at;   /* when its last instance's end was seen (uptime ns) */
    uint32_t            promoted;   /* restarts that promoted the warm spare, since boot */
    /* Bumped with every new `fs` channel (fs_serve_end): the mount's
     * generation in DEVMGR_MOUNTS. A restart on the kept channel doesn't
     * move it: fat keeps its views (and open files) across its deaths. */
    uint32_t            chan_gen;
};

extern struct binding devs[MAX_DEVS];
extern unsigned       ndevs;
extern handle_t       pci_res, port;
/* The desktop's notices (DEVMGR_SR_NOTIFY; none under `nocomp`). */
extern struct notice_box devmgr_notices;
/* Problems for the exit code: real drivers that crashed or were given up
 * on, and drivers that didn't end cleanly. */
extern unsigned       problems;
/* The argument "hidboot": passed on to every hid (mice stay in the boot
 * protocol). */
extern bool           hidboot;
/* The argument "vtdtest" (with "iommu=on", the IOMMU checks test entry):
 * passed on to drv/hda, which runs its deliberate DMA faults before
 * serving (M11 stage 5). */
extern bool           vtdtest;
/* The argument "bootdisk=0x<id>": the MBR disk id of the disk the machine
 * booted from (the loader's, passed on by the kernel and init); 0: not
 * known. disk.c takes the Jam OS disk with that id as the boot disk. */
extern uint32_t       boot_mbr_id;
/* The argument "vlan=<id>" or "vlan=none" (the kernel's, through init;
 * <jam/netdev.h> netdev_vlan_args): the network's mode, a VLAN or
 * NETFRAME_MODE_UNTAGGED, passed on as the same word (netdev_mode_word)
 * to the driver of every network card (PCI class 02, PCI_CLASS_NETWORK).
 * 0: none given, or not valid: the drivers start without it and keep the
 * network off. */
extern uint16_t       net_vlan;
#define PCI_CLASS_NETWORK 0x02u
extern uint64_t       devmgr_started;   /* when devmgr started (uptime, ns) */
/* The word a PCI driver at `path` is started with after its name, when
 * its match-table row asks for one and devmgr was given it ("netprobe" or
 * "netsend" or "net" for drv/rtl8125), else NULL. */
const char *pci_driver_arg(const char *path);
/* The first binding pass's counts, for DEVMGR_STATUS. */
extern unsigned       nbound, nfailed, nskipped;
/* DEVMGR_SHUTDOWN was answered: the loop stops as if every client had left. */
extern bool           shutdown_asked;

void say(bool report_it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *bdf(const struct binding *b);   /* "00:04.0", "usb 6.1:0", "fat-data", "test" */
bool in_bootfs(const char *path);

/* chans.c. Take the channels init gave (the startup message); false if
 * there is none to live by (no control or query channel). */
bool     chans_init(void);
/* Answer what is queued on every channel: ERR_SHOULD_WAIT once all are
 * drained, ERR_PEER_CLOSED once the clients of the channel devmgr lives by
 * are all gone, else a read's failure. */
status_t chans_serve(void);
/* Watch every channel that isn't watched (ONCE): ERR_SHOULD_WAIT when all
 * are, else a bind's failure. */
status_t chans_arm(void);
/* A port packet: true if it was a channel's (watched again by the next
 * chans_arm, read by the next chans_serve). */
bool     chans_packet(uint64_t key);
/* Does device `dev` (a devs index) have a device channel? Then the query
 * channel never hands out its service. */
bool     chans_device_owned(uint32_t dev);
/* DEVMGR_DEVICE_CHANNEL: a new channel scoped to device `dev`; *out: the
 * client end. ERR_BAD_STATE: it has one already (until that one's clients
 * are all gone); ERR_NO_RESOURCES: no free slot. */
status_t chans_new_device(uint32_t dev, handle_t *out);

/* request.c. Answer one request that came on the channel `from` says: the
 * reply, and *nh handles in hs, each to be sent with rs[i]. */
void request_handle(const struct devmgr_req *q, const struct request_from *from,
                    struct devmgr_rep *r, handle_t *hs, rights_t *rs, uint32_t *nh);

/* bind.c. Start b's driver (state RUNNING on success): its handles from
 * scratch (for a PCI function: woken to D0, a new dma_cap, interrupt
 * object and BARs), DR_SERVE on b->serve if a restart kept one, else on a
 * new channel whose other end becomes b->client. */
status_t start_driver(struct binding *b);
/* Kill b's driver's whole job (anything it started goes too) and turn Bus
 * Master Enable off through our own handle. Returns once it is dead. */
void kill_driver(struct binding *b);
/* Let go of a dead driver's process and job (and its port binding). */
void forget_driver(struct binding *b);
/* Stop a running driver: close its channel, wait for it to return (kill
 * it if it doesn't, or at once if `kill`), forget it. True if it ended
 * with exit 0 by itself (or `excused`) and its job is empty (checked only
 * then: a killed driver's DMA pages may still be quarantined). */
bool stop_driver(struct binding *b, bool kill, bool excused);
/* Is the job of a driver that has ended empty? One that isn't yet is
 * looked at again for a while (job_run_due): true for now, and if it is
 * still not empty then it is logged as not ended cleanly and counted in
 * `problems`. False only if it can't be looked at again. */
bool job_empty(handle_t job, const char *who);
void job_run_due(void);
uint64_t job_next_deadline(void);   /* DEADLINE_NEVER: nothing to look at again */
/* Wait (bounded) until every job being looked at again is decided; false
 * if any was not empty in the end. */
bool job_settle(void);
/* DEVMGR_DRIVER_VIEW: the function and each memory BAR as a driver gets
 * them (hs[i] to be sent with rs[i]); *mask: which BARs. */
status_t driver_view(struct binding *b, handle_t *hs, rights_t *rs, uint32_t *nh, uint32_t *mask);
unsigned mem_bars(const struct binding *b);

/* Close b's client end (and stop watching it). */
void close_client(struct binding *b);
/* The next message on h didn't fit (ERR_BUFFER_TOO_SMALL: n bytes, nh
 * handles): take it off the queue anyway, unanswered, and close its
 * handles. (usb.c) */
void discard(handle_t h, uint32_t n, uint32_t nh);

/* usb.c: the interfaces usb-bus reports on its DR_SERVE channel, and
 * their class drivers. */
#define MAX_USB_IFS 64
/* b's driver wrote on its channel by itself (KEY_EVENTS). */
void usb_driver_events(struct binding *b);
/* A kept interface channel's KEY_USBIF packet. */
void usb_if_closed(uint64_t key);
/* b's driver (a usb-bus) is gone: forget the interfaces it reported (their
 * class drivers see their channels close and end by themselves). */
void usb_bus_gone(struct binding *b);
/* A BIND_USB binding's handles for its driver: DR_USB (a duplicate of the
 * kept channel) and, with a console, DR_INPUT. ERR_PEER_CLOSED: the
 * interface is gone; ERR_SHOULD_WAIT: the console is restarting. */
status_t usb_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* The word a BIND_USB binding's driver is started with after its name:
 * "hidboot" for drv/hid when devmgr was given it, else NULL. */
const char *usb_driver_arg(const struct binding *b);
/* DEVMGR_RELEASE: a duplicate of the interface channel b's driver had (the
 * one channel usb-bus lets open the interface's bulk endpoints), for a
 * test that drives the interface itself. ERR_PEER_CLOSED: it is gone. */
status_t usb_channel(const struct binding *b, handle_t *out);
/* b is a BIND_USB binding that won't run again: free its slot. */
void usb_retire(struct binding *b, const char *why);
/* Is b's interface gone? */
bool usb_gone(const struct binding *b);
/* The console to connect class drivers to: devmgr's client end of it,
 * from SR_CONSOLE or DEVMGR_SET_CONSOLE (consumed). A new one restarts the
 * class drivers waiting for it. With the argument "comp" (init's, when
 * the compositor draws the screen) it is an INPUT-level compctl channel
 * instead (abi/idl/compctl.idl): the keys and the mouse go to the
 * compositor; "the console" in this file's names then means it. */
void usb_new_console(handle_t ch);
/* What the class drivers type into, for the log: "console" or "compositor". */
const char *usb_hub_name(void);
/* The argument "comp" (main.c). */
extern bool comp_input;
/* Could b's driver have ended because the console went away? */
bool usb_console_gone(const struct binding *b);

/* disk.c: the disks usb-storage serves (and DEVMGR_TEST_DISK's), and the
 * filesystem services of the boot disk. */
#define STORAGE_DRIVER "drv/usb-storage"
#define PART_ESP  DEVMGR_PART_ESP    /* the boot disk's ESP, read-only, at /esp */
#define PART_DATA DEVMGR_PART_DATA   /* the boot disk's data partition, at /data */
/* b is a BIND_USB binding about to run STORAGE_DRIVER for usb-bus device
 * `id`: give it a disk (b->disk). False: too many disks. */
bool disk_attach(struct binding *b, uint32_t id);
/* b (a disk's driver) won't run again: forget its disk. */
void disk_detach(struct binding *b);
/* b's process started (bind.c): a disk's driver is asked what it holds; a
 * filesystem service is a mount from now on. */
void disk_started(struct binding *b);
/* b's process is gone (bind.c): a disk's driver takes the disk's mounts
 * with it; a filesystem service is no mount until it is back. */
void disk_stopped(struct binding *b);
/* b (a disk's driver, or a BIND_FS) has something to read on its channel:
 * the answers to what devmgr asked without waiting. */
void disk_events(struct binding *b);
/* A test disk's KEY_DISK packet. */
void disk_key(uint64_t key);
/* DEVMGR_TEST_DISK: ch (consumed) is a `storage` channel; *id: the disk's. */
status_t disk_test(handle_t ch, uint32_t *id);
/* b (BIND_FS) died, its process still held. True if that is the end of it
 * (b freed, no restart): the ESP's service ended before it answered the
 * boot-disk check, so the disk is not the boot disk; or a /usbN service
 * ended by itself with FAT_EXIT_NO_VOLUME (`no_volume`): the partition
 * holds no FAT volume and is left alone. */
bool fs_check_ended(struct binding *b, bool no_volume);
/* FAT_ARG_FORMAT for the one service that may format a blank partition
 * (the boot disk's data partition), NULL for every other. */
const char *fs_format_arg(const struct binding *b);
/* For an instance that replaces one that ended: FAT_ARG_KILLED after a
 * deliberate kill, FAT_ARG_CRASHED after any other end; NULL for a first
 * start (fat counts only crashes against the request in progress). */
const char *fs_end_arg(const struct binding *b);
/* DEVMGR_REMOUNT: /usbN (a test disk's: /usbN-test) read-write or
 * read-only. ERR_NOT_FOUND: no such mount; ERR_BAD_STATE: its service
 * isn't serving. */
status_t disk_remount(unsigned n, bool test, bool writable);
/* DEVMGR_ESP_WRITE: the boot disk's ESP read-write (*out: a channel to its
 * service, which is no mount meanwhile) or read-only again (out unused).
 * ERR_NOT_FOUND: no boot disk with an ESP service; ERR_BAD_STATE: it
 * isn't serving. */
status_t disk_esp_write(bool writable, handle_t *out);
/* A BIND_FS binding's handle for its service: FAT_SR_BLOCK, a new `block`
 * channel on its partition. ERR_PEER_CLOSED: the disk is gone. */
status_t fs_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* Close b's `fsctl` channel, if it has one. */
void     fs_ctl_close(struct binding *b);
/* The mount point b (BIND_FS) serves: "/data", "/esp-test", "/usb0". The
 * string is good until the next call. */
const char *fs_mount_path(const struct binding *b);
/* b (BIND_FS) won't run again: free its slot. */
void fs_retire(struct binding *b);
/* DEVMGR_FS_SVC: the filesystem service on partition `part` of disk `id`,
 * or NULL. */
struct binding *fs_find(uint32_t id, uint32_t part);
/* DEVMGR_FS_MOUNT: the filesystem service of mount `which`
 * (DEVMGR_MOUNT_*; flags DEVMGR_MOUNT_TEST: a test disk's), or NULL. */
struct binding *fs_find_mount(uint32_t which, uint32_t flags);
/* b's (BIND_FS) DR_SERVE: a duplicate of the `fs` server end devmgr keeps
 * across its restarts (a new channel only when there is none, or its
 * client end was closed). */
status_t fs_serve_end(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* Does b (BIND_FS) keep its `fs` channel while no instance runs? Then it
 * stays a mount meanwhile, and calls on it wait for the next instance. */
bool     fs_channel_kept(const struct binding *b);
/* devmgr is stopping: fs.sync each mounted data partition while its disk
 * still works (the services are killed when their disk's driver goes). */
void disk_sync_all(void);
/* Give up on what did not answer in time, and start the next /usbN
 * service that waits its turn. Called after every event. */
void disk_run_due(void);
/* When disk_run_due has something to do next, or DEADLINE_NEVER. */
uint64_t disk_next_deadline(void);

/* A mount: a running filesystem service of the boot disk, or of another
 * disk once its volume is mounted. */
struct mount {
    char     path[16];   /* the mount point */
    uint32_t bind;       /* devs index of its filesystem service */
    uint32_t gen;        /* that binding's `fs` channel's generation (chan_gen): a new
                          * channel is a new mount, a restart on the kept one isn't */
};
/* The mounts as they are now into out (DEVMGR_MAX_MOUNTS slots). Returns
 * how many. */
unsigned disk_mounts(struct mount *out);

/* mounts.c: DEVMGR_MOUNTS. */
/* Something that may have changed the mounts happened: if the list differs
 * from the one last handed out, the generation moves on and every waiting
 * call is answered. */
void mounts_update(void);
/* A DEVMGR_MOUNTS request (txid, known generation) that came on ch:
 * answered now if the generation differs, else kept waiting. */
void mounts_request(handle_t ch, uint32_t txid, uint32_t known);
/* Answer the calls that waited DEVMGR_MOUNTS_WAIT (ERR_TIMED_OUT). */
void mounts_run_due(void);
/* When mounts_run_due has something to do next, or DEADLINE_NEVER. */
uint64_t mounts_next_deadline(void);

/* supervise.c. */
/* b's driver process (start generation `gen`) terminated. */
void sup_died(struct binding *b, uint32_t gen);
/* Start every restart that is due. */
void sup_run_due(void);
/* The earliest due restart, or DEADLINE_NEVER. */
uint64_t sup_next_deadline(void);
/* Forget b's supervision: no restart is due any more, the kept channel
 * end goes; the restart history is cleared. */
void sup_reset(struct binding *b);

/* spare.c: a filesystem service that outlives its process
 * (docs/M11.6-PLAN.md): per BIND_FS binding a keeper and a state VMO, and
 * one warm spare fat for every mount. */
/* `on`: keep a warm spare (no boot word `nospare`). */
void     spare_init(bool on);
/* Start b (BIND_FS): for a `restart` after a death, the warm spare if one
 * waits, else (and for any other start) a new process (start_driver);
 * either way with its state and a new keep channel, whose keeper then
 * hands it what was kept. A restart is measured: the log says when it ran
 * and when it first answered. Errors as start_driver's. */
status_t fs_run(struct binding *b, bool restart);
/* b's state VMO (made the first time) and a new keep channel into x
 * (SR_STATE, SR_KEEP), for its next instance. One that can't be made is
 * said and left out: that instance starts fresh. */
void     fs_kept_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* The `block` channel opened in advance for b (the boot disk's /data)
 * into *out, if there is one and its disk still answers: true. */
bool     fs_block_prepared(const struct binding *b, handle_t *out);
/* b won't run again as it was (retired, given up on, stopped in order):
 * what its keeper kept closes (its clients see ERR_PEER_CLOSED), its state
 * VMO goes, and a `block` channel opened in advance for it. */
void     fs_kept_release(struct binding *b);
/* Start the warm spare (and open /data's `block` channel for it) when due. */
void     spare_due(void);
uint64_t spare_next_deadline(void);   /* DEADLINE_NEVER: nothing due */
/* The port's packets: KEY_SPARE (the spare ended), KEY_KEEP (a keep channel). */
void     spare_event(void);
void     spare_keep_event(uint64_t key);
/* SUPERVISION: is a warm spare waiting now? */
bool     spare_waits(void);
/* devmgr is stopping: the spare goes, and the `block` channel it had. */
void     spare_stop(void);
