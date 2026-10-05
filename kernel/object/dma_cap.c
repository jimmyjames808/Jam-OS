/* DMA capabilities: the permission to pin memory for device DMA
 * (vmo_pin), and, when the IOMMU translates, what the device can reach.
 *
 * A cap is bound to one PCI function (dma_cap_create_for; the
 * dma_cap_create system call needs its RES_PCI_DEV). Every pin made with a
 * cap is on the cap's `pins` list (vmo.c links and unlinks it; lock order:
 * the cap's object lock, then the VMO's).
 *
 * The IOMMU (<jam/iommu.h>, the boot word iommu=on): a bound cap of a
 * function a VT-d unit translates gets a DOMAIN of its own when it is made
 * (an empty page table, charged to the cap's job, with the function's
 * RMRRs mapped), and the function's context entry names it from then on.
 * vmo_pin maps each pinned page there at its own address, and vmo_unpin
 * unmaps it and waits for the unit's invalidation before the pages may go
 * (vmo.c): the device reaches only what its current cap has pinned. With
 * iommu=off or no VT-d, caps have no domain: the device can reach all of
 * RAM, and Bus Master Enable and the quarantine below are the protection.
 *
 * Safe rebind: a function has at most one CURRENT cap, the last one made
 * for it (`owner`, its koid, under the command-filter lock). Making a cap
 * turns the function's Bus Master Enable off and, with the IOMMU, switches
 * its context entry to the new cap's empty domain (one step under "dma
 * owner"): the new owner (its driver) turns bus mastering on with
 * dma_cap_bus_master only once it has quiesced the device (reset it, or
 * seen its DMA engine idle), so a transfer the previous driver left queued
 * can't land anywhere. Nobody else turns it on: the pci_bus_master system
 * call only turns it off. vmo_pin with a bound cap needs the cap to be
 * current and Bus Master Enable on.
 *
 * Closing the last handle (a process kill closes them the same way) runs
 * where nothing may block (the object teardown, preemption off):
 *   1. `closed` is set, so no new pin can finish (vmo_pin re-checks it
 *      before it publishes the pin);
 *   2. if the cap is still its function's current one, Bus Master Enable
 *      goes off and the command register is read back (flushing the posted
 *      write), under the command-filter lock so no racing filtered config
 *      write can turn it back on; an older cap (its function has a new
 *      owner) leaves it alone;
 *   3. only then the pins made with it go: an unbound cap (kernel tests)
 *      releases them; a bound cap puts them in its BATCH (each page with a
 *      checksum, to see later whether anything wrote it), made with the
 *      cap so the close allocates nothing, for the "dma quarantine" thread:
 *      - with a domain the batch is due at once. The thread (which may
 *        wait) points the function back at its home domain unless a newer
 *        cap has taken it already, waits for pins still being made or
 *        undone with the cap, then destroys the domain, which invalidates
 *        what the unit cached for its domain id; once the unit has
 *        confirmed that, the pages are FREED at once. If it can't confirm
 *        (an invalidation timed out or was refused), the batch keeps its
 *        domain and its pages and is tried again every second (RETRY_NS):
 *        they are never released unconfirmed, since the unit may still
 *        use the domain's mappings;
 *      - without a domain the batch is QUARANTINED: the device may still
 *        hold the pages' addresses in a queued transfer, and Bus Master
 *        Enable comes back on with the next driver. The quarantine keeps
 *        the pages (still charged to their VMO's job) until
 *        DMA_QUARANTINE_GRACE_NS after the function's current cap next
 *        turns bus mastering on (a driver that turned it on without
 *        quiescing the device gets its stale writes into quarantined
 *        pages, not someone else's), or DMA_QUARANTINE_TIMEOUT_NS after
 *        the close if no driver does (memory never stays held forever;
 *        while Bus Master Enable stays off the device reaches nothing
 *        anyway). A clean driver unpins before it exits, so nothing is
 *        quarantined.
 *      At release each page is compared with its checksum: a changed page
 *      means something (the device, if the VMO had no other writer) wrote
 *      it late (logged, counted in the stats).
 * A cap that never had a handle (a kernel test's, a failed create) gives
 * its function and its domain up the same way when it is destroyed. A pin
 * holds a reference on its cap, so the struct outlives the handles until
 * the last pin is gone (a pin in a batch drops it).
 *
 * The batches are per function: a list (one per close), each with its own
 * deadline, under `q_lock`; the "dma quarantine" kernel thread releases
 * due ones. At most one quarantined batch per owner can be waiting:
 * pinning needs the current cap with bus mastering on, and that turning-on
 * starts the older batches' grace.
 *
 * The counters a reader sees (dma_quarantine_stats) move in one step. A
 * batch being released is off the list but still counted in `pins` and
 * `pages` until its pages are back with their VMOs; then, in one q_lock
 * section, they drop and `released` or `freed` (and `changed`) rise. So
 * `pages + released + freed` never dips, and a reader that sees a
 * function's pins at 0 also sees every one of its batches finished: pages
 * given back (and uncharged from their job once their VMO goes) and
 * counted.
 *
 * Locks: "dma owner" (a mutex: a new cap's owner change and domain switch)
 * before the command-filter lock (taken and dropped inside it) and before
 * the IOMMU's own ("vtd context", <jam/iommu.h>); q_lock (a spinlock) is
 * never held across anything that waits. */
#include <jam/dbghook.h>
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#define PCI_COMMAND 0x04
#define CMD_BME     0x04

/* How long the release thread waits for a closed cap's pins still being
 * made or undone (each at most a commit and one invalidation, 100 ms). */
#define SETTLE_NS (10 * NS_PER_S)
/* How often the release thread tries again to take away a domain the unit
 * didn't confirm gone. */
#define RETRY_NS  (1 * NS_PER_S)

struct q_batch {
    struct list_node     node;          /* on dma_fn.batches */
    struct list_node     pins;          /* vmo.c ranges (their cap_node) */
    uint64_t             npins, pages;  /* pins and pages in the batch */
    uint64_t             deadline;      /* uptime_ns() at which it goes (or is tried again) */
    struct iommu_domain *dom;           /* taken away first, then the pages freed; NULL: quarantined */
    struct dma_cap      *cap;           /* with dom: the closed cap (a reference) until its pins in
                                         * flight are done, or NULL */
    bool                 unconfirmed;   /* a try failed (said once); the release thread's */
};

struct dma_fn {
    uint64_t         owner;      /* koid of the current cap, 0: none (cmd lock) */
    bool             init;       /* batches is a list (q_lock) */
    struct list_node batches;    /* struct q_batch waiting (q_lock) */
    uint32_t         releasing;  /* batches taken off the list, not yet released (q_lock) */
    uint64_t         pins, pages;          /* held now, including batches releasing (q_lock) */
    uint64_t         released, freed;      /* totals, pages (q_lock) */
    uint64_t         changed;              /* total pages found changed at release (q_lock) */
};

static struct dma_fn fns[PCI_MAX_DEVS];
static spinlock_t q_lock = SPINLOCK_INIT("dma quarantine");
static struct waitqueue q_wq;
static int q_state;              /* 0 not started, 1 starting, 2 running */
static struct mutex owner_lock;  /* "dma owner"; initialised by dma_quarantine_start */

static struct dma_fn *fn_of(const struct pci_dev *d)
{
    return d && d->index < PCI_MAX_DEVS ? &fns[d->index] : NULL;
}

/* For log lines: "%02x:%02x.%x". */
#define BDF(d) (d)->info.bus, (d)->info.dev, (d)->info.fn

static const char *plural(uint64_t n)
{
    return n == 1 ? "" : "s";
}

/* ---- the batches ---------------------------------------------------------------- */

static void wake_reaper(void)
{
    if (__atomic_load_n(&q_state, __ATOMIC_ACQUIRE) == 2)
        waitqueue_wake_all(&q_wq);
}

/* b onto d's list, its pins and pages counted as held. */
static void add_batch(struct pci_dev *d, struct q_batch *b)
{
    struct dma_fn *fn = fn_of(d);
    uint64_t f = spin_lock_irqsave(&q_lock);
    if (!fn->init) {
        list_init(&fn->batches);
        fn->init = true;
    }
    list_add_tail(&fn->batches, &b->node);
    fn->pins += b->npins;
    fn->pages += b->pages;
    spin_unlock_irqrestore(&q_lock, f);
    wake_reaper();
}

/* Wait (bounded) until no pin is left on c: the ones still being made or
 * undone when it closed finish by themselves, each with its own unmap. */
static bool pins_settled(struct dma_cap *c)
{
    uint64_t deadline = uptime_ns() + SETTLE_NS;
    while (dma_cap_pin_count(&c->base)) {
        if (uptime_ns() > deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

/* b's domain out of the device's reach for good, so b's pages may be
 * freed at once: the function back home unless a newer cap's domain has
 * replaced it already (that switch waited for the same invalidation), the
 * closed cap's pins in flight finished, the domain destroyed (the unit's
 * caches for its id invalidated and waited for: the context entries and
 * translations it tagged, the function's included). False when the unit
 * didn't confirm it: b keeps its domain (and its cap until the pins in
 * flight are done) and its pages, and is tried again RETRY_NS later. The
 * pages are never released before the unit confirms: the domain still
 * maps them, and the unit may still hold the function's old context
 * entry, which a later switch's device-selective invalidation doesn't
 * reach (it names the domain id in memory, VT-d 6.5.1.1), so a timed
 * release could hand the next driver's device freed pages. Only freeing
 * the domain's id invalidates what it tagged. */
static bool take_domain_away(struct pci_dev *d, struct q_batch *b)
{
    status_t st = iommu_detach(b->dom);
    if (st == ERR_BAD_STATE)
        st = OK;   /* not attached: a newer cap's domain replaced it */
    if (st == OK && b->cap) {
        if (!pins_settled(b->cap)) {
            st = ERR_TIMED_OUT;
        } else {
            kobject_unref(&b->cap->base);
            b->cap = NULL;
        }
    }
    if (st == OK)
        st = iommu_domain_destroy(b->dom);
    if (st == OK) {
        if (b->unconfirmed)
            kprintf("dma: %02x:%02x.%x: the IOMMU confirmed a closed dma_cap's domain gone at "
                    "last\n", BDF(d));
        return true;
    }
    if (!b->unconfirmed)
        report("dma: %02x:%02x.%x: the IOMMU did not confirm that a closed dma_cap's domain is "
               "gone (%d): its %lu page%s held, tried again every second", BDF(d), st,
               b->pages, plural(b->pages));
    b->unconfirmed = true;
    return false;
}

/* b (taken off its list) back on it: due again `after` from now. */
static void requeue(struct pci_dev *d, struct q_batch *b, uint64_t after)
{
    struct dma_fn *fn = fn_of(d);
    b->deadline = uptime_ns() + after;
    uint64_t f = spin_lock_irqsave(&q_lock);
    list_add_tail(&fn->batches, &b->node);
    fn->releasing--;
    spin_unlock_irqrestore(&q_lock, f);
    wake_reaper();
}

static void log_release(struct pci_dev *d, const struct q_batch *b, bool freed, uint64_t pages,
                        uint64_t changed, const char *why)
{
    if (changed)
        kprintf("dma: %02x:%02x.%x: %lu held page%s CHANGED after their dma_cap closed (%s)\n",
                BDF(d), changed, plural(changed),
                freed ? "not by the device, which the IOMMU kept out: another holder of the VMO?"
                      : "the device, after a driver turned bus mastering on without quiescing "
                        "it?");
    if (freed && b->npins)
        kprintf("dma: %02x:%02x.%x: %lu pin%s (%lu page%s) of a closed dma_cap freed: the "
                "IOMMU took them from the device\n", BDF(d), b->npins, plural(b->npins), pages,
                plural(pages));
    else if (!freed)
        kprintf("dma: %02x:%02x.%x: quarantine released (%lu pin%s, %lu page%s, %s)\n", BDF(d),
                b->npins, plural(b->npins), pages, plural(pages), why);
}

/* Release a batch taken off its function's list (no lock held; it may
 * wait). Only once its pages are back does it leave the counters, all in
 * one step (see the file header). */
static void release_batch(struct pci_dev *d, struct q_batch *b, const char *why)
{
    bool freed = b->dom != NULL;
    if (freed && !take_domain_away(d, b)) {
        requeue(d, b, RETRY_NS);
        return;
    }
    uint64_t pages = 0, changed = 0;
    vmo_release_quarantined(&b->pins, &pages, &changed);
    DBG_HOOK(DBG_DMA_RELEASED, d);
    struct dma_fn *fn = fn_of(d);
    uint64_t f = spin_lock_irqsave(&q_lock);
    fn->pins -= b->npins;
    fn->pages -= b->pages;
    if (freed)
        fn->freed += pages;
    else
        fn->released += pages;
    fn->changed += changed;
    fn->releasing--;
    spin_unlock_irqrestore(&q_lock, f);
    log_release(d, b, freed, pages, changed, why);
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

/* q_lock held: take a batch whose time is up (*dev gets its function),
 * or NULL; *next gets the earliest deadline of the batches still waiting
 * on the functions looked at. */
static struct q_batch *find_due_locked(uint64_t now, struct pci_dev **dev, uint64_t *next)
{
    for (uint32_t i = 0; i < pci_count() && i < PCI_MAX_DEVS; i++) {
        struct dma_fn *fn = &fns[i];
        if (!fn->init)
            continue;
        struct q_batch *due = take_due_locked(fn, now, false);
        if (due) {
            *dev = pci_get(i);
            return due;
        }
        for (struct list_node *n = fn->batches.next; n != &fn->batches; n = n->next) {
            struct q_batch *b = container_of(n, struct q_batch, node);
            if (b->deadline < *next)
                *next = b->deadline;
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
        struct pci_dev *dev = NULL;
        struct q_batch *due = find_due_locked(now, &dev, &next);
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
                                     __ATOMIC_ACQUIRE)) {
        /* Running, or another CPU is starting it: wait for that, so the
         * owner lock is ready when this returns. */
        while (__atomic_load_n(&q_state, __ATOMIC_ACQUIRE) != 2)
            cpu_relax();
        return;
    }
    mutex_init(&owner_lock, "dma owner");
    waitqueue_init(&q_wq, "dma quarantine wait");
    struct thread *t = thread_create("dma quarantine", reaper_main, NULL, PRIO_DEFAULT);
    thread_detach(t);
    __atomic_store_n(&q_state, 2, __ATOMIC_RELEASE);
}

/* Bus mastering just went on for d through its current cap: every
 * quarantined batch waiting starts its grace period. */
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

/* The close path of a bound cap: its remaining pins into its batch, due
 * at once to have its domain taken away, or quarantined without one. */
static void close_batch(struct dma_cap *c)
{
    struct q_batch *b = c->batch;
    c->batch = NULL;
    list_init(&b->pins);
    vmo_quarantine_cap_pins(c, &b->pins, &b->npins, &b->pages);
    if (c->dom) {
        kobject_ref(&c->base);   /* for the pins still in flight: see take_domain_away */
        b->cap = c;
        b->dom = c->dom;
        b->deadline = 0;
        add_batch(c->dev, b);
        return;
    }
    if (!b->npins) {
        kfree(b);   /* a clean close */
        return;
    }
    b->deadline = uptime_ns() + DMA_QUARANTINE_TIMEOUT_NS;
    add_batch(c->dev, b);
    kprintf("dma: %02x:%02x.%x: dma_cap closed with %lu pin%s (%lu page%s) still held: "
            "quarantined\n", BDF(c->dev), b->npins, plural(b->npins), b->pages,
            plural(b->pages));
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
    out->freed = fn->freed;
    out->changed = fn->changed;
    spin_unlock_irqrestore(&q_lock, f);
}

void dma_quarantine_flush(struct pci_dev *d)
{
    struct dma_fn *fn = fn_of(d);
    if (!fn)
        return;
    /* Each batch listed now is tried once: one whose domain the unit
     * doesn't confirm gone goes back on the list, held. */
    uint32_t left = 0;
    uint64_t lf = spin_lock_irqsave(&q_lock);
    if (fn->init)
        for (struct list_node *n = fn->batches.next; n != &fn->batches; n = n->next)
            left++;
    spin_unlock_irqrestore(&q_lock, lf);
    for (;;) {
        uint64_t f = spin_lock_irqsave(&q_lock);
        struct q_batch *b = left ? take_due_locked(fn, 0, true) : NULL;
        bool busy = fn->releasing != 0;
        spin_unlock_irqrestore(&q_lock, f);
        if (b) {
            left--;
            release_batch(d, b, "flushed");
        }
        else if (busy)
            thread_sleep_ms(1);   /* the reaper is releasing one: done in a moment */
        else
            return;
    }
}

/* ---- the cap -------------------------------------------------------------------- */

/* If c is still its function's current cap: nobody's now, Bus Master
 * Enable off, the write landed (read back). */
static void give_up(struct dma_cap *c)
{
    struct dma_fn *fn = fn_of(c->dev);
    uint64_t f = pci_cmd_lock();
    if (fn->owner == c->base.koid) {
        fn->owner = 0;
        pci_set_bus_master(c->dev, false);
        (void)pci_cfg_read(c->dev, PCI_COMMAND, 2);   /* read back: the write has landed */
    }
    pci_cmd_unlock(f);
}

static void dma_cap_destroy(struct kobject *obj)
{
    struct dma_cap *c = (struct dma_cap *)obj;
    if (c->batch) {
        /* It never had a handle, so no close ran (and no pin is left: a
         * pin holds a reference). */
        give_up(c);
        if (c->dom) {
            list_init(&c->batch->pins);
            c->batch->dom = c->dom;
            add_batch(c->dev, c->batch);   /* due at once: deadline 0 */
        } else {
            kfree(c->batch);
        }
    }
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
    give_up(c);
    close_batch(c);
}

static const struct kobject_ops dma_cap_ops = {
    .name = "dma_cap",
    .destroy = dma_cap_destroy,
    .on_zero_handles = dma_cap_zero_handles,
};

/* A cap for d (NULL: unbound), charged to job (may be NULL); a bound one
 * gets its batch now. */
static status_t cap_new(struct pci_dev *d, struct job *job, struct kobject **out)
{
    struct dma_cap *c = kzalloc(sizeof(*c));
    struct q_batch *b = d ? kzalloc(sizeof(*b)) : NULL;
    status_t st = !c || (d && !b) ? ERR_NO_MEMORY : job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st != OK) {
        kfree(b);
        kfree(c);
        return st;
    }
    kobject_init(&c->base, OBJ_DMA_CAP, &dma_cap_ops, "dma_cap", 0);
    c->dev = d;
    c->batch = b;
    list_init(&c->pins);
    job_ref(job);
    c->job = job;
    *out = &c->base;
    return OK;
}

status_t dma_cap_create(struct kobject **out)
{
    return cap_new(NULL, NULL, out);
}

/* c becomes its function's current cap: Bus Master Enable off, and with a
 * domain the function's context entry names it (the previous cap's
 * domain out of the device's reach, the invalidation waited for). One
 * step under "dma owner", so two caps made at once can't end with one the
 * owner and the other's domain attached. */
static status_t take_over(struct dma_cap *c)
{
    mutex_lock(&owner_lock);
    uint64_t f = pci_cmd_lock();
    fn_of(c->dev)->owner = c->base.koid;
    pci_set_bus_master(c->dev, false);
    pci_cmd_unlock(f);
    status_t st = c->dom ? iommu_attach(c->dom) : OK;
    mutex_unlock(&owner_lock);
    return st;
}

status_t dma_cap_create_for(struct pci_dev *d, struct job *job, struct kobject **out)
{
    if (!d || !fn_of(d))
        return ERR_INVALID_ARGS;
    /* The kernel never writes their command register, so it couldn't take
     * Bus Master Enable away again. */
    if (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return ERR_ACCESS_DENIED;
    dma_quarantine_start();
    struct kobject *obj;
    status_t st = cap_new(d, job, &obj);
    if (st != OK)
        return st;
    struct dma_cap *c = (struct dma_cap *)obj;
    st = iommu_domain_create(d, job, &c->dom);
    if (st == ERR_NOT_SUPPORTED)
        st = OK;   /* not translated: no domain, the quarantine protects */
    if (st == OK)
        st = take_over(c);
    if (st != OK) {
        kobject_unref(obj);   /* its destroy gives the function and the domain up */
        return st;
    }
    *out = obj;
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

bool dma_cap_translated(struct kobject *cap)
{
    struct dma_cap *c = dma_cap_from_kobject(cap);
    return c && c->dom;
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
