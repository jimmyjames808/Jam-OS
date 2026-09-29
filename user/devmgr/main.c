/* devmgr: binds drivers to PCI functions (M6 phase 2) and keeps them
 * running (M7 supervision). A process in bootfs (bin/devmgr) that init
 * starts with a RES_PCI resource (SR_RESOURCE) sliced from the root, and
 * the server end of its channel (SR_DEVMGR; the protocol, and the
 * reconnect rule its clients follow, are in <devmgr.h>).
 *
 * It enumerates every function (pci_enum), matches each against the table
 * below and, for every match whose driver ELF is in bootfs, wakes the
 * function to D0 if it was left in D1-D3, makes the driver's handles and
 * starts drv/<name> in a job of its own (a child of devmgr's job, with
 * limits; bind.c). The policy is all here; the kernel only enforces
 * rights. A driver gets exactly (roles from <jam/driver.h>):
 *
 *   DR_PCIDEV  its function, RIGHT_READ | RIGHT_WRITE (+ wait, inspect):
 *              filtered config reads/writes, nothing else (no
 *              RIGHT_MANAGE: no bus mastering, dma_caps or interrupt
 *              objects; no RIGHT_SLICE: no BAR resources of its own)
 *   DR_BAR(n)  each memory BAR as a RES_MMIO, RIGHT_MAP (+ wait, inspect)
 *              (the kernel still refuses the MSI-X table / PBA pages)
 *   DR_IRQ(0)  an interrupt object: MSI-X entry 0 if the function has
 *              MSI-X, else its MSI
 *   DR_DMA     a dma_cap bound to the function: the function's new
 *              current cap, which turns Bus Master Enable OFF. The driver
 *              turns it on (drv_dma_bus_master; DMA and MSI both need it)
 *              once it has quiesced the device, so nothing a previous
 *              driver left queued reaches memory (M7). Closing the cap --
 *              the driver exiting or being killed -- turns it off again;
 *              pins still held then are quarantined by the kernel
 *   DR_SERVE   a channel whose other end devmgr keeps (GET_SERVICE hands
 *              out duplicates of it)
 * The hardware handles (all but DR_SERVE) come without RIGHT_DUPLICATE and
 * RIGHT_TRANSFER (<devmgr.h> DEVMGR_DRV_*_RIGHTS): the driver can't pass
 * them on, so nothing of the device outlives the driver's job.
 *
 * M7: a driver may also WRITE on DR_SERVE by itself (txid 0): usb-bus
 * sends `usbbus.interface_attached` (abi/idl/usbbus.idl) with the
 * interface's `usb` channel for each interface of a new device. devmgr
 * watches each driver's channel for these and keeps the channels (usb.c).
 *
 * A driver that dies is restarted with backoff, or given up on
 * (supervise.c). devmgr keeps its own handle to each function (with
 * RIGHT_MANAGE, from pci_device_open) and nothing else of the driver's.
 * Each binding is logged, with one RESULTS line per bound driver.
 *
 * It runs until every client end of its channel is gone (init closes its
 * own at the end of the boot): then it closes each driver's client end,
 * waits for the drivers to return, kills any that don't, and exits 0 if
 * every driver ended cleanly with its job at zero and nothing crashed or
 * was given up on meanwhile. */
#include "internal.h"

/* ---- the match table ---------------------------------------------------------
 * vendor/device (0xffff = any) and/or class (class << 16 | subclass << 8 |
 * prog_if; ANY_CLASS = any). The first match wins; a driver whose ELF
 * isn't in bootfs is skipped with a log line. Adding a driver is one line
 * here and its directory under drivers/. */
#define ANY_CLASS 0xffffffffu
static const struct {
    uint16_t    vendor, device;
    uint32_t    class_code;
    const char *path;
} matches[] = {
    { 0x1234, 0x11e8, ANY_CLASS, "drv/edu" },        /* QEMU's edu test device */
    { 0xffff, 0xffff, 0x0c0330, "drv/usb-bus" },     /* any xHCI controller (M7) */
};

#define TEST_DRIVER_PATH "drv/crasher"

struct binding devs[MAX_DEVS];
unsigned ndevs, problems;
handle_t pci_res, port;
static unsigned nbound, nfailed, nskipped;

void say(bool report_it, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    if (report_it)
        jam_debug_report(buf, (uint64_t)n);
    else
        jam_debug_write(buf, (uint64_t)n);
}

const char *bdf(const struct binding *b)
{
    static char s[16];
    if (b->kind != BIND_PCI)
        return "test";
    snprintf(s, sizeof(s), "%02x:%02x.%x", b->info.bus, b->info.dev, b->info.fn);
    return s;
}

static const char *match(const struct pci_dev_info *i)
{
    if (i->flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return NULL;   /* never a driver's */
    uint32_t cls = (uint32_t)i->class_code << 16 | (uint32_t)i->subclass << 8 | i->prog_if;
    for (unsigned k = 0; k < sizeof(matches) / sizeof(matches[0]); k++)
        if ((matches[k].vendor == 0xffff || matches[k].vendor == i->vendor) &&
            (matches[k].device == 0xffff || matches[k].device == i->device) &&
            (matches[k].class_code == ANY_CLASS || matches[k].class_code == cls))
            return matches[k].path;
    return NULL;
}

bool in_bootfs(const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    return bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, &size) == OK;
}

static void bind_all(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (!b->path)
            continue;
        if (!in_bootfs(b->path)) {
            say(false, "devmgr: %s %04x:%04x: %s is not in bootfs, skipped", bdf(b),
                b->info.vendor, b->info.device, b->path);
            nskipped++;
            b->path = NULL;
            continue;
        }
        b->last = start_driver(b);
        if (b->last != OK) {
            say(true, "devmgr: %s %04x:%04x -> %s: bind FAILED (%s)", bdf(b), b->info.vendor,
                b->info.device, b->path, status_str(b->last));
            nfailed++;
            continue;
        }
        nbound++;
        unsigned bars = mem_bars(b);
        say(true, "devmgr: %s %04x:%04x -> %s (%s, %u BAR%s, dma) bound", bdf(b), b->info.vendor,
            b->info.device, b->path,
            b->info.msix_vectors ? "MSI-X" : b->info.msi_vectors ? "MSI" : "no irq", bars,
            bars == 1 ? "" : "s");
    }
}

/* ---- the protocol --------------------------------------------------------------- */

static struct binding *find(const struct devmgr_req *q, bool msix_wildcard)
{
    uint32_t seen = 0;
    /* GET_SERVICE 0xffff/0xffff: the instance-th function with a driver
     * running (M7: tests find usb-bus this way). */
    bool any_bound = q->ordinal == DEVMGR_GET_SERVICE && q->vendor == 0xffff &&
                     q->device == 0xffff;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        bool hit = any_bound ? b->kind == BIND_PCI && b->proc != HANDLE_INVALID
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
                                      (uint64_t)jam_clock_get() + STOP_WAIT, &seen);
    b->killed = true;
    sup_died(b, b->gen);   /* a death like any other: the restart is scheduled now */
    return st;
}

/* Bind b again from scratch, with a fresh restart history. */
static status_t rebind(struct binding *b)
{
    if (b->proc)
        stop_driver(b, true, true);
    sup_reset(b);
    close_client(b);
    b->last = start_driver(b);
    say(false, "devmgr: %s %s bound again (%s)", bdf(b), b->path, status_str(b->last));
    return b->last;
}

/* Handle one request; the reply (and *nh handles in hs, each to arrive
 * with rs[i]) to send back. */
static void handle(const struct devmgr_req *q, struct devmgr_rep *r, handle_t *hs, rights_t *rs,
                   uint32_t *nh)
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
    if (q->ordinal == DEVMGR_TEST_DRIVER) {
        struct binding *t = test_binding();
        if (!t || !in_bootfs(TEST_DRIVER_PATH))
            r->status = ERR_NOT_FOUND;
        else if (t->state != DEVMGR_SUP_RUNNING && t->state != DEVMGR_SUP_RESTARTING)
            r->status = rebind(t);
        return;
    }
    struct binding *b = find(q, q->ordinal == DEVMGR_DRIVER_VIEW);
    bool known = b && b->path && b->state != DEVMGR_SUP_NONE;   /* a driver was started */
    switch (q->ordinal) {
    case DEVMGR_GET_SERVICE:
        if (!known)
            r->status = ERR_NOT_FOUND;
        else if ((b->state != DEVMGR_SUP_RUNNING && b->state != DEVMGR_SUP_RESTARTING) ||
                 !b->client)
            r->status = ERR_BAD_STATE;
        else if ((r->status = jam_handle_duplicate(b->client, RIGHT_SAME, &hs[0])) == OK)
            *nh = 1;
        return;
    case DEVMGR_GET_DRIVER:
        if (!known) {
            r->status = ERR_NOT_FOUND;
            return;
        }
        if (!b->proc) {
            r->status = ERR_BAD_STATE;
            return;
        }
        r->a = b->index;
        if ((r->status = jam_handle_duplicate(b->proc, RIGHTS_BASIC, &hs[0])) == OK &&
            (r->status = jam_handle_duplicate(b->job, RIGHTS_BASIC, &hs[1])) == OK) {
            *nh = 2;
            if (b->kind == BIND_PCI &&
                (r->status = jam_handle_duplicate(b->dev, RIGHTS_BASIC | RIGHT_READ, &hs[2])) == OK)
                *nh = 3;
        }
        break;
    case DEVMGR_KILL:
        r->status = known ? kill_request(b) : ERR_NOT_FOUND;
        return;
    case DEVMGR_REBIND:
        r->status = b && b->path ? rebind(b) : ERR_NOT_FOUND;
        return;
    case DEVMGR_SUPERVISION:
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
        return;
    case DEVMGR_DRIVER_VIEW: {
        if (!b) {
            r->status = ERR_NOT_FOUND;
            return;
        }
        uint32_t mask = 0;
        r->status = driver_view(b, hs, rs, nh, &mask);
        r->a = mask;
        break;
    }
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

/* Answer everything queued on ch. ERR_PEER_CLOSED once every client is
 * gone and nothing is left to read. */
static status_t serve(handle_t ch)
{
    for (;;) {
        _Alignas(8) uint8_t buf[64];
        handle_t in[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = ch, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)in,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            /* Nothing of ours is that big: take it off the queue unanswered. */
            uint8_t *big = malloc(n ? n : 1);
            handle_t *bh = malloc((nh ? nh : 1) * sizeof(handle_t));
            a.bytes = (uint64_t)(uintptr_t)big;
            a.bytes_cap = n;
            a.handles = (uint64_t)(uintptr_t)bh;
            a.handles_cap = nh;
            if (big && bh && jam_channel_read(&a) == OK)
                for (uint32_t i = 0; i < nh; i++)
                    jam_handle_close(bh[i]);
            free(big);
            free(bh);
            continue;
        }
        if (st != OK)
            return st;
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(in[i]);   /* no request carries handles */
        if (n < 4)
            continue;   /* no txid: nobody to answer */
        struct devmgr_rep r = { .txid = ((struct devmgr_req *)buf)->txid,
                                .status = ERR_INVALID_ARGS };
        handle_t hs[DEVMGR_MAX_HANDLES];
        rights_t rs[DEVMGR_MAX_HANDLES];
        uint32_t nout = 0;
        if (n == sizeof(struct devmgr_req) && !nh)
            handle((struct devmgr_req *)buf, &r, hs, rs, &nout);
        uint32_t rn = r.status == OK ? sizeof(r) : DEVMGR_REP_HDR;
        if (jam_channel_write_rights(ch, &r, rn, hs, rs, nout) != OK)
            for (uint32_t i = 0; i < nout; i++)
                jam_handle_close(hs[i]);   /* the client is gone */
        sup_run_due();   /* a long burst of requests mustn't hold up a restart */
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t ch = startup_handle(SR_DEVMGR);
    pci_res = startup_handle(SR_RESOURCE);
    if (!ch || !pci_res) {
        say(true, "devmgr: no %s in the startup message", ch ? "PCI resource" : "channel");
        return 1;
    }
    status_t st = jam_port_create(&port);
    if (st != OK) {
        say(true, "devmgr: no port (%s)", status_str(st));
        return 1;
    }
    for (uint32_t i = 0; ndevs < MAX_DEVS; i++) {
        struct binding *b = &devs[ndevs];
        st = jam_pci_enum(pci_res, i, &b->info);
        if (st == ERR_OUT_OF_RANGE)
            break;
        if (st != OK) {
            say(true, "devmgr: pci_enum(%u): %s", i, status_str(st));
            return 1;
        }
        b->kind = BIND_PCI;
        b->index = i;
        b->path = match(&b->info);
        ndevs++;
    }
    bind_all();
    say(false, "devmgr: %u function(s), %u driver(s) bound, %u failed, %u skipped; serving",
        ndevs, nbound, nfailed, nskipped);

    bool armed = false;
    for (;;) {
        st = serve(ch);
        if (st != ERR_SHOULD_WAIT)
            break;
        /* ONCE, re-armed after it fires (it fires at once if a message came
         * in meanwhile); a driver's death arrives on the same port, and a
         * due restart ends the wait. */
        if (!armed) {
            st = jam_port_bind(port, ch, KEY_CHANNEL, SIG_READABLE | SIG_PEER_CLOSED,
                               PORT_BIND_ONCE);
            if (st != OK)
                break;
            armed = true;
        }
        struct port_packet pkt;
        st = jam_port_wait(port, sup_next_deadline(), &pkt);
        if (st == OK && pkt.key == KEY_CHANNEL)
            armed = false;
        else if (st == OK && (pkt.key & KEY_DRIVER) && KEY_INDEX(pkt.key) < ndevs)
            sup_died(&devs[KEY_INDEX(pkt.key)], KEY_GEN(pkt.key));
        else if (st == OK && (pkt.key & KEY_EVENTS) && KEY_INDEX(pkt.key) < ndevs) {
            struct binding *b = &devs[KEY_INDEX(pkt.key)];
            if (b->client_key == pkt.key)
                usb_driver_events(b);
        } else if (st == OK && (pkt.key & KEY_USBIF))
            usb_if_closed(pkt.key);
        else if (st != OK && st != ERR_TIMED_OUT)
            break;
        sup_run_due();
    }
    bool ok = nfailed == 0;
    if (st != ERR_PEER_CLOSED) {
        say(true, "devmgr: serving failed (%s)", status_str(st));
        ok = false;
    }
    /* Every client is gone: no more restarts; stop the drivers. */
    unsigned stopped = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        sup_reset(b);
        if (b->proc) {
            if (!stop_driver(b, false, false)) {
                say(true, "devmgr: %s %s did not end cleanly", bdf(b), b->path);
                ok = false;
            }
            stopped++;
        }
        close_client(b);
    }
    for (unsigned i = 0; i < ndevs; i++) {
        if (devs[i].dev)
            jam_handle_close(devs[i].dev);
        usb_bus_gone(&devs[i]);   /* the interface channels we kept */
    }
    if (problems) {
        say(true, "devmgr: %u driver problem(s) while running (crashes, give-ups: see above)",
            problems);
        ok = false;
    }
    say(false, "devmgr: %u driver(s) stopped; exiting", stopped);
    return ok ? 0 : 1;
}
