/* devmgr's own pieces (M7): main.c (startup, the protocol, the event
 * loop), bind.c (starting and stopping a driver: its handles, its job),
 * supervise.c (what happens when a driver dies: restart with backoff, or
 * give up), usb.c (the USB interfaces usb-bus reports). The protocol is in
 * <devmgr.h>. */
#pragma once

#include <os.h>
#include <devmgr.h>
#include <jam/driver.h>

#define MS        1000000ull
#define S         1000000000ull
#define MAX_DEVS  64
#define STOP_WAIT (15 * S)   /* > xhci-noop's worst case (~11 s of bounded waits) */

/* Supervision (supervise.c). */
#define SUP_BACKOFF_FIRST (100 * MS)
#define SUP_BACKOFF_MAX   (5 * S)
#define SUP_WINDOW        (60 * S)
#define SUP_RESTART_LIMIT 5   /* restarts within SUP_WINDOW; the next death gives up */

/* Port keys: devmgr's channel, and a driver process's SIG_TERMINATED
 * (binding index and start generation, so a stale packet is recognised). */
#define KEY_CHANNEL        1ull
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
};

struct binding {
    enum bind_kind      kind;
    uint32_t            index;      /* BIND_PCI: pci_enum's */
    struct pci_dev_info info;       /* BIND_PCI: pci_enum's; BIND_SOFT: vendor/device only */
    const char         *path;       /* the driver; NULL: none for it */
    bool                test;       /* its deaths and giving up are expected (not problems) */
    handle_t            dev;        /* BIND_PCI: ours, with RIGHT_MANAGE (0 until started once) */
    /* The driver while it runs. */
    handle_t            job, proc;
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

/* usb.c (M7): the interfaces usb-bus reports on its DR_SERVE channel. */
#define MAX_USB_IFS 64
/* b's driver wrote on its channel by itself (KEY_EVENTS). */
void usb_driver_events(struct binding *b);
/* A kept interface channel's KEY_USBIF packet. */
void usb_if_closed(uint64_t key);
/* b's driver (a usb-bus) is gone: forget the interfaces it reported. */
void usb_bus_gone(struct binding *b);

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
