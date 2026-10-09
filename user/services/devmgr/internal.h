/* devmgr's own pieces: main.c (startup, the match table, the event
 * loop), chans.c (its channels: reading requests, what each channel may
 * ask), request.c (the answers), bind.c (starting and stopping a driver: its handles, its job),
 * supervise.c (what happens when a driver dies: restart with backoff, or
 * give up), usb.c (the USB interfaces usb-bus reports), disks.c (the
 * disks usb-storage serves, told to volumes on the DISKS channel). The
 * protocol is in <devmgr.h>. The disks' filesystems and mounts are
 * volumes' (user/services/volumes/), since M12's storage split. */
#pragma once

#include <devmgr.h>
#include <jam/driver.h>
#include <notice.h>
#include <os.h>
#include <supervise.h>

#define MAX_DEVS  128   /* PCI functions, the crash-test driver, USB class drivers */
/* How long a driver gets to end by itself when asked to stop, before its
 * job is killed. usb-bus takes longest: a Disable Slot per device (1 s
 * timeout each), then its final halt and reset (about 3 s of bounded
 * waits). */
#define STOP_WAIT (15 * NS_PER_S)

/* Port keys: a driver process's SIG_TERMINATED (binding index and start
 * generation, so a stale packet is recognised). */
#define KEY_DRIVER         (1ull << 32)
#define KEY_OF(i, gen)     (KEY_DRIVER | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A driver wrote on its DR_SERVE channel by itself (a disk's driver, a
 * filesystem service: answers, disk.c): binding index and start generation. */
#define KEY_EVENTS         (1ull << 33)
#define KEY_EV_OF(i, gen)  (KEY_EVENTS | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A kept USB interface channel closed (the device went): usb_ifs slot and
 * its generation. */
#define KEY_USBIF          (1ull << 34)
#define KEY_IF_OF(i, gen)  (KEY_USBIF | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* One of devmgr's channels has something to read, or closed (chans.c):
 * its slot and the slot's generation. */
#define KEY_CHAN           (1ull << 36)
#define KEY_CHAN_OF(i, gen) (KEY_CHAN | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A usb-bus's events channel (usb.c, usbbus_events) has something to read,
 * or closed: its slot and the slot's generation. */
#define KEY_BUSEV          (1ull << 39)
#define KEY_BUSEV_OF(i, gen) (KEY_BUSEV | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
#define KEY_INDEX(k)       ((uint32_t)((k) >> 16) & 0xffffu)
#define KEY_GEN(k)         ((uint32_t)(k) & 0xffffu)

/* What a channel of devmgr's may ask (chans.c): the query channel the
 * queries, but never a device that has a device channel; a device channel
 * the queries about its own device alone; the control channel everything
 * (<devmgr.h> "Trust"). */
enum level {
    LEVEL_QUERY,
    LEVEL_DEVICE,
    LEVEL_CONTROL,
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
    struct sup_window   win;        /* the restart window (<supervise.h>): the last restarts,
                                     * those since boot, the last backoff */
    /* BIND_USB (usb.c). */
    int32_t             usb_if;     /* its usb_ifs slot; -1: the interface is gone */
    uint32_t            usb_id;     /* usb-bus's device id */
    uint8_t             usb_ifnum;  /* the interface number */
    bool                input_wait; /* its restart waits for a (new) input channel */
    uint32_t            input_gen;  /* its current run got DR_INPUT from input channel number
                                     * input_gen (0: none) */
    char                name[32];   /* its process name: "hid-<path>:<if>" */
    /* A disk's driver (a BIND_USB running STORAGE_DRIVER: disks.c). */
    bool                disk;       /* it is one: its channel is never handed out */
    uint32_t            disk_txid;  /* the svc.connect asked of it for volumes (0: none) */
};

extern struct binding devs[MAX_DEVS];
extern unsigned       ndevs;
extern handle_t       pci_res, port;
/* The desktop's notices (DEVMGR_SR_NOTIFY). */
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
const char *bdf(const struct binding *b);   /* "00:04.0", "usb 6.1:0", "test" */
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
/* DEVMGR_DRIVER_VIEW: the function and each memory BAR as a driver gets
 * them (hs[i] to be sent with rs[i]); *mask: which BARs. */
status_t driver_view(struct binding *b, handle_t *hs, rights_t *rs, uint32_t *nh, uint32_t *mask);
unsigned mem_bars(const struct binding *b);

/* Close b's client end (and stop watching it). */
void close_client(struct binding *b);
/* The next message on h didn't fit (ERR_BUFFER_TOO_SMALL: n bytes, nh
 * handles): take it off the queue anyway, unanswered, and close its
 * handles. (usb.c, disks.c) */
void discard(handle_t h, uint32_t n, uint32_t nh);

/* usb.c: the interfaces usb-bus reports on its events channel, and their
 * class drivers. */
#define MAX_USB_IFS 64
/* b (a usb-bus) is about to start: its events channel (DR_EVENTS), whose
 * other end devmgr serves. Nothing for any other driver. */
status_t usb_bus_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* A usb-bus's events channel's KEY_BUSEV packet. */
void usb_bus_events(uint64_t key);
/* A kept interface channel's KEY_USBIF packet. */
void usb_if_closed(uint64_t key);
/* b's driver (a usb-bus) is gone: forget the interfaces it reported (their
 * class drivers see their channels close and end by themselves). */
void usb_bus_gone(struct binding *b);
/* A BIND_USB binding's handles for its driver: DR_USB (a duplicate of the
 * kept channel) and, with an input channel, DR_INPUT. ERR_PEER_CLOSED: the
 * interface is gone; ERR_SHOULD_WAIT: the compositor is restarting. */
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
/* The compositor to connect class drivers to: an INPUT-level compctl
 * channel (abi/idl/compctl.idl), from SR_CONSOLE or devmgrctl.set_input
 * (consumed): the keys and the mouse go to the compositor. A new one
 * restarts the class drivers waiting for it. */
void usb_new_input(handle_t ch);
/* What the class drivers type into, for the log: "compositor". */
const char *usb_hub_name(void);
/* Could b's driver have ended because the compositor went away? */
bool usb_input_gone(const struct binding *b);

/* disks.c: the disks usb-storage serves, told to volumes (the DISKS
 * channel's `disks` events, abi/idl/volumes.idl). */
#define STORAGE_DRIVER "drv/usb-storage"
/* DEVMGR_SET_VOLUMES: ch (consumed) is the client end of a new DISKS
 * channel: every running disk's driver is asked for a `storage` channel
 * for it again. */
void     disks_set_volumes(handle_t ch);
/* b (a disk's driver) started: a `storage` channel of volumes' own is
 * asked of it without waiting (svc.connect), and handed to volumes
 * (disks.attach) when it comes (disks_events). */
void     disks_started(struct binding *b);
/* b (a disk's driver) has something to read on its channel: the answer to
 * that svc.connect. */
void     disks_events(struct binding *b);
/* b (a disk's driver) won't run again: the device is gone (disks.gone). */
void     disks_device_gone(struct binding *b);

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
