/* devmgr: the answers to its protocol's requests (<devmgr.h>): which
 * binding a request names (find), and what each method does with it.
 * chans.c reads the requests off devmgr's channels and decides what each
 * channel may ask; main.c owns the bindings and the loop. */
#include "internal.h"

#define TEST_DRIVER_PATH "drv/crasher"

static struct binding *find(const struct devmgr_req *q, bool msix_wildcard)
{
    uint32_t seen = 0;
    /* GET_SERVICE 0xffff/0xffff: the instance-th function with a driver
     * running (tests find usb-bus this way). GET_DRIVER and KILL
     * 0xffff/0xffff: the instance-th function with a driver bound, running
     * or not, so the numbering holds while one restarts (init's `kill`
     * finds a PCI driver's process this way). */
    bool any = q->vendor == 0xffff && q->device == 0xffff;
    bool any_running = any && q->ordinal == DEVMGR_GET_SERVICE;
    bool any_bound = any && (q->ordinal == DEVMGR_GET_DRIVER || q->ordinal == DEVMGR_KILL);
    bool usb = q->vendor == DEVMGR_USB_IFACE;
    if (q->vendor == DEVMGR_FS_SVC)
        return fs_find(q->instance, q->device);
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind == BIND_FS)
            continue;   /* filesystem services only by DEVMGR_FS_SVC */
        if (usb || b->kind == BIND_USB) {
            /* USB class drivers only by DEVMGR_USB_IFACE (id, interface) */
            if (usb && b->kind == BIND_USB && b->path && b->usb_id == q->instance &&
                b->usb_ifnum == q->device)
                return b;
            continue;
        }
        bool hit = any_running ? b->kind == BIND_PCI && b->proc != HANDLE_INVALID
                   : any_bound ? b->kind == BIND_PCI && b->path
                   : msix_wildcard && q->vendor == 0xffff && q->device == 0xffff
                       ? b->kind == BIND_PCI && b->info.msix_vectors &&
                             !(b->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
                       : b->info.vendor == q->vendor && b->info.device == q->device;
        if (hit && seen++ == q->instance)
            return b;
    }
    return NULL;
}

/* The crash-test driver's binding (a software device), made on first use. */
static struct binding *test_binding(void)
{
    struct devmgr_req q = { 0, 0, DEVMGR_TEST_VENDOR, DEVMGR_TEST_DEVICE, 0 };
    struct binding *b = find(&q, false);
    if (b || ndevs == MAX_DEVS)
        return b;
    b = &devs[ndevs++];
    *b = (struct binding){ .kind = BIND_SOFT, .path = TEST_DRIVER_PATH, .test = true };
    b->info.vendor = DEVMGR_TEST_VENDOR;
    b->info.device = DEVMGR_TEST_DEVICE;
    return b;
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

/* RELEASE: b is left without a driver until REBIND. A disk's filesystems
 * are synced first: they go with its driver. For a USB interface the
 * caller gets the channel the driver had (hs[0]). */
static void release(struct binding *b, struct devmgr_rep *r, handle_t *hs, uint32_t *nh)
{
    if (b->disk)
        disk_sync_all();
    unbind(b);
    say(false, "devmgr: %s %s released: no driver until it is bound again", bdf(b), b->path);
    if (b->kind == BIND_USB && (r->status = usb_channel(b, &hs[0])) == OK)
        *nh = 1;
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
        b->state = DEVMGR_SUP_RESTARTING;   /* once the console is back */
        b->restart_at = DEADLINE_NEVER;
        b->console_wait = true;
        b->last = OK;
    }
    return b->last;
}

/* TEST_DRIVER: start the crash-test driver unless it runs already. */
static status_t test_driver(void)
{
    struct binding *t = test_binding();
    if (!t || !in_bootfs(TEST_DRIVER_PATH))
        return ERR_NOT_FOUND;
    if (t->state != DEVMGR_SUP_RUNNING && t->state != DEVMGR_SUP_RESTARTING)
        return rebind(t);
    return OK;
}

/* GET_SERVICE: a duplicate of b's client end into hs[0]. known: a driver
 * was started for b. Never a disk's driver's or a filesystem service's: a
 * disk's `storage` channel opens every partition for writing, so it stays
 * devmgr's own, and a filesystem's channel is DEVMGR_MOUNTS's to hand out
 * (the control channel's alone). An exclusive driver's (hda's) only on the
 * audio and control channels. */
static void get_service(const struct binding *b, bool known, enum level lv, struct devmgr_rep *r,
                        handle_t *hs, uint32_t *nh)
{
    if (!known)
        r->status = ERR_NOT_FOUND;
    else if (b->disk || (b->exclusive && lv == LEVEL_QUERY))
        r->status = ERR_ACCESS_DENIED;
    else if ((b->state != DEVMGR_SUP_RUNNING && b->state != DEVMGR_SUP_RESTARTING) ||
             !b->client)
        r->status = ERR_BAD_STATE;
    else if ((r->status = jam_handle_duplicate(b->client, RIGHT_SAME, &hs[0])) == OK)
        *nh = 1;
}

/* GET_DRIVER: read-only views of b's process, job and function. */
static void get_driver(const struct binding *b, bool known, struct devmgr_rep *r, handle_t *hs,
                       uint32_t *nh)
{
    if (!known) {
        r->status = ERR_NOT_FOUND;
        return;
    }
    if (!b->proc) {
        r->status = ERR_BAD_STATE;
        return;
    }
    r->a = b->index;
    const handle_t src[3] = { b->proc, b->job, b->dev };
    const rights_t rights[3] = { RIGHTS_BASIC, RIGHTS_BASIC, RIGHTS_BASIC | RIGHT_READ };
    uint32_t want = b->kind == BIND_PCI ? 3 : 2;   /* only a PCI binding has a function */
    uint32_t got = 0;
    while (got < want && (r->status = jam_handle_duplicate(src[got], rights[got], &hs[got])) == OK)
        got++;
    if (r->status != OK)   /* all or nothing: close what was duplicated */
        while (got > 0)
            jam_handle_close(hs[--got]);
    *nh = got;
}

/* SUPERVISION: b's state, restarts, backoff, and its DMA quarantine. */
static void supervision(const struct binding *b, struct devmgr_rep *r)
{
    if (!b) {
        r->status = ERR_NOT_FOUND;
        return;
    }
    r->a = b->state;
    r->b = b->restarts;
    r->c = b->backoff_ms;
    if (b->kind == BIND_PCI) {
        struct pci_dev_info now;
        if (jam_pci_enum(pci_res, b->index, &now) == OK) {
            r->d = now.dma_quarantined;
            r->e = now.dma_changed;
        }
    }
}

/* DRIVER_VIEW: b's function and memory BARs with a driver's rights. */
static void view(struct binding *b, struct devmgr_rep *r, handle_t *hs, rights_t *rs,
                 uint32_t *nh)
{
    /* A PCI function's only: a USB or soft binding has no function (its
     * index 0 would open PCI function 0, and a reused USB binding would
     * then lose that RIGHT_MANAGE handle). */
    if (!b || b->kind != BIND_PCI) {
        r->status = ERR_NOT_FOUND;
        return;
    }
    uint32_t mask = 0;
    r->status = driver_view(b, hs, rs, nh, &mask);
    r->a = mask;
}

void request_handle(const struct devmgr_req *q, enum level lv, struct devmgr_rep *r,
                    handle_t *hs, rights_t *rs, uint32_t *nh)
{
    *nh = 0;
    for (uint32_t i = 0; i < DEVMGR_MAX_HANDLES; i++)
        rs[i] = RIGHT_SAME;
    r->status = OK;
    if (q->ordinal == DEVMGR_STATUS) {
        r->a = nbound;
        r->b = nfailed;
        r->c = nskipped;
        return;
    }
    if (q->ordinal == DEVMGR_SHUTDOWN) {
        shutdown_asked = true;   /* run() sees it once this reply is sent */
        return;
    }
    if (q->ordinal == DEVMGR_TEST_DRIVER) {
        r->status = test_driver();
        return;
    }
    if (q->ordinal == DEVMGR_REMOUNT) {
        bool valid = q->vendor == DEVMGR_USB_MOUNT && !(q->instance & ~3u);
        r->status = valid ? disk_remount(q->device, q->instance & DEVMGR_REMOUNT_TEST,
                                         q->instance & DEVMGR_REMOUNT_WRITE)
                          : ERR_INVALID_ARGS;
        return;
    }
    struct binding *b = find(q, q->ordinal == DEVMGR_DRIVER_VIEW);
    bool known = b && b->path && b->state != DEVMGR_SUP_NONE;   /* a driver was started */
    switch (q->ordinal) {
    case DEVMGR_GET_SERVICE:
        get_service(b, known, lv, r, hs, nh);
        return;
    case DEVMGR_GET_DRIVER:
        get_driver(b, known, r, hs, nh);
        break;
    case DEVMGR_KILL:
        r->status = known ? kill_request(b) : ERR_NOT_FOUND;
        return;
    case DEVMGR_REBIND:
        /* not a filesystem service: those come and go with their disk */
        r->status = b && b->path && b->kind != BIND_FS ? rebind(b) : ERR_NOT_FOUND;
        return;
    case DEVMGR_RELEASE:
        if (b && b->path && b->kind != BIND_FS)
            release(b, r, hs, nh);
        else
            r->status = ERR_NOT_FOUND;
        return;
    case DEVMGR_SUPERVISION:
        supervision(b, r);
        return;
    case DEVMGR_DRIVER_VIEW:
        view(b, r, hs, rs, nh);
        break;
    default:
        r->status = ERR_NOT_SUPPORTED;
        return;
    }
    if (r->status != OK) {   /* hand out all or nothing */
        for (uint32_t i = 0; i < *nh; i++)
            jam_handle_close(hs[i]);
        *nh = 0;
    }
}
