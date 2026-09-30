/* devmgr: binds drivers to PCI functions and keeps them running
 * (supervise.c). A process in bootfs (bin/devmgr) that init
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
 *              driver left queued reaches memory. Closing the cap --
 *              the driver exiting or being killed -- turns it off again;
 *              pins still held then are quarantined by the kernel
 *   DR_SERVE   a channel whose other end devmgr keeps (GET_SERVICE hands
 *              out duplicates of it)
 * The hardware handles (all but DR_SERVE) come without RIGHT_DUPLICATE and
 * RIGHT_TRANSFER (<devmgr.h> DEVMGR_DRV_*_RIGHTS): the driver can't pass
 * them on, so nothing of the device outlives the driver's job.
 *
 * A driver may also WRITE on DR_SERVE by itself (txid 0): usb-bus
 * sends `usbbus.interface_attached` (abi/idl/usbbus.idl) with the
 * interface's `usb` channel for each interface of a new device. devmgr
 * watches each driver's channel for these and keeps the channels (usb.c).
 *
 * A driver that dies is restarted with backoff, or given up on
 * (supervise.c). devmgr keeps its own handle to each function (with
 * RIGHT_MANAGE, from pci_device_open) and nothing else of the driver's.
 * Each binding is logged, with one RESULTS line per bound driver.
 *
 * The argument "nousb" (init passes it on for the safe mode boot entry)
 * leaves USB host controllers (class 0c03xx) without a driver: no USB at
 * all, the console's input is the serial port alone.
 *
 * USB interfaces usb-bus reports get class drivers (usb.c: class 3 ->
 * drv/hid, each in a job of its own, supervised the same way), connected
 * to the console when there is one (SR_CONSOLE, then DEVMGR_SET_CONSOLE).
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
    uint16_t    vendor, device;   /* PCI ids, 0xffff: any */
    uint32_t    class_code;       /* class << 16 | subclass << 8 | prog_if, or ANY_CLASS */
    const char *path;             /* the driver in bootfs */
} matches[] = {
    { 0x1234, 0x11e8, ANY_CLASS, "drv/edu" },        /* QEMU's edu test device */
    { 0xffff, 0xffff, 0x0c0330, "drv/usb-bus" },     /* any xHCI controller */
};

#define TEST_DRIVER_PATH "drv/crasher"

struct binding devs[MAX_DEVS];
unsigned ndevs, problems;
handle_t pci_res, port;
static unsigned nbound, nfailed, nskipped;
static bool nousb;

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
    static char s[40];
    if (b->kind == BIND_USB) {
        snprintf(s, sizeof(s), "usb %s", b->name + 4);   /* "hid-6.1:0" -> "usb 6.1:0" */
        return s;
    }
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
    if (nousb && (cls >> 8) == 0x0c03) {
        say(false, "devmgr: %02x:%02x.%x %04x:%04x: a USB controller, left alone (nousb)",
            i->bus, i->dev, i->fn, i->vendor, i->device);
        return NULL;
    }
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
     * running (tests find usb-bus this way). */
    bool any_bound = q->ordinal == DEVMGR_GET_SERVICE && q->vendor == 0xffff &&
                     q->device == 0xffff;
    bool usb = q->vendor == DEVMGR_USB_IFACE;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (usb || b->kind == BIND_USB) {
            /* USB class drivers only by DEVMGR_USB_IFACE (id, interface) */
            if (usb && b->kind == BIND_USB && b->path && b->usb_id == q->instance &&
                b->usb_ifnum == q->device)
                return b;
            continue;
        }
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
                                      now() + STOP_WAIT, &seen);
    b->killed = true;
    sup_died(b, b->gen);   /* a death like any other: the restart is scheduled now */
    return st;
}

/* Bind b again from scratch, with a fresh restart history. */
static status_t rebind(struct binding *b)
{
    if (b->proc)
        stop_driver(b, true, true);
    /* Nothing runs now: a start that fails below must not leave it RUNNING
     * with no process (no restart would ever come, KILL would fail). */
    if (b->state == DEVMGR_SUP_RUNNING)
        b->state = DEVMGR_SUP_NONE;
    sup_reset(b);
    close_client(b);
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
 * was started for b. */
static void get_service(const struct binding *b, bool known, struct devmgr_rep *r, handle_t *hs,
                        uint32_t *nh)
{
    if (!known)
        r->status = ERR_NOT_FOUND;
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
        r->status = test_driver();
        return;
    }
    struct binding *b = find(q, q->ordinal == DEVMGR_DRIVER_VIEW);
    bool known = b && b->path && b->state != DEVMGR_SUP_NONE;   /* a driver was started */
    switch (q->ordinal) {
    case DEVMGR_GET_SERVICE:
        get_service(b, known, r, hs, nh);
        return;
    case DEVMGR_GET_DRIVER:
        get_driver(b, known, r, hs, nh);
        break;
    case DEVMGR_KILL:
        r->status = known ? kill_request(b) : ERR_NOT_FOUND;
        return;
    case DEVMGR_REBIND:
        r->status = b && b->path ? rebind(b) : ERR_NOT_FOUND;
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

/* What a query channel (SR_DEVMGR) may ask; the control channel
 * (SR_DEVMGR_CTL) may ask everything. */
static bool query_ok(uint32_t ordinal)
{
    return ordinal == DEVMGR_STATUS || ordinal == DEVMGR_GET_SERVICE ||
           ordinal == DEVMGR_GET_DRIVER || ordinal == DEVMGR_SUPERVISION;
}

/* Answer everything queued on ch (control: the control channel). Returns
 * ERR_SHOULD_WAIT once the queue is empty, ERR_PEER_CLOSED once every
 * client is gone and nothing is left to read. */
static status_t serve(handle_t ch, bool control)
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
            discard(ch, n, nh);   /* nothing of ours is that big */
            continue;
        }
        if (st != OK)
            return st;
        /* Only SET_CONSOLE carries a handle (one), and only on control. */
        const struct devmgr_req *q = (const struct devmgr_req *)buf;
        bool denied = n >= 8 && !control && !query_ok(q->ordinal);
        bool set_console = !denied && n == sizeof(*q) && q->ordinal == DEVMGR_SET_CONSOLE &&
                           nh == 1;
        if (!set_console)
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(in[i]);
        if (n < 4)
            continue;   /* no txid: nobody to answer */
        struct devmgr_rep r = { .txid = q->txid, .status = ERR_INVALID_ARGS };
        handle_t hs[DEVMGR_MAX_HANDLES];
        rights_t rs[DEVMGR_MAX_HANDLES];
        uint32_t nout = 0;
        if (denied) {
            r.status = ERR_ACCESS_DENIED;
        } else if (set_console) {
            usb_new_console(in[0]);
            r.status = OK;
        } else if (n == sizeof(struct devmgr_req) && !nh) {
            handle(q, &r, hs, rs, &nout);
        }
        uint32_t rn = r.status == OK ? sizeof(r) : DEVMGR_REP_HDR;
        if (jam_channel_write_rights(ch, &r, rn, hs, rs, nout) != OK)
            for (uint32_t i = 0; i < nout; i++)
                jam_handle_close(hs[i]);   /* the client is gone */
        sup_run_due();   /* a long burst of requests mustn't hold up a restart */
    }
}

/* ---- main ------------------------------------------------------------------------ */

/* Every PCI function into devs[], each with the driver it matches. */
static status_t enumerate(void)
{
    for (uint32_t i = 0; ndevs < MAX_DEVS; i++) {
        struct binding *b = &devs[ndevs];
        status_t st = jam_pci_enum(pci_res, i, &b->info);
        if (st == ERR_OUT_OF_RANGE)
            break;
        if (st != OK) {
            say(true, "devmgr: pci_enum(%u): %s", i, status_str(st));
            return st;
        }
        b->kind = BIND_PCI;
        b->index = i;
        b->path = match(&b->info);
        ndevs++;
    }
    return OK;
}

/* The port keys of chans[0] (control) and chans[1] (queries). */
static const uint64_t chan_keys[2] = { KEY_CONTROL, KEY_CHANNEL };

/* Answer what is queued on both channels. ERR_SHOULD_WAIT once both are
 * drained; a query channel whose clients are gone is dropped, the life
 * channel's end is the result (ERR_PEER_CLOSED). */
static status_t serve_channels(handle_t chans[2], const bool armed[2], unsigned life)
{
    status_t st = ERR_SHOULD_WAIT;
    for (unsigned c = 0; c < 2 && st == ERR_SHOULD_WAIT; c++) {
        if (!chans[c])
            continue;
        st = serve(chans[c], c == 0);
        if (st == ERR_PEER_CLOSED && c != life) {
            if (armed[c])
                jam_port_unbind(port, chans[c], chan_keys[c]);
            jam_handle_close(chans[c]);   /* nobody queries any more */
            chans[c] = HANDLE_INVALID;
            st = ERR_SHOULD_WAIT;
        }
    }
    return st;
}

/* ONCE, re-armed after it fires (it fires at once if a message came in
 * meanwhile); a driver's death arrives on the same port, and a due
 * restart ends the wait. ERR_SHOULD_WAIT when both are armed. */
static status_t arm_channels(const handle_t chans[2], bool armed[2])
{
    status_t st = ERR_SHOULD_WAIT;
    for (unsigned c = 0; c < 2 && st == ERR_SHOULD_WAIT; c++) {
        if (!chans[c] || armed[c])
            continue;
        st = jam_port_bind(port, chans[c], chan_keys[c], SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_ONCE);
        if (st != OK)
            break;
        armed[c] = true;
        st = ERR_SHOULD_WAIT;
    }
    return st;
}

/* Wait for one packet (or the next due restart) and act on it. */
static status_t wait_event(bool armed[2])
{
    struct port_packet pkt;
    status_t st = jam_port_wait(port, sup_next_deadline(), &pkt);
    if (st != OK)
        return st;
    if (pkt.key == KEY_CHANNEL || pkt.key == KEY_CONTROL) {
        armed[pkt.key == KEY_CONTROL ? 0 : 1] = false;
    } else if ((pkt.key & KEY_DRIVER) && KEY_INDEX(pkt.key) < ndevs) {
        sup_died(&devs[KEY_INDEX(pkt.key)], KEY_GEN(pkt.key));
    } else if ((pkt.key & KEY_EVENTS) && KEY_INDEX(pkt.key) < ndevs) {
        struct binding *b = &devs[KEY_INDEX(pkt.key)];
        if (b->client_key == pkt.key)
            usb_driver_events(b);
    } else if (pkt.key & KEY_USBIF) {
        usb_if_closed(pkt.key);
    }
    return OK;
}

/* Serve until the life channel's clients are all gone (ERR_PEER_CLOSED)
 * or something fails (its status). */
static status_t run(handle_t chans[2], unsigned life)
{
    bool armed[2] = { false, false };
    for (;;) {
        status_t st = serve_channels(chans, armed, life);
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = arm_channels(chans, armed);
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = wait_event(armed);
        if (st != OK && st != ERR_TIMED_OUT)
            return st;
        sup_run_due();
    }
}

/* Every client is gone: no more restarts; stop the drivers. True if each
 * ended cleanly and nothing went wrong while running. */
static bool stop_all(void)
{
    bool ok = true;
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
    return ok;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        nousb |= !strcmp(argv[i], "nousb");
    /* chans[0]: control (SR_DEVMGR_CTL), chans[1]: queries (SR_DEVMGR).
     * devmgr runs until the control channel's clients are all gone (with
     * no control channel: the query channel's); a query channel whose
     * clients are gone is just dropped. */
    handle_t chans[2] = { startup_handle(SR_DEVMGR_CTL), startup_handle(SR_DEVMGR) };
    unsigned life = chans[0] ? 0 : 1;

    pci_res = startup_handle(SR_RESOURCE);
    if (!chans[life] || !pci_res) {
        say(true, "devmgr: no %s in the startup message",
            chans[life] ? "PCI resource" : "channel");
        return 1;
    }
    status_t st = jam_port_create(&port);
    if (st != OK) {
        say(true, "devmgr: no port (%s)", status_str(st));
        return 1;
    }
    /* With a console, class drivers send their input to it. */
    if (startup_handle(SR_CONSOLE))
        usb_new_console(startup_handle(SR_CONSOLE));
    if (enumerate() != OK)
        return 1;
    bind_all();
    say(false, "devmgr: %u function(s), %u driver(s) bound, %u failed, %u skipped; serving",
        ndevs, nbound, nfailed, nskipped);

    st = run(chans, life);
    bool ok = nfailed == 0;
    if (st != ERR_PEER_CLOSED) {
        say(true, "devmgr: serving failed (%s)", status_str(st));
        ok = false;
    }
    if (!stop_all())
        ok = false;
    return ok ? 0 : 1;
}
