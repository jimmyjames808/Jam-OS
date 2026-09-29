/* devmgr: binds drivers to PCI functions (M6 phase 2). A process in
 * bootfs (bin/devmgr) that init starts with a RES_PCI resource
 * (SR_RESOURCE) sliced from the root, and the server end of its channel
 * (SR_DEVMGR; the protocol is in <devmgr.h>).
 *
 * It enumerates every function (pci_enum), matches each against the table
 * below and, for every match whose driver ELF is in bootfs, makes the
 * driver's handles and starts drv/<name> in a job of its own (a child of
 * devmgr's job, with limits). The policy is all here; the kernel only
 * enforces rights. A driver gets exactly (roles from <jam/driver.h>):
 *
 *   DR_PCIDEV  its function, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE:
 *              filtered config reads/writes, nothing else (no
 *              RIGHT_MANAGE: no bus mastering, dma_caps or interrupt
 *              objects; no RIGHT_SLICE: no BAR resources of its own)
 *   DR_BAR(n)  each memory BAR as a RES_MMIO, RIGHTS_BASIC | RIGHT_MAP
 *              (the kernel still refuses the MSI-X table / PBA pages)
 *   DR_IRQ(0)  an interrupt object: MSI-X entry 0 if the function has
 *              MSI-X, else its MSI
 *   DR_DMA     a dma_cap bound to the function; devmgr turns Bus Master
 *              Enable on (DMA and MSI both need it); closing the cap -- the
 *              driver exiting or being killed -- turns it off and releases
 *              every pin
 *   DR_SERVE   a channel whose other end devmgr keeps (GET_SERVICE hands
 *              out duplicates of it)
 *
 * devmgr keeps its own handle to each function (with RIGHT_MANAGE, from
 * pci_device_open) and nothing else of the driver's. Each binding is
 * logged, with one RESULTS line per bound driver.
 *
 * It runs until every client end of its channel is gone (init closes its
 * own at the end of the boot): then it closes each driver's client end,
 * waits for the drivers to return, kills any that don't, and exits 0 if
 * every driver ended cleanly with its job at zero. */
#include <os.h>
#include <devmgr.h>
#include <jam/driver.h>

#define MS          1000000ull
#define S           1000000000ull
#define MAX_DEVS    64
#define STOP_WAIT   (5 * S)
#define KEY_CHANNEL 1
#define KEY_DRIVER  0x100   /* + binding index: its process terminated */

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
    { 0xffff, 0xffff, 0x0c0330, "drv/xhci-noop" },   /* any xHCI controller */
};

/* Per-driver job limits: 16 MiB, 256 handles, 16 threads, 1 MiB queued. */
static const struct { uint32_t kind; uint64_t value; } limits[] = {
    { JOB_LIMIT_PAGES, 4096 },
    { JOB_LIMIT_HANDLES, 256 },
    { JOB_LIMIT_THREADS, 16 },
    { JOB_LIMIT_MSG_BYTES, 1u << 20 },
};

struct binding {
    uint32_t            index;      /* pci_enum's */
    struct pci_dev_info info;
    const char         *path;       /* NULL: no driver for it */
    handle_t            dev;        /* ours, with RIGHT_MANAGE (0 until bound once) */
    handle_t            job, proc, client;   /* while bound */
    bool                running;
    bool                killed;     /* by DEVMGR_KILL: its end needn't be clean */
    status_t            last;       /* the last bind's status */
};

static struct binding devs[MAX_DEVS];
static unsigned ndevs, nbound, nfailed, nskipped;
static handle_t pci_res, port;

static void say(bool report_it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(bool report_it, const char *fmt, ...)
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

static const char *bdf(const struct binding *b)
{
    static char s[16];
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

static bool in_bootfs(const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    return bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, &size) == OK;
}

static bool is_mem_bar(const struct pci_dev_info *i, unsigned n)
{
    uint32_t f = i->bar[n].flags;
    return i->bar[n].size && (f & PCI_BAR_MMIO) && !(f & PCI_BAR_UNSIZED);
}

/* ---- binding ------------------------------------------------------------------ */

/* A driver's copy of one of our handles, with fewer rights. */
static status_t narrowed(handle_t h, rights_t rights, handle_t *out)
{
    return jam_handle_duplicate(h, rights, out);
}

/* A BAR resource with a driver's rights (made from our device handle, it
 * starts with ours). */
static status_t bar_for_driver(handle_t dev, unsigned n, handle_t *out)
{
    handle_t bar;
    status_t st = jam_pci_bar_resource(dev, n, &bar);
    if (st != OK)
        return st;
    st = jam_handle_replace(bar, DEVMGR_DRV_BAR_RIGHTS, out);
    if (st != OK)
        jam_handle_close(bar);
    return st;
}

static status_t bind(struct binding *b)
{
    struct spawn_handle x[STARTUP_MAX_HANDLES];
    unsigned n = 0;
    handle_t h, job = HANDLE_INVALID, client = HANDLE_INVALID, proc;
    status_t st = OK;
    if (!b->dev)
        st = jam_pci_device_open(pci_res, b->index, &b->dev);
    if (st == OK && (st = narrowed(b->dev, DEVMGR_DRV_DEV_RIGHTS, &h)) == OK)
        x[n++] = (struct spawn_handle){ SR_DRIVER(DR_PCIDEV), h };
    for (unsigned i = 0; st == OK && i < 6; i++)
        if (is_mem_bar(&b->info, i) && (st = bar_for_driver(b->dev, i, &h)) == OK)
            x[n++] = (struct spawn_handle){ SR_DRIVER(DR_BAR(i)), h };
    if (st == OK && (b->info.msix_vectors || b->info.msi_vectors)) {
        st = jam_interrupt_create_msi(b->dev, 0, b->info.msix_vectors ? IRQ_MSIX : 0, &h);
        if (st == OK)
            x[n++] = (struct spawn_handle){ SR_DRIVER(DR_IRQ(0)), h };
    }
    if (st == OK && (st = jam_dma_cap_create(b->dev, &h)) == OK)
        x[n++] = (struct spawn_handle){ SR_DRIVER(DR_DMA), h };
    if (st == OK)
        st = jam_pci_bus_master(b->dev, 1);   /* off again when the dma_cap goes */
    if (st == OK && (st = jam_channel_create(&client, &h)) == OK)
        x[n++] = (struct spawn_handle){ SR_DRIVER(DR_SERVE), h };
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    for (unsigned i = 0; st == OK && i < sizeof(limits) / sizeof(limits[0]); i++)
        st = jam_job_set_limit(job, limits[i].kind, limits[i].value);
    if (st == OK) {
        const char *argv[] = { b->path };
        struct spawn_args a = {
            .path = b->path, .argc = 1, .argv = argv, .job = job, .extra = x, .nextra = n,
        };
        st = spawn(&a, &proc);   /* consumes the extras either way */
        n = 0;
    }
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(x[i].h);   /* the dma_cap going turns bus mastering off */
    if (st == OK) {
        uint64_t key = KEY_DRIVER + (uint64_t)(b - devs);
        st = jam_port_bind(port, proc, key, SIG_TERMINATED, PORT_BIND_ONCE);
        if (st != OK)
            jam_handle_close(proc);   /* the job kill below takes the process */
    }
    if (st != OK) {
        if (client)
            jam_handle_close(client);
        if (job) {
            jam_job_kill(job);   /* whatever did start */
            jam_handle_close(job);
        }
        return st;
    }
    b->job = job;
    b->proc = proc;
    b->client = client;
    b->running = true;
    b->killed = false;
    return OK;
}

static bool job_empty(handle_t job, const char *who)
{
    struct job_info ji;
    if (jam_job_get_info(job, &ji) != OK)
        return false;
    bool empty = true;
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k]) {
            say(false, "devmgr: %s left %lu units of job kind %u", who,
                (unsigned long)ji.used[k], k);
            empty = false;
        }
    return empty;
}

/* The driver process is dead: record how it ended. */
static void reaped(struct binding *b)
{
    struct process_info info;
    if (!b->running || jam_process_get_info(b->proc, &info) != OK ||
        info.state != PROCESS_DEAD)
        return;
    b->running = false;
    bool bad = !b->killed && (info.killed || info.exit_code);   /* KILL: expected */
    say(bad, "devmgr: %s %s %s", bdf(b), b->path,
        info.killed ? "was killed" : info.exit_code ? "exited with an error" : "exited");
}

/* Kill b's driver: its whole job (killing only the process would leave
 * anything it started holding its dma_cap, interrupt object and BARs),
 * then Bus Master Enable off through our own handle, in case a dma_cap it
 * sent away (on DR_SERVE) outlives it. Review of M6 phase 2. */
static void kill_driver(struct binding *b)
{
    jam_job_kill(b->job);   /* returns once everything in it is dead */
    jam_pci_bus_master(b->dev, 0);
}

/* Stop b's driver (kill = don't wait for it to return by itself) and
 * forget it. True if it ended cleanly (exit 0, unless `excused`: a KILL or
 * a REBIND ended it) and its job is empty. */
static bool unbind(struct binding *b, bool kill, bool excused)
{
    if (!b->proc)
        return true;
    jam_handle_close(b->client);   /* a serving driver sees its client gone */
    b->client = HANDLE_INVALID;
    signals_t seen;
    if (kill)
        kill_driver(b);
    status_t st = jam_object_wait_one(b->proc, SIG_TERMINATED,
                                      (uint64_t)jam_clock_get() + STOP_WAIT, &seen);
    bool ok = st == OK;
    if (st != OK) {
        say(true, "devmgr: %s %s did not stop in %lu s: killing its job", bdf(b), b->path,
            (unsigned long)(STOP_WAIT / S));
        kill_driver(b);
    }
    struct process_info info;
    if (jam_process_get_info(b->proc, &info) != OK || info.state != PROCESS_DEAD)
        ok = false;
    else if (!excused && (info.killed || info.exit_code))
        ok = false;
    ok &= job_empty(b->job, b->path);
    jam_port_unbind(port, b->proc, KEY_DRIVER + (uint64_t)(b - devs));
    jam_handle_close(b->proc);
    jam_handle_close(b->job);
    b->proc = b->job = HANDLE_INVALID;
    b->running = false;
    return ok;
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
        b->last = bind(b);
        if (b->last != OK) {
            say(true, "devmgr: %s %04x:%04x -> %s: bind FAILED (%s)", bdf(b), b->info.vendor,
                b->info.device, b->path, status_str(b->last));
            nfailed++;
            continue;
        }
        nbound++;
        unsigned bars = 0;
        for (unsigned k = 0; k < 6; k++)
            bars += is_mem_bar(&b->info, k);
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
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        bool hit = msix_wildcard && q->vendor == 0xffff && q->device == 0xffff
                       ? b->info.msix_vectors &&
                             !(b->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
                       : b->info.vendor == q->vendor && b->info.device == q->device;
        if (hit && seen++ == q->instance)
            return b;
    }
    return NULL;
}

/* Handle one request; the reply (and *nh handles in hs) to send back. */
static void handle(const struct devmgr_req *q, struct devmgr_rep *r, handle_t *hs, uint32_t *nh)
{
    *nh = 0;
    r->status = OK;
    if (q->ordinal == DEVMGR_STATUS) {
        r->a = nbound;
        r->b = nfailed;
        r->c = nskipped;
        return;
    }
    struct binding *b = find(q, q->ordinal == DEVMGR_DRIVER_VIEW);
    bool bound = b && b->proc;
    switch (q->ordinal) {
    case DEVMGR_GET_SERVICE:
        if (!bound)
            r->status = ERR_NOT_FOUND;
        else if (!b->running)
            r->status = ERR_BAD_STATE;
        else if ((r->status = jam_handle_duplicate(b->client, RIGHT_SAME, &hs[0])) == OK)
            *nh = 1;
        return;
    case DEVMGR_GET_DRIVER:
        if (!bound) {
            r->status = ERR_NOT_FOUND;
            return;
        }
        r->a = b->index;
        if ((r->status = jam_handle_duplicate(b->proc, RIGHTS_BASIC, &hs[0])) == OK &&
            (r->status = jam_handle_duplicate(b->job, RIGHTS_BASIC, &hs[1])) == OK &&
            (r->status = jam_handle_duplicate(b->dev, RIGHTS_BASIC | RIGHT_READ, &hs[2])) == OK)
            *nh = 3;
        break;
    case DEVMGR_KILL:
        if (!bound)
            r->status = ERR_NOT_FOUND;
        else if (b->running) {
            kill_driver(b);
            signals_t seen;
            r->status = jam_object_wait_one(b->proc, SIG_TERMINATED,
                                            (uint64_t)jam_clock_get() + STOP_WAIT, &seen);
            b->killed = true;
            reaped(b);
        }
        return;
    case DEVMGR_REBIND:
        if (!b || !b->path) {
            r->status = ERR_NOT_FOUND;
            return;
        }
        unbind(b, true, true);
        r->status = b->last = bind(b);
        say(false, "devmgr: %s %s bound again (%s)", bdf(b), b->path, status_str(r->status));
        return;
    case DEVMGR_DRIVER_VIEW: {
        if (!b) {
            r->status = ERR_NOT_FOUND;
            return;
        }
        if (!b->dev && (r->status = jam_pci_device_open(pci_res, b->index, &b->dev)) != OK)
            return;
        handle_t dev = b->dev;
        r->a = 0;
        if ((r->status = narrowed(dev, DEVMGR_DRV_DEV_RIGHTS, &hs[0])) != OK)
            return;
        *nh = 1;
        for (unsigned i = 0; i < 6; i++) {
            if (!is_mem_bar(&b->info, i))
                continue;
            if ((r->status = bar_for_driver(dev, i, &hs[*nh])) != OK)
                break;
            r->a |= 1u << i;
            (*nh)++;
        }
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
        struct devmgr_rep r = { ((struct devmgr_req *)buf)->txid, ERR_INVALID_ARGS, 0, 0, 0 };
        handle_t hs[DEVMGR_MAX_HANDLES];
        uint32_t nout = 0;
        if (n == sizeof(struct devmgr_req) && !nh)
            handle((struct devmgr_req *)buf, &r, hs, &nout);
        uint32_t rn = r.status == OK ? sizeof(r) : DEVMGR_REP_HDR;
        if (jam_channel_write(ch, &r, rn, hs, nout) != OK)
            for (uint32_t i = 0; i < nout; i++)
                jam_handle_close(hs[i]);   /* the client is gone */
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
        b->index = i;
        b->path = match(&b->info);
        ndevs++;
    }
    bind_all();
    say(false, "devmgr: %u function(s), %u driver(s) bound, %u failed, %u skipped; serving",
        ndevs, nbound, nfailed, nskipped);

    bool ok = nfailed == 0, armed = false;
    for (;;) {
        st = serve(ch);
        if (st != ERR_SHOULD_WAIT)
            break;
        /* ONCE, re-armed after it fires (it fires at once if a message came
         * in meanwhile); a driver's death arrives on the same port. */
        if (!armed) {
            st = jam_port_bind(port, ch, KEY_CHANNEL, SIG_READABLE | SIG_PEER_CLOSED,
                               PORT_BIND_ONCE);
            if (st != OK)
                break;
            armed = true;
        }
        struct port_packet pkt;
        st = jam_port_wait(port, DEADLINE_NEVER, &pkt);
        if (st != OK)
            break;
        if (pkt.key == KEY_CHANNEL)
            armed = false;
        else if (pkt.key >= KEY_DRIVER && pkt.key < KEY_DRIVER + ndevs)
            reaped(&devs[pkt.key - KEY_DRIVER]);
    }
    if (st != ERR_PEER_CLOSED) {
        say(true, "devmgr: serving failed (%s)", status_str(st));
        ok = false;
    }
    /* Every client is gone: stop the drivers. */
    unsigned stopped = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (!b->proc)
            continue;
        /* One a test killed (and didn't rebind) only has to be clean; one
         * that exited or crashed by itself before now must have exited 0
         * (review of M6 phase 2: an xhci-noop failure on a plain boot used
         * to end in "no problems"). */
        if (!unbind(b, !b->running, b->killed)) {
            say(true, "devmgr: %s %s did not end cleanly", bdf(b), b->path);
            ok = false;
        }
        stopped++;
    }
    for (unsigned i = 0; i < ndevs; i++)
        if (devs[i].dev)
            jam_handle_close(devs[i].dev);
    say(false, "devmgr: %u driver(s) stopped; exiting", stopped);
    return ok ? 0 : 1;
}
