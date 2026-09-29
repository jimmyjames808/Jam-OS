/* devmgr's protocol (M6 phase 2): how a program finds a driver devmgr
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
 * Trust: whoever holds the channel is trusted (init and what init starts).
 * GET_DRIVER, KILL, REBIND, DRIVER_VIEW, SUPERVISION and TEST_DRIVER are
 * for tests and administration; a later milestone splits them onto a
 * channel of their own.
 *
 * Supervision (M7): devmgr restarts a driver that dies unexpectedly
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
 * devmgr itself (M7 cleanup): in shell mode init restarts it when it dies,
 * with its whole job (every driver it ran), so every devmgr client end
 * sees ERR_PEER_CLOSED. The new devmgr binds everything again from
 * scratch. A client that needs devmgr for good gets the new client end
 * from whoever gave it the old one: init sends the shell each new one on
 * the shell's SR_USER + 2 channel (a message of one u32
 * INIT_SHELL_DEVMGR carrying the handle); the programs the shell runs get
 * the current one when they start. */
#pragma once

#include <os.h>

/* init -> shell, on the shell's SR_USER + 2 channel: a new devmgr client
 * end (the one handle of the message). */
#define INIT_SHELL_DEVMGR   1u


#define DEVMGR_PROTOCOL_ID  3u   /* next to the IDL's null (1) and edu (2) */
/* -> u32 bound, failed, skipped. Answered once the first binding pass is
 * done, so init can wait for it. */
#define DEVMGR_STATUS       0x00030001u
/* (dev) -> 1 handle: a channel to the driver's DR_SERVE end (a duplicate
 * of devmgr's client end). ERR_NOT_FOUND: no such device, or no driver
 * bound to it (ERR_BAD_STATE: bound, but the driver is gone for good).
 * While a restart is due, the channel the restarted driver will serve
 * (see the reconnect rule above). M7: vendor and device 0xffff name the
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
 * due, or it is gone). */
#define DEVMGR_GET_DRIVER   0x00030003u
/* (dev) -> (): kill the driver's job and answer once it is dead. That is
 * a death like any other: supervision restarts it (M7; not counted as a
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
 * and pages found written while held since boot (M7). */
#define DEVMGR_SUPERVISION  0x00030007u
/* () -> (): start the crash-test driver (drv/crasher) as a supervised
 * driver of the software device DEVMGR_TEST_VENDOR:DEVMGR_TEST_DEVICE
 * (no hardware), unless it runs already; after a give-up it starts again
 * with a fresh restart history. Its deaths and give-up are expected (not
 * problems). ERR_NOT_FOUND: no drv/crasher in bootfs. */
#define DEVMGR_TEST_DRIVER  0x00030008u
/* (1 handle: a client end of the console's channel) -> (): the console to
 * connect class drivers to (M7). init sends it after it restarted the
 * console (the first one comes as SR_CONSOLE); the class drivers that
 * ended because the old console went away start again connected to it. */
#define DEVMGR_SET_CONSOLE  0x00030009u

/* USB class drivers (M7) are named by DEVMGR_USB_IFACE as the vendor, the
 * interface number as the device and usb-bus's device id (usbbus.device's
 * `id`) as the instance, for GET_DRIVER, KILL, REBIND and SUPERVISION.
 * Such a binding exists from the interface's attach until it is gone. */
#define DEVMGR_USB_IFACE    0xfffeu

#define DEVMGR_SUP_NONE       0u   /* no driver started (yet) */
#define DEVMGR_SUP_RUNNING    1u
#define DEVMGR_SUP_RESTARTING 2u   /* died; its restart is due */
#define DEVMGR_SUP_GAVE_UP    3u   /* died too often: no more restarts */
#define DEVMGR_SUP_FINISHED   4u   /* exited 0 by itself (a one-shot driver) */

/* The software device of the crash-test driver, and its protocol (a
 * request is u32 txid, u32 ordinal; drivers/crasher/crasher.c has the
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
 * interrupt or dma_cap out through DR_SERVE to outlive it (M7; review of
 * M6 phase 2, finding 2). devmgr makes them with RIGHT_TRANSFER and hands
 * them over with channel_write_rights. */
#define DEVMGR_DRV_KEEP       (RIGHT_WAIT | RIGHT_INSPECT)
#define DEVMGR_DRV_DEV_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_BAR_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_MAP)
#define DEVMGR_DRV_IRQ_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_DMA_RIGHTS DEVMGR_DRV_KEEP
/* A class driver's channels (M7: DR_USB, DR_INPUT). */
#define DEVMGR_DRV_CHAN_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)

struct devmgr_req {
    uint32_t txid;
    uint32_t ordinal;
    uint16_t vendor, device;   /* STATUS: 0 */
    uint32_t instance;
} __attribute__((packed));

struct devmgr_rep {
    uint32_t txid;
    int32_t  status;
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
