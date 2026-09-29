/* devmgr's own pieces: main.c (startup, the protocol, the event
 * loop), bind.c (starting and stopping a driver: its handles, its job),
 * supervise.c (what happens when a driver dies: restart with backoff, or
 * give up), usb.c (the USB interfaces usb-bus reports). The protocol is in
 * <devmgr.h>. */
#pragma once

#include <os.h>
#include <devmgr.h>
#include <jam/driver.h>

#define MAX_DEVS  128   /* PCI functions, the crash-test driver, USB class drivers */
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

/* Port keys: devmgr's channel, and a driver process's SIG_TERMINATED
 * (binding index and start generation, so a stale packet is recognised). */
#define KEY_CHANNEL        1ull   /* the query channel (SR_DEVMGR) */
#define KEY_CONTROL        2ull   /* the control channel (SR_DEVMGR_CTL) */
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
#define KEY_INDEX(k)       ((uint32_t)((k) >> 16) & 0xffffu)
#define KEY_GEN(k)         ((uint32_t)(k) & 0xffffu)

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
    char                name[32];   /* its process name, "hid-<path>:<if>" */
};

extern struct binding devs[MAX_DEVS];
extern unsigned       ndevs;
extern handle_t       pci_res, port;
/* Problems for the exit code: real drivers that crashed or were given up
 * on, and drivers that didn't end cleanly. */
extern unsigned       problems;

void say(bool report_it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *bdf(const struct binding *b);   /* "00:04.0", or "test" */
bool in_bootfs(const char *path);

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
/* Is the job empty? Logs what's left if not. */
bool job_empty(handle_t job, const char *who);
/* DEVMGR_DRIVER_VIEW: the function and each memory BAR as a driver gets
 * them (hs[i] to be sent with rs[i]); *mask: which BARs. */
status_t driver_view(struct binding *b, handle_t *hs, rights_t *rs, uint32_t *nh, uint32_t *mask);
unsigned mem_bars(const struct binding *b);

/* Close b's client end (and stop watching it). */
void close_client(struct binding *b);

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
/* b is a BIND_USB binding that won't run again: free its slot. */
void usb_retire(struct binding *b, const char *why);
/* Is b's interface gone? */
bool usb_gone(const struct binding *b);
/* The console: devmgr's client end of it (0: none), from SR_CONSOLE
 * or DEVMGR_SET_CONSOLE. A new one restarts the class drivers waiting for
 * it. */
extern handle_t console;
void usb_new_console(handle_t ch);
/* Could b's driver have ended because the console went away? */
bool usb_console_gone(const struct binding *b);

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
