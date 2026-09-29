/* devmgr: starting and stopping a driver (see main.c for what a driver
 * gets, internal.h for the calls). A start makes everything from scratch:
 * that is also the safe-rebind path a restart takes. For a PCI function:
 *   - woken to D0 if it was left in D1-D3 (the kernel waits out the
 *     transition and puts back what a reset lost);
 *   - a new dma_cap: the function's new current cap, which turns Bus
 *     Master Enable off; the new driver turns it on only once it has
 *     quiesced the device, and whatever the dead driver still had pinned
 *     stays quarantined by the kernel until then;
 *   - a new interrupt object (the dead driver's vector went with it) and
 *     new BAR resources.
 * The hardware handles are handed over without RIGHT_DUPLICATE and
 * RIGHT_TRANSFER. */
#include "internal.h"

/* Per-driver job limits: 16 MiB, 256 handles, 16 threads, 1 MiB queued. */
static const struct { uint32_t kind; uint64_t value; } limits[] = {
    { JOB_LIMIT_PAGES, 4096 },
    { JOB_LIMIT_HANDLES, 256 },
    { JOB_LIMIT_THREADS, 16 },
    { JOB_LIMIT_MSG_BYTES, 1u << 20 },
};

static bool is_mem_bar(const struct pci_dev_info *i, unsigned n)
{
    uint32_t f = i->bar[n].flags;
    return i->bar[n].size && (f & PCI_BAR_MMIO) && !(f & PCI_BAR_UNSIZED);
}

/* A driver's copy of one of our handles, with fewer rights, plus
 * RIGHT_TRANSFER for the one hand-over (channel_write_rights drops it on
 * the way: the driver's copy has exactly `rights`). */
static status_t narrowed(handle_t h, rights_t rights, handle_t *out)
{
    return jam_handle_duplicate(h, rights | RIGHT_TRANSFER, out);
}

/* A BAR resource with a driver's rights (made from our device handle, it
 * starts with ours), plus RIGHT_TRANSFER as above. */
static status_t bar_for_driver(handle_t dev, unsigned n, handle_t *out)
{
    handle_t bar;
    status_t st = jam_pci_bar_resource(dev, n, &bar);
    if (st != OK)
        return st;
    st = jam_handle_replace(bar, DEVMGR_DRV_BAR_RIGHTS | RIGHT_TRANSFER, out);
    if (st != OK)
        jam_handle_close(bar);
    return st;
}

/* A function left in D1-D3 (by firmware, or a power-down) is woken to D0
 * before a driver gets it (a driver can't use a sleeping device, and only
 * we may change its power state: RIGHT_MANAGE). The kernel waits out the
 * transition (10 ms) and puts back the BARs and command register a
 * D3hot -> D0 reset loses. */
static status_t wake(struct binding *b)
{
    uint32_t pm = pci_find_cap(b->dev, 0x01), pmcsr = 0;
    if (!pm || jam_pci_config_read(b->dev, pm + 4, 2, &pmcsr) != OK || !(pmcsr & 3))
        return OK;
    uint32_t was = pmcsr & 3;
    status_t st = jam_pci_config_write(b->dev, pm + 4, 2, pmcsr & ~0x8003u);   /* (15: PME status, W1C) */
    if (st == OK && jam_pci_config_read(b->dev, pm + 4, 2, &pmcsr) == OK && (pmcsr & 3))
        st = ERR_TIMED_OUT;
    say(true, "devmgr: %s %04x:%04x was in D%u: %s", bdf(b), b->info.vendor, b->info.device, was,
        st == OK ? "woken to D0" : "can't wake it");
    return st;
}

/* One more startup handle for a driver: h under driver role `role`,
 * arriving with `rights`. */
static void add(struct spawn_handle *x, rights_t *xr, unsigned *n, uint32_t role, handle_t h,
                rights_t rights)
{
    x[*n] = (struct spawn_handle){ SR_DRIVER(role), h };
    xr[(*n)++] = rights;
}

/* A PCI function's handles for its driver. */
static status_t pci_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    handle_t h;
    status_t st = OK;
    if (!b->dev)
        st = jam_pci_device_open(pci_res, b->index, &b->dev);
    if (st == OK)
        st = wake(b);
    if (st == OK && (st = narrowed(b->dev, DEVMGR_DRV_DEV_RIGHTS, &h)) == OK)
        add(x, xr, n, DR_PCIDEV, h, DEVMGR_DRV_DEV_RIGHTS);
    for (unsigned i = 0; st == OK && i < 6; i++)
        if (is_mem_bar(&b->info, i) && (st = bar_for_driver(b->dev, i, &h)) == OK)
            add(x, xr, n, DR_BAR(i), h, DEVMGR_DRV_BAR_RIGHTS);
    if (st == OK && (b->info.msix_vectors || b->info.msi_vectors)) {
        st = jam_interrupt_create_msi(b->dev, 0, b->info.msix_vectors ? IRQ_MSIX : 0, &h);
        if (st == OK)
            add(x, xr, n, DR_IRQ(0), h, DEVMGR_DRV_IRQ_RIGHTS);
    }
    if (st == OK && (st = jam_dma_cap_create(b->dev, &h)) == OK)   /* bus mastering off now */
        add(x, xr, n, DR_DMA, h, DEVMGR_DRV_DMA_RIGHTS);
    return st;
}

/* DR_SERVE: the end a restart kept (clients may have queued calls on it
 * already), else a new channel whose other end becomes b->client. A USB
 * class driver serves nobody. */
static status_t add_serve(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    status_t st = OK;
    if (!b->serve && b->kind != BIND_USB) {
        handle_t client;
        if ((st = jam_channel_create(&client, &b->serve)) == OK) {
            close_client(b);
            b->client = client;
        }
    }
    if (st == OK && b->kind != BIND_USB) {
        add(x, xr, n, DR_SERVE, b->serve, RIGHT_SAME);
        b->serve = HANDLE_INVALID;
    }
    return st;
}

/* b's driver in job, with the n handles in x (arriving with xr[i]), which
 * spawn consumes whatever happens. */
static status_t spawn_driver(const struct binding *b, handle_t job, const struct spawn_handle *x,
                             const rights_t *xr, unsigned n, handle_t *proc)
{
    /* A USB class driver is named after its interface ("hid-6.1:0"):
     * in the log, in `ps`, for the shell's `kill`. */
    const char *name = b->kind == BIND_USB ? b->name : NULL;
    const char *argv[] = { name ? name : b->path };
    struct spawn_args a = {
        .path = b->path, .name = name, .argc = 1, .argv = argv, .job = job, .extra = x,
        .nextra = n, .extra_rights = xr,
    };
    return spawn(&a, proc);
}

/* What the driver writes on its own (usb-bus: interface_attached): the
 * port watches its client end. */
static void watch_events(struct binding *b)
{
    if (b->client && !b->client_key) {
        uint64_t key = KEY_EV_OF(b - devs, b->gen);
        if (jam_port_bind(port, b->client, key, SIG_READABLE, PORT_BIND_PERSISTENT) == OK)
            b->client_key = key;
        else
            say(false, "devmgr: %s: can't watch its channel for events", bdf(b));
    }
}

status_t start_driver(struct binding *b)
{
    struct spawn_handle x[STARTUP_MAX_HANDLES];
    rights_t xr[STARTUP_MAX_HANDLES];
    unsigned n = 0;
    handle_t job = HANDLE_INVALID, proc = HANDLE_INVALID;
    status_t st = b->kind == BIND_PCI   ? pci_handles(b, x, xr, &n)
                  : b->kind == BIND_USB ? usb_handles(b, x, xr, &n)
                                        : OK;
    if (st == OK)
        st = add_serve(b, x, xr, &n);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    for (unsigned i = 0; st == OK && i < sizeof(limits) / sizeof(limits[0]); i++)
        st = jam_job_set_limit(job, limits[i].kind, limits[i].value);
    if (st == OK) {
        st = spawn_driver(b, job, x, xr, n, &proc);   /* consumes the extras either way */
        n = 0;
    }
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(x[i].h);   /* the dma_cap going turns bus mastering off */
    if (st == OK) {
        b->gen++;
        st = jam_port_bind(port, proc, KEY_OF(b - devs, b->gen), SIG_TERMINATED, PORT_BIND_ONCE);
        if (st != OK)
            jam_handle_close(proc);   /* the job kill below takes the process */
    }
    if (st != OK) {
        if (job) {
            jam_job_kill(job);   /* whatever did start */
            jam_handle_close(job);
        }
        /* The driver's end of the channel went with the extras: its
         * clients see PEER_CLOSED, and the next start makes a new one. */
        close_client(b);
        return st;
    }
    b->job = job;
    b->proc = proc;
    b->killed = false;
    b->state = DEVMGR_SUP_RUNNING;
    watch_events(b);
    return OK;
}

void close_client(struct binding *b)
{
    if (b->client && b->client_key)
        jam_port_unbind(port, b->client, b->client_key);
    b->client_key = 0;
    if (b->client)
        jam_handle_close(b->client);
    b->client = HANDLE_INVALID;
}

bool job_empty(handle_t job, const char *who)
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

/* Its whole job: killing only the process would leave anything it started
 * holding its dma_cap, interrupt object and BARs. Then Bus Master Enable
 * off through our own handle (the dma_cap's close has done it already;
 * this is belt and braces, so no path leaves a dead driver's device
 * mastering the bus). */
void kill_driver(struct binding *b)
{
    if (b->job)
        jam_job_kill(b->job);   /* returns once everything in it is dead */
    if (b->kind == BIND_PCI && b->dev)
        jam_pci_bus_master(b->dev, 0);
}

void forget_driver(struct binding *b)
{
    usb_bus_gone(b);   /* a usb-bus: the interfaces it reported went with it */
    if (b->proc) {
        jam_port_unbind(port, b->proc, KEY_OF(b - devs, b->gen));
        jam_handle_close(b->proc);
    }
    if (b->job)
        jam_handle_close(b->job);
    b->proc = b->job = HANDLE_INVALID;
}

bool stop_driver(struct binding *b, bool kill, bool excused)
{
    if (!b->proc)
        return true;
    close_client(b);   /* a serving driver sees its client gone */
    signals_t seen;
    if (kill)
        kill_driver(b);
    status_t st = jam_object_wait_one(b->proc, SIG_TERMINATED,
                                      now() + STOP_WAIT, &seen);
    bool ok = st == OK;
    if (st != OK) {
        say(true, "devmgr: %s %s did not stop in %lu s: killing its job", bdf(b), b->path,
            (unsigned long)(STOP_WAIT / NS_PER_S));
        kill_driver(b);
    }
    struct process_info info;
    if (jam_process_get_info(b->proc, &info) != OK || info.state != PROCESS_DEAD)
        ok = false;
    else if (!excused && (info.killed || info.exit_code))
        ok = false;
    /* Its job must be empty when it ended by itself; a killed driver's DMA
     * pages may still be in the kernel's quarantine (released a moment
     * after the next driver turns bus mastering on). */
    if (ok && !kill && !info.killed)
        ok = job_empty(b->job, b->path);
    forget_driver(b);
    return ok;
}

status_t driver_view(struct binding *b, handle_t *hs, rights_t *rs, uint32_t *nh, uint32_t *mask)
{
    status_t st = OK;
    *nh = 0;
    *mask = 0;
    if (!b->dev && (st = jam_pci_device_open(pci_res, b->index, &b->dev)) != OK)
        return st;
    if ((st = narrowed(b->dev, DEVMGR_DRV_DEV_RIGHTS, &hs[0])) != OK)
        return st;
    rs[0] = DEVMGR_DRV_DEV_RIGHTS;
    *nh = 1;
    for (unsigned i = 0; i < 6; i++) {
        if (!is_mem_bar(&b->info, i))
            continue;
        if ((st = bar_for_driver(b->dev, i, &hs[*nh])) != OK)
            break;
        rs[*nh] = DEVMGR_DRV_BAR_RIGHTS;
        *mask |= 1u << i;
        (*nh)++;
    }
    return st;
}

unsigned mem_bars(const struct binding *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 6; i++)
        n += is_mem_bar(&b->info, i);
    return n;
}
