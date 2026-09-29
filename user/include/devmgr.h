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
 * GET_DRIVER, KILL, REBIND and DRIVER_VIEW are for tests and
 * administration; a later milestone splits them onto a channel of their
 * own. */
#pragma once

#include <os.h>

#define DEVMGR_PROTOCOL_ID  3u   /* next to the IDL's null (1) and edu (2) */
/* -> u32 bound, failed, skipped. Answered once the first binding pass is
 * done, so init can wait for it. */
#define DEVMGR_STATUS       0x00030001u
/* (dev) -> 1 handle: a channel to the driver's DR_SERVE end (a duplicate
 * of devmgr's client end). ERR_NOT_FOUND: no such device, or no driver
 * bound to it (ERR_BAD_STATE: bound, but the driver is gone). */
#define DEVMGR_GET_SERVICE  0x00030002u
/* (dev) -> 3 handles, read-only views of a bound driver: its process
 * (RIGHTS_BASIC: wait, info), its job (RIGHTS_BASIC: wait, job_get_info)
 * and its function (RIGHTS_BASIC | RIGHT_READ: config reads), plus u32
 * pci_index. */
#define DEVMGR_GET_DRIVER   0x00030003u
/* (dev) -> (): kill the driver's process and answer once it is dead. */
#define DEVMGR_KILL         0x00030004u
/* (dev) -> (): kill the driver if it still runs, then bind the device
 * again from scratch (new interrupt object, dma_cap, channel). */
#define DEVMGR_REBIND       0x00030005u
/* (dev) -> handles: its function with a driver's rights and each memory
 * BAR as a driver gets it, plus u32 bar_mask (bit n: BAR n, in handle
 * order after the function). The rights a driver has, without the
 * interrupt object and the dma_cap, for tests of what drivers can't do. */
#define DEVMGR_DRIVER_VIEW  0x00030006u

/* What devmgr gives each driver (and DRIVER_VIEW hands out). */
#define DEVMGR_DRV_DEV_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_BAR_RIGHTS (RIGHTS_BASIC | RIGHT_MAP)

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
                                * DRIVER_VIEW: bar_mask */
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
