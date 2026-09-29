/* DMA capabilities: the permission to pin memory for device DMA
 * (vmo_pin). The IOMMU domain will hang off it once VT-d/AMD-Vi arrive.
 *
 * A cap is bound to one PCI function (dma_cap_create_for; the
 * dma_cap_create system call needs its RES_PCI_DEV). Every pin made with a
 * cap is on the cap's `pins` list (vmo.c links and unlinks it; lock order:
 * the cap's object lock, then the VMO's).
 *
 * Safe rebind: a function has at
 * most one CURRENT cap, the last one made for it (`owner`, its koid, under
 * the command-filter lock). Making a cap turns the function's Bus Master
 * Enable off: the new owner (its driver) turns it on with
 * dma_cap_bus_master only once it has quiesced the device (reset it, or
 * seen its DMA engine idle), so a transfer the previous driver left queued
 * can't land anywhere. Nobody else turns it on: the pci_bus_master system
 * call only turns it off. vmo_pin with a bound cap needs the cap to be
 * current and Bus Master Enable on.
 *
 * Closing the last handle (a process kill closes them the same way):
 *   1. `closed` is set, so no new pin can finish (vmo_pin re-checks it
 *      before it publishes the pin);
 *   2. if the cap is still its function's current one, Bus Master Enable
 *      goes off and the command register is read back (flushing the posted
 *      write), under the command-filter lock so no racing filtered config
 *      write can turn it back on; an older cap (its function has a new
 *      owner) leaves it alone;
 *   3. only then the pins made with it go: an unbound cap (kernel tests)
 *      releases them; a bound cap QUARANTINES them. The device may still
 *      hold their addresses in a queued transfer, and Bus Master Enable
 *      comes back on with the next driver. The quarantine keeps the pages
 *      (still charged to their VMO's job) until DMA_QUARANTINE_GRACE_NS
 *      after the function's current cap next turns bus mastering on (a
 *      driver that turned it on without quiescing the device gets its
 *      stale writes into quarantined pages, not someone else's), or
 *      DMA_QUARANTINE_TIMEOUT_NS after the close if no driver does (memory
 *      never stays held forever; while Bus Master Enable stays off the
 *      device reaches nothing anyway). A clean driver unpins before it
 *      exits, so nothing is quarantined. At release each page is compared
 *      with a checksum taken at quarantine: a changed page means the
 *      device wrote it late (logged, counted in the stats).
 * A pin holds a reference on its cap, so the struct outlives the handles
 * until the last pin is gone (a quarantined pin drops it).
 *
 * The quarantine is per function: a list of batches (one per unclean
 * close), each with its own deadline, under `q_lock`. The "dma quarantine"
 * kernel thread releases due batches. At most one batch per owner can be
 * waiting: pinning needs the current cap with bus mastering on, and that
 * turning-on starts the older batches' grace.
 *
 * The counters a reader sees (dma_quarantine_stats) move in one step. A
 * batch being released is off the list but still counted in `pins` and
 * `pages` until its pages are back with their VMOs; then, in one q_lock
 * section, they drop and `released` / `changed` rise. So `pages +
 * released` never dips, and a reader that sees a function's pins at 0 also
 * sees every one of its batches finished: pages given back (and uncharged
 * from their job once their VMO goes) and counted as released. */
#include <jam/dbghook.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PCI_COMMAND 0x04
#define CMD_BME     0x04

struct q_batch {
    struct list_node node;          /* on dma_fn.batches */
    struct list_node pins;          /* vmo.c ranges (their cap_node) */
    uint64_t         npins, pages;  /* pins and pages in the batch */
    uint64_t         deadline;      /* uptime_ns() at which it goes */
};

struct dma_fn {
    uint64_t         owner;      /* koid of the current cap, 0: none (cmd lock) */
    bool             init;       /* batches is a list (q_lock) */
    struct list_node batches;    /* struct q_batch waiting (q_lock) */
    uint32_t         releasing;  /* batches taken off the list, not yet released (q_lock) */
    uint64_t         pins, pages;          /* held now, including batches releasing (q_lock) */
    uint64_t         released, changed;    /* totals, pages (q_lock) */
};

static struct dma_fn fns[PCI_MAX_DEVS];
static spinlock_t q_lock = SPINLOCK_INIT("dma quarantine");
static struct waitqueue q_wq;
static int q_state;            /* 0 not started, 1 starting, 2 running */

static struct dma_fn *fn_of(const struct pci_dev *d)
{
    return d && d->index < PCI_MAX_DEVS ? &fns[d->index] : NULL;
}

/* For log lines: "%02x:%02x.%x". */
#define BDF(d) (d)->info.bus, (d)->info.dev, (d)->info.fn

/* ---- the quarantine ------------------------------------------------------------ */

static void wake_reaper(void)
{
    if (__atomic_load_n(&q_state, __ATOMIC_ACQUIRE) == 2)
        waitqueue_wake_all(&q_wq);
}

/* Release a batch taken off its function's list (no lock held). Only
 * once its pages are back does it leave the counters, all in one step
 * (see the file header). */
static void release_batch(struct pci_dev *d, struct q_batch *b, const char *why)
{
    uint64_t pages = 0, changed = 0;
    vmo_release_quarantined(&b->pins, &pages, &changed);
    DBG_HOOK(DBG_DMA_RELEASED, d);
    struct dma_fn *fn = fn_of(d);
    uint64_t f = spin_lock_irqsave(&q_lock);
    fn->pins -= b->npins;
    fn->pages -= b->pages;
    fn->released += pages;
    fn->changed += changed;
    fn->releasing--;
    spin_unlock_irqrestore(&q_lock, f);
    if (changed)
        kprintf("dma: %02x:%02x.%x: %lu quarantined page%s CHANGED while held: "
                "the device wrote them after its dma_cap closed (a driver turned bus "
                "mastering on without quiescing it?)\n", BDF(d), changed,
                changed == 1 ? "" : "s");
    kprintf("dma: %02x:%02x.%x: quarantine released (%lu pin%s, %lu page%s, %s)\n", BDF(d),
            b->npins, b->npins == 1 ? "" : "s", pages, pages == 1 ? "" : "s", why);
    kfree(b);
}

/* With q_lock held: take fn's first batch due at `now` (all of them if
 * `all`) off its list, for release_batch. It stays in the counters until
 * then. */
static struct q_batch *take_due_locked(struct dma_fn *fn, uint64_t now, bool all)
{
    if (!fn->init)
        return NULL;
    for (struct list_node *n = fn->batches.next; n != &fn->batches; n = n->next) {
        struct q_batch *b = container_of(n, struct q_batch, node);
        if (all || b->deadline <= now) {
            list_del(&b->node);
            fn->releasing++;
            return b;
        }
    }
    return NULL;
}

static void reaper_main(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&q_lock);
    for (;;) {
        uint64_t now = uptime_ns(), next = DEADLINE_NEVER;
        struct q_batch *due = NULL;
        struct pci_dev *dev = NULL;
        for (uint32_t i = 0; i < pci_count() && i < PCI_MAX_DEVS && !due; i++) {
            struct dma_fn *fn = &fns[i];
            if (!fn->init)
                continue;
            if ((due = take_due_locked(fn, now, false))) {
                dev = pci_get(i);
                break;
            }
            for (struct list_node *n = fn->batches.next; n != &fn->batches; n = n->next) {
                struct q_batch *b = container_of(n, struct q_batch, node);
                if (b->deadline < next)
                    next = b->deadline;
            }
        }
        if (due) {
            spin_unlock_irqrestore(&q_lock, f);
            release_batch(dev, due, "its time was up");
            f = spin_lock_irqsave(&q_lock);
            continue;
        }
        if (next == DEADLINE_NEVER)
            waitqueue_wait(&q_wq, &q_lock, &f);
        else
            waitqueue_wait_until(&q_wq, &q_lock, &f, next);
    }
}

void dma_quarantine_start(void)
{
    int expect = 0;
    if (!__atomic_compare_exchange_n(&q_state, &expect, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;   /* running, or another CPU is starting it */
    waitqueue_init(&q_wq, "dma quarantine wait");
    struct thread *t = thread_create("dma quarantine", reaper_main, NULL, PRIO_DEFAULT);
    thread_detach(t);
    __atomic_store_n(&q_state, 2, __ATOMIC_RELEASE);
}

/* Bus mastering just went on for d through its current cap: every batch
 * waiting starts its grace period. */
static void start_grace(const struct pci_dev *d)
{
    struct dma_fn *fn = fn_of(d);
    uint64_t until = uptime_ns() + DMA_QUARANTINE_GRACE_NS;
    bool any = false;
    uint64_t f = spin_lock_irqsave(&q_lock);
    if (fn->init)
        for (struct list_node *n = fn->batches.next; n != &fn->batches; n = n->next) {
            struct q_batch *b = container_of(n, struct q_batch, node);
            if (b->deadline > until) {
                b->deadline = until;
                any = true;
            }
        }
    spin_unlock_irqrestore(&q_lock, f);
    if (any)
        wake_reaper();
}

/* The close path of a bound cap: its remaining pins into a new batch. */
static void quarantine(struct dma_cap *c)
{
    struct q_batch *b = kzalloc(sizeof(*b));
    if (!b) {
        /* No memory for the bookkeeping: Bus Master Enable is off, so
         * release at once, as before the quarantine existed. */
        vmo_release_cap_pins(c);
        return;
    }
    list_init(&b->pins);
    vmo_quarantine_cap_pins(c, &b->pins, &b->npins, &b->pages);
    if (!b->npins) {
        kfree(b);   /* a clean close */
        return;
    }
    b->deadline = uptime_ns() + DMA_QUARANTINE_TIMEOUT_NS;
    struct dma_fn *fn = fn_of(c->dev);
    uint64_t f = spin_lock_irqsave(&q_lock);
    if (!fn->init) {
        list_init(&fn->batches);
        fn->init = true;
    }
    list_add_tail(&fn->batches, &b->node);
    fn->pins += b->npins;
    fn->pages += b->pages;
    spin_unlock_irqrestore(&q_lock, f);
    kprintf("dma: %02x:%02x.%x: dma_cap closed with %lu pin%s (%lu page%s) still held: "
            "quarantined\n", BDF(c->dev), b->npins, b->npins == 1 ? "" : "s", b->pages,
            b->pages == 1 ? "" : "s");
    wake_reaper();
}

void dma_quarantine_stats(struct pci_dev *d, struct dma_quarantine_stats *out)
{
    struct dma_fn *fn = fn_of(d);
    *out = (struct dma_quarantine_stats){ 0 };
    if (!fn)
        return;
    uint64_t f = spin_lock_irqsave(&q_lock);
    out->pins = fn->pins;
    out->pages = fn->pages;
    out->released = fn->released;
    out->changed = fn->changed;
    spin_unlock_irqrestore(&q_lock, f);
}

void dma_quarantine_flush(struct pci_dev *d)
{
    struct dma_fn *fn = fn_of(d);
    if (!fn)
        return;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&q_lock);
        struct q_batch *b = take_due_locked(fn, 0, true);
        bool busy = fn->releasing != 0;
        spin_unlock_irqrestore(&q_lock, f);
        if (b)
            release_batch(d, b, "flushed");
        else if (busy)
            thread_sleep_ms(1);   /* the reaper is releasing one: done in a moment */
        else
            return;
    }
}

/* ---- the cap -------------------------------------------------------------------- */

static void dma_cap_destroy(struct kobject *obj)
{
    struct dma_cap *c = (struct dma_cap *)obj;
    job_uncharge(c->job, JOB_LIMIT_HANDLES, 1);
    job_unref(c->job);
    kfree(c);
}

static void dma_cap_zero_handles(struct kobject *obj)
{
    struct dma_cap *c = (struct dma_cap *)obj;
    uint64_t f = spin_lock_irqsave(&obj->lock);
    c->closed = true;
    spin_unlock_irqrestore(&obj->lock, f);
    if (!c->dev) {
        vmo_release_cap_pins(c);
        return;
    }
    struct dma_fn *fn = fn_of(c->dev);
    f = pci_cmd_lock();
    if (fn->owner == obj->koid) {
        fn->owner = 0;
        pci_set_bus_master(c->dev, false);
        (void)pci_cfg_read(c->dev, PCI_COMMAND, 2);   /* read back: the write has landed */
    }
    pci_cmd_unlock(f);
    quarantine(c);
}

static const struct kobject_ops dma_cap_ops = {
    .name = "dma_cap",
    .destroy = dma_cap_destroy,
    .on_zero_handles = dma_cap_zero_handles,
};

static status_t cap_new(struct pci_dev *d, struct kobject **out)
{
    struct dma_cap *c = kzalloc(sizeof(*c));
    if (!c)
        return ERR_NO_MEMORY;
    kobject_init(&c->base, OBJ_DMA_CAP, &dma_cap_ops, "dma_cap", 0);
    c->dev = d;
    list_init(&c->pins);
    *out = &c->base;
    return OK;
}

status_t dma_cap_create(struct kobject **out)
{
    return cap_new(NULL, out);
}

status_t dma_cap_create_for(struct pci_dev *d, struct kobject **out)
{
    if (!d || !fn_of(d))
        return ERR_INVALID_ARGS;
    /* The kernel never writes their command register, so it couldn't take
     * Bus Master Enable away again. */
    if (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return ERR_ACCESS_DENIED;
    dma_quarantine_start();
    status_t st = cap_new(d, out);
    if (st != OK)
        return st;
    /* The function's new owner, starting with bus mastering off. */
    uint64_t f = pci_cmd_lock();
    fn_of(d)->owner = (*out)->koid;
    pci_set_bus_master(d, false);
    pci_cmd_unlock(f);
    return OK;
}

struct pci_dev *dma_cap_device(struct kobject *cap)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    return c ? c->dev : NULL;
}

status_t dma_cap_set_job(struct kobject *cap, struct job *job)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    if (!c || c->job)
        return ERR_BAD_STATE;
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st == OK) {
        job_ref(job);
        c->job = job;
    }
    return st;
}

uint64_t dma_cap_pin_count(struct kobject *cap)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    if (!c)
        return 0;
    uint64_t n = 0;
    uint64_t f = spin_lock_irqsave(&cap->lock);
    for (struct list_node *p = c->pins.next; p != &c->pins; p = p->next)
        n++;
    spin_unlock_irqrestore(&cap->lock, f);
    return n;
}

bool dma_cap_bus_master_on(struct kobject *cap)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    if (!c || !c->dev)
        return true;   /* unbound (kernel tests): no device to ask */
    if (__atomic_load_n(&fn_of(c->dev)->owner, __ATOMIC_RELAXED) != cap->koid)
        return false;  /* its function has a newer cap (or it closed) */
    uint32_t cmd = pci_cfg_read(c->dev, PCI_COMMAND, 2);
    return cmd != 0xffff && cmd != 0xffffffffu && (cmd & CMD_BME);   /* all ones: gone */
}

status_t dma_cap_bus_master(struct kobject *cap, bool on)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    if (!c)
        return ERR_WRONG_TYPE;
    if (!c->dev)
        return ERR_NOT_SUPPORTED;
    uint64_t f = pci_cmd_lock();
    status_t st = fn_of(c->dev)->owner == cap->koid ? pci_set_bus_master(c->dev, on)
                                                   : ERR_BAD_STATE;
    pci_cmd_unlock(f);
    if (st == OK && on)
        start_grace(c->dev);
    return st;
}
