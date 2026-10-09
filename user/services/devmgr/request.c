/* devmgr: the answers to its protocols' requests (abi/idl/devmgr.idl):
 * which binding an address names (find), and what each method does with
 * it. chans.c reads the requests off devmgr's channels, decides which
 * protocol and so which methods a channel may ask, and runs the generated
 * dispatch over the tables at the bottom; main.c owns the bindings and the
 * loop. Each handler's ctx is the `struct request_from` of the channel the
 * request came on. */
#include "internal.h"

#define TEST_DRIVER_PATH "drv/crasher"
/* The crash-test driver's software device has the PCI-style ids 0000:c4a5 (in logs). */
#define TEST_VENDOR 0x0000u
#define TEST_DEVICE 0xc4a5u

/* KIND_PCI_CLASS: the n-th PCI function of class `cls` that has a driver
 * (bound now or not), in enumeration order. */
static struct binding *class_find(uint32_t cls, uint32_t n)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        uint32_t c = (uint32_t)b->info.class_code << 16 | (uint32_t)b->info.subclass << 8 |
                     b->info.prog_if;
        if (b->kind == BIND_PCI && b->path && c == cls && n-- == 0)
            return b;
    }
    return NULL;
}

/* The n-th PCI binding for which hit(b) holds. */
static struct binding *nth_pci(uint32_t n, bool (*hit)(const struct binding *, uint32_t, uint32_t),
                               uint32_t a, uint32_t b2)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind == BIND_PCI && hit(b, a, b2) && n-- == 0)
            return b;
    }
    return NULL;
}

static bool hit_ids(const struct binding *b, uint32_t vendor, uint32_t device)
{
    return b->info.vendor == vendor && b->info.device == device;
}

static bool hit_running(const struct binding *b, uint32_t a, uint32_t c)
{
    (void)a;
    (void)c;
    return b->proc != HANDLE_INVALID;
}

static bool hit_bound(const struct binding *b, uint32_t a, uint32_t c)
{
    (void)a;
    (void)c;
    return b->path != NULL;
}

static bool hit_msix(const struct binding *b, uint32_t a, uint32_t c)
{
    (void)a;
    (void)c;
    return b->info.msix_vectors && !(b->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY));
}

static bool hit_bdf(const struct binding *b, uint32_t bdf, uint32_t c)
{
    (void)c;
    return b->info.segment == 0 && b->info.bus == (bdf >> 8) && b->info.dev == ((bdf >> 3) & 31u) &&
           b->info.fn == (bdf & 7u);
}

/* The binding the address (kind, a, b, n) names, or NULL for none; the
 * status is ERR_INVALID_ARGS for an address that isn't one (a bad kind, a
 * field a kind says is 0), else OK. running_only: KIND_PCI_BOUND counts
 * the functions whose driver runs now (get_service's numbering), not those
 * with a driver bound (everyone else's). */
static status_t find(uint8_t kind, uint32_t a, uint32_t b, uint32_t n, bool running_only,
                     struct binding **out)
{
    struct binding *found = NULL;
    switch (kind) {
    case DEVMGR_KIND_PCI_IDS:
        found = nth_pci(n, hit_ids, a, b);
        break;
    case DEVMGR_KIND_PCI_CLASS:
        if (b)
            return ERR_INVALID_ARGS;
        found = class_find(a, n);
        break;
    case DEVMGR_KIND_PCI_BOUND:
        if (a || b)
            return ERR_INVALID_ARGS;
        found = nth_pci(n, running_only ? hit_running : hit_bound, 0, 0);
        break;
    case DEVMGR_KIND_PCI_MSIX:
        if (a || b)
            return ERR_INVALID_ARGS;
        found = nth_pci(n, hit_msix, 0, 0);
        break;
    case DEVMGR_KIND_PCI_BDF:
        if (a > 0xffffu || b || n)
            return ERR_INVALID_ARGS;
        found = nth_pci(0, hit_bdf, a, 0);
        break;
    case DEVMGR_KIND_USB_IFACE:
        if (b > 0xffu || n)
            return ERR_INVALID_ARGS;
        for (unsigned i = 0; i < ndevs && !found; i++)
            if (devs[i].kind == BIND_USB && devs[i].path && devs[i].usb_id == a &&
                devs[i].usb_ifnum == b)
                found = &devs[i];
        break;
    case DEVMGR_KIND_TEST:
        if (a || b || n)
            return ERR_INVALID_ARGS;
        for (unsigned i = 0; i < ndevs && !found; i++)
            if (devs[i].kind == BIND_SOFT)
                found = &devs[i];
        break;
    default:   /* KIND_SELF is the channel's, resolved before this */
        return ERR_INVALID_ARGS;
    }
    *out = found;
    return OK;
}

/* The crash-test driver's binding (a software device), made on first use. */
static struct binding *test_binding(void)
{
    struct binding *b = NULL;
    if (find(DEVMGR_KIND_TEST, 0, 0, 0, false, &b) != OK || b || ndevs == MAX_DEVS)
        return b;
    b = &devs[ndevs++];
    *b = (struct binding){ .kind = BIND_SOFT, .path = TEST_DRIVER_PATH, .test = true };
    b->info.vendor = TEST_VENDOR;
    b->info.device = TEST_DEVICE;
    return b;
}

/* The binding an address names, as the channel the request came on may
 * see it: on a device channel KIND_SELF is the channel's own device and
 * any other device is ERR_ACCESS_DENIED. *out is NULL for no such device. */
static status_t named(const struct request_from *from, uint8_t kind, uint32_t a, uint32_t b,
                      uint32_t n, bool running_only, struct binding **out)
{
    struct binding *bd = NULL;
    if (kind == DEVMGR_KIND_SELF) {
        if (from->lv != LEVEL_DEVICE || a || b || n)
            return ERR_INVALID_ARGS;   /* only a device channel has a device of its own */
        bd = &devs[from->dev];
    } else {
        status_t st = find(kind, a, b, n, running_only, &bd);
        if (st != OK)
            return st;
    }
    if (from->lv == LEVEL_DEVICE && bd != &devs[from->dev])
        return ERR_ACCESS_DENIED;
    *out = bd;
    return OK;
}

/* A driver was started for b (and b isn't gone). */
static bool known(const struct binding *b)
{
    return b && b->path && b->state != DEVMGR_SUP_NONE;
}

static status_t kill_request(struct binding *b)
{
    if (b->state != DEVMGR_SUP_RUNNING)
        return OK;   /* nothing runs */
    kill_driver(b);
    signals_t seen;
    status_t st = jam_object_wait_one(b->proc, SIG_TERMINATED,
                                      now() + STOP_WAIT, &seen);
    b->killed = true;
    sup_died(b, b->gen);   /* a death like any other: the restart is scheduled now */
    return st;
}

/* b's driver goes and no restart is due: a fresh restart history. */
static void unbind(struct binding *b)
{
    if (b->proc)
        stop_driver(b, true, true);
    /* Nothing runs now: it must not stay RUNNING with no process (no
     * restart would ever come, KILL would fail). */
    if (b->state == DEVMGR_SUP_RUNNING)
        b->state = DEVMGR_SUP_NONE;
    sup_reset(b);
    close_client(b);
}

/* release: b is left without a driver until rebind. A disk's mounts go
 * with its driver: the caller asked volumes to stop them first
 * (volumes.release_disk). For a USB interface, *usb (not NULL) gets the
 * channel the driver had. */
static status_t release(struct binding *b, handle_t *usb)
{
    unbind(b);
    say(false, "devmgr: %s %s released: no driver until it is bound again", bdf(b), b->path);
    if (usb && b->kind == BIND_USB)
        return usb_channel(b, usb);
    return OK;
}

/* Bind b again from scratch, with a fresh restart history. */
static status_t rebind(struct binding *b)
{
    unbind(b);
    b->last = start_driver(b);
    say(false, "devmgr: %s %s bound again (%s)", bdf(b), b->path, status_str(b->last));
    if (b->kind == BIND_USB && b->last == ERR_PEER_CLOSED) {
        usb_retire(b, "device gone");
    } else if (b->kind == BIND_USB && b->last == ERR_SHOULD_WAIT) {
        b->state = DEVMGR_SUP_RESTARTING;   /* once the input is back */
        b->restart_at = DEADLINE_NEVER;
        b->input_wait = true;
        b->last = OK;
    }
    return b->last;
}

/* test_driver: start the crash-test driver unless it runs already. */
static status_t test_driver(void)
{
    struct binding *t = test_binding();
    if (!t || !in_bootfs(TEST_DRIVER_PATH))
        return ERR_NOT_FOUND;
    if (t->state != DEVMGR_SUP_RUNNING && t->state != DEVMGR_SUP_RESTARTING)
        return rebind(t);
    return OK;
}

/* ---- devmgr ------------------------------------------------------------------------- */

static status_t d_status(void *ctx, uint32_t *out_bound, uint32_t *out_failed,
                         uint32_t *out_skipped)
{
    const struct request_from *from = ctx;
    if (from->lv == LEVEL_DEVICE)
        return ERR_ACCESS_DENIED;   /* a device channel is about its device alone */
    *out_bound = nbound;
    *out_failed = nfailed;
    *out_skipped = nskipped;
    return OK;
}

/* get_service: a duplicate of b's client end. A driver was started for b.
 * Never a disk's driver's: a disk's `storage` channel opens every
 * partition for writing, so it stays devmgr's own (and the one it asks for
 * volumes, volumes'). Never, on the query channel, a device that has a
 * device channel: its holder's (and the control channel's). */
static status_t d_get_service(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                              handle_t *out_service)
{
    const struct request_from *from = ctx;
    struct binding *bd = NULL;
    status_t st = named(from, kind, a, b, n, true, &bd);
    if (st != OK)
        return st;
    if (!known(bd))
        return ERR_NOT_FOUND;
    if (bd->disk || (from->lv == LEVEL_QUERY && chans_device_owned((uint32_t)(bd - devs))))
        return ERR_ACCESS_DENIED;
    if ((bd->state != DEVMGR_SUP_RUNNING && bd->state != DEVMGR_SUP_RESTARTING) || !bd->client)
        return ERR_BAD_STATE;
    return jam_handle_duplicate(bd->client, RIGHT_SAME, out_service);
}

/* get_driver: read-only views of b's process and job. */
static status_t d_get_driver(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                             handle_t *out_process, handle_t *out_job, uint32_t *out_pci_index)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!known(bd))
        return ERR_NOT_FOUND;
    if (!bd->proc)
        return ERR_BAD_STATE;
    if ((st = jam_handle_duplicate(bd->proc, RIGHTS_BASIC, out_process)) != OK)
        return st;
    if ((st = jam_handle_duplicate(bd->job, RIGHTS_BASIC, out_job)) != OK) {
        jam_handle_close(*out_process);   /* all or nothing */
        *out_process = HANDLE_INVALID;
        return st;
    }
    *out_pci_index = bd->index;
    return OK;
}

/* get_function: a read-only view of b's PCI function. */
static status_t d_get_function(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                               handle_t *out_function)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!known(bd))
        return ERR_NOT_FOUND;
    if (bd->kind != BIND_PCI)
        return ERR_NOT_SUPPORTED;   /* only a PCI binding has a function */
    if (!bd->proc)
        return ERR_BAD_STATE;
    return jam_handle_duplicate(bd->dev, RIGHTS_BASIC | RIGHT_READ, out_function);
}

/* supervision: b's state, restarts, backoff, and its DMA quarantine. */
static status_t d_supervision(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                              uint32_t *out_state, uint32_t *out_restarts,
                              uint32_t *out_backoff_ms, uint32_t *out_dma_quarantined,
                              uint32_t *out_dma_changed)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!bd)
        return ERR_NOT_FOUND;
    *out_state = bd->state;
    *out_restarts = bd->win.restarts;
    *out_backoff_ms = bd->win.backoff_ms;
    *out_dma_quarantined = *out_dma_changed = 0;
    if (bd->kind == BIND_PCI) {
        struct pci_dev_info cur;
        if (jam_pci_enum(pci_res, bd->index, &cur) == OK) {
            *out_dma_quarantined = cur.dma_quarantined;
            *out_dma_changed = cur.dma_changed;
        }
    }
    return OK;
}

const struct devmgr_ops devmgr_ops = {
    .status = d_status,
    .get_service = d_get_service,
    .get_driver = d_get_driver,
    .get_function = d_get_function,
    .supervision = d_supervision,
};

/* ---- devmgrctl: the control channel only (chans.c) ------------------------------------- */

static status_t c_kill(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                       uint64_t *out_koid)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!known(bd))
        return ERR_NOT_FOUND;
    struct process_info info;
    *out_koid = bd->proc && jam_process_get_info(bd->proc, &info) == OK ? info.koid : 0;
    return kill_request(bd);
}

/* kill_process: the driver whose process is `koid`, wherever it is bound. */
static status_t c_kill_process(void *ctx, uint64_t koid)
{
    (void)ctx;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *bd = &devs[i];
        struct process_info info;
        if (!known(bd) || !bd->proc || jam_process_get_info(bd->proc, &info) != OK ||
            info.koid != koid)
            continue;
        return kill_request(bd);
    }
    return ERR_NOT_FOUND;
}

static status_t c_rebind(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    return bd && bd->path ? rebind(bd) : ERR_NOT_FOUND;
}

static status_t c_release(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    return bd && bd->path ? release(bd, NULL) : ERR_NOT_FOUND;
}

static status_t c_release_usb(void *ctx, uint32_t dev, uint32_t iface, handle_t *out_usb)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, DEVMGR_KIND_USB_IFACE, dev, iface, 0, false, &bd);
    if (st != OK)
        return st;
    return bd && bd->path ? release(bd, out_usb) : ERR_NOT_FOUND;
}

/* The PCI binding an address names, for the driver's views. */
static status_t pci_named(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                          struct binding **out)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    /* A PCI function's only: a USB or soft binding has no function (its
     * index 0 would open PCI function 0, and a reused USB binding would
     * then lose that RIGHT_MANAGE handle). */
    if (!bd || bd->kind != BIND_PCI)
        return ERR_NOT_FOUND;
    *out = bd;
    return OK;
}

static status_t c_driver_view(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                              handle_t *out_function, uint32_t *out_bar_mask)
{
    struct binding *bd = NULL;
    status_t st = pci_named(ctx, kind, a, b, n, &bd);
    if (st != OK)
        return st;
    return driver_function(bd, out_function, out_bar_mask);
}

static status_t c_driver_bar(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                             uint8_t bar, handle_t *out_bar)
{
    struct binding *bd = NULL;
    status_t st = pci_named(ctx, kind, a, b, n, &bd);
    if (st != OK)
        return st;
    return driver_bar(bd, bar, out_bar);
}

static status_t c_test_driver(void *ctx)
{
    (void)ctx;
    return test_driver();
}

static status_t c_set_input(void *ctx, handle_t input)
{
    (void)ctx;
    usb_new_input(input);   /* consumed */
    return OK;
}

static status_t c_set_volumes(void *ctx, handle_t disks)
{
    (void)ctx;
    disks_set_volumes(disks);   /* consumed */
    return OK;
}

static status_t c_device_channel(void *ctx, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                                 handle_t *out_device)
{
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!bd || !bd->path)
        return ERR_NOT_FOUND;
    if (bd->kind != BIND_PCI && bd->kind != BIND_SOFT)
        return ERR_NOT_SUPPORTED;   /* USB bindings come and go */
    return chans_new_device((uint32_t)(bd - devs), out_device);
}

/* disk_storage: a `storage` channel of the caller's own to a disk's
 * driver, answered when the driver has answered svc.connect (disks.c). */
static status_t c_disk_storage(void *ctx, struct idl_txn txn, uint8_t kind, uint32_t a,
                               uint32_t b, uint32_t n, handle_t *out_storage)
{
    (void)out_storage;
    struct binding *bd = NULL;
    status_t st = named(ctx, kind, a, b, n, false, &bd);
    if (st != OK)
        return st;
    if (!known(bd))
        return ERR_NOT_FOUND;
    return disks_test_storage(bd, txn);
}

static status_t c_shutdown(void *ctx)
{
    (void)ctx;
    shutdown_asked = true;   /* run() sees it once this reply is sent */
    return OK;
}

const struct devmgrctl_ops devmgrctl_ops = {
    .kill = c_kill,
    .rebind = c_rebind,
    .release = c_release,
    .release_usb = c_release_usb,
    .driver_view = c_driver_view,
    .driver_bar = c_driver_bar,
    .test_driver = c_test_driver,
    .set_input = c_set_input,
    .set_volumes = c_set_volumes,
    .device_channel = c_device_channel,
    .shutdown = c_shutdown,
    .kill_process = c_kill_process,
    .disk_storage = c_disk_storage,
};
