/* Interrupt objects (OBJ_INTERRUPT, M6 Track B). See <jam/interrupt.h>.
 *
 * An interrupt object owns one (cpu, vector) from vector_alloc and, for a
 * device, one MSI or MSI-X vector of a PCI function programmed to deliver
 * there. Its handler (fire) runs in interrupt context on that CPU:
 *
 *   - no allocation and no sleeping: it takes the object's lock (irqsave)
 *     and raises SIG_INTERRUPT, which runs the observers under that lock:
 *     a port binding folds the edge into its own preallocated packet
 *     (port.c queue_signal), a waiter is woken (thread_wake);
 *   - every fire is ONE not-matching -> matching edge for the observers,
 *     even while SIG_INTERRUPT is still set from an earlier fire: the bit
 *     is cleared and set again under the lock ("pulse"). So a PERSISTENT
 *     binding counts every fire: fired 3 times before the packet was read
 *     = one packet with count 3, and a fire after the packet was read but
 *     before interrupt_ack queues a new packet instead of being lost;
 *   - MSI-X vectors and maskable MSI are masked at the device from the
 *     first fire until interrupt_ack; the device latches what happens
 *     meanwhile (its pending bit) and sends it when unmasked. Plain MSI
 *     (not maskable) stays live and every fire is counted.
 *
 * interrupt_ack, under the object's lock: clear SIG_INTERRUPT, then unmask.
 * A fire can't slip in between (it needs the same lock); one that arrives
 * after the ack (the device's pending message, or a new edge) raises the
 * signal again and queues a new packet. None is lost.
 *
 * Teardown (last handle closed, or last reference for an object that
 * never had a handle), once: mark it dead under the lock (later fires are
 * only counted), mask/disable it at the device, then vector_free, which
 * returns only when no CPU is still inside fire() for it. The memory goes
 * with the last reference; a fire() that finds the magic gone panics
 * rather than use freed memory.
 *
 * Device bookkeeping: MSI is one vector per function and MSI-X is enabled
 * per function, so live device objects are listed (dev_lock) to refuse a
 * second object for the same vector, MSI together with MSI-X, and to turn
 * MSI-X off when the last of a function's vectors goes.
 *
 * Lock order: "interrupt devices" (dev_lock) -> "pci command" (Track C's
 * pci_cmd_lock, around pci_msi_enable only) -> "pci config" (Track A's
 * leaf); "interrupt" (the object) -> "pci config" (mask in fire/ack) and
 * -> "port" -> "port waiters" -> run queues (observers). The vector
 * allocator's lock is a leaf and is never held with these. Track A's
 * pci_msi_set / pci_msi_enable / pci_msi_mask must therefore not sleep,
 * and pci_msi_mask must be callable from an interrupt handler.
 *
 * Job charge: one JOB_LIMIT_HANDLES unit of the creator's job (like a VMO
 * struct), from creation until the memory is freed. */
#include <jam/interrupt.h>
#include <jam/interrupt_test.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource_impl.h>   /* pci_cmd_lock */
#include <jam/spinlock.h>
#include <jam/x86.h>

#define IRQ_MAGIC      0x49525131u   /* "IRQ1" */
#define IRQ_MAGIC_DEAD 0xdead1a1au

enum irq_kind { IK_VIRTUAL, IK_MSI, IK_MSIX };

struct kinterrupt {
    struct kobject    base;       /* base.lock ("interrupt") guards the flags below */
    volatile uint32_t magic;
    uint8_t           kind;       /* enum irq_kind */
    bool              maskable;   /* masked at the device from fire to ack */
    bool              masked;
    bool              pending;    /* virtual maskable: fired while masked (a PBA bit) */
    bool              dead;       /* teardown started: fires are ignored */
    volatile uint8_t  torn;       /* TORN_*: teardown runs once */
    bool              listed;     /* on dev_irqs (dev_lock) */
    uint8_t           vec;
    uint32_t          cpu;
    struct pci_dev   *dev;
    uint32_t          index;      /* MSI-X table entry, 0 for MSI */
    struct list_node  dev_node;   /* on dev_irqs (dev_lock) */
    struct job       *job;        /* charged one JOB_LIMIT_HANDLES unit (a reference) */
    volatile uint64_t fires;
    volatile uint64_t late;
};

static spinlock_t dev_lock = SPINLOCK_INIT("interrupt devices");
static struct list_node dev_irqs = LIST_INIT(dev_irqs);
static volatile uint64_t live;

static struct kinterrupt *to_irq(struct kobject *obj)
{
    return obj && obj->type == OBJ_INTERRUPT ? container_of(obj, struct kinterrupt, base) : NULL;
}

/* ---- the device side ------------------------------------------------------ */

static void dev_mask(struct kinterrupt *o, bool masked)
{
    if (o->kind != IK_VIRTUAL)
        pci_msi_mask(o->dev, o->kind == IK_MSIX, o->index, masked);
}

/* Program the function to send (o->cpu, o->vec) and turn it on. */
static status_t dev_claim(struct kinterrupt *o)
{
    bool msix = o->kind == IK_MSIX;
    struct pci_dev *d = o->dev;
    uint64_t f = spin_lock_irqsave(&dev_lock);
    uint32_t same = 0;
    for (struct list_node *n = dev_irqs.next; n != &dev_irqs; n = n->next) {
        struct kinterrupt *x = container_of(n, struct kinterrupt, dev_node);
        if (x->dev != d)
            continue;
        if (x->kind != o->kind) {   /* MSI and MSI-X never on together */
            spin_unlock_irqrestore(&dev_lock, f);
            return ERR_BAD_STATE;
        }
        if (x->index == o->index) {
            spin_unlock_irqrestore(&dev_lock, f);
            return ERR_ALREADY_BOUND;
        }
        same++;
    }
    status_t st = OK;
    if (msix)
        pci_msi_mask(d, true, o->index, true);   /* quiet while it changes */
    st = pci_msi_set(d, msix, o->index, msi_address(o->cpu), msi_data(o->vec));
    if (st == OK && same == 0) {
        /* MSI enable also sets INTx Disable in the command register: take
         * Track C's command lock, so a driver's filtered config write (a
         * read-modify-write of the same register under that lock) can't
         * undo it. */
        uint64_t cf = pci_cmd_lock();
        st = pci_msi_enable(d, msix, true);
        pci_cmd_unlock(cf);
    }
    if (st == OK) {
        if (o->maskable)
            pci_msi_mask(d, msix, o->index, false);
        list_add_tail(&dev_irqs, &o->dev_node);
        o->listed = true;
    } else if (msix) {
        pci_msi_mask(d, true, o->index, true);
    }
    spin_unlock_irqrestore(&dev_lock, f);
    return st;
}

/* Stop the function sending it: mask the entry, and turn MSI / MSI-X off
 * when this was the function's last vector. */
static void dev_release(struct kinterrupt *o)
{
    bool msix = o->kind == IK_MSIX;
    struct pci_dev *d = o->dev;
    uint64_t f = spin_lock_irqsave(&dev_lock);
    if (o->listed) {
        list_del(&o->dev_node);
        o->listed = false;
        bool others = false;
        for (struct list_node *n = dev_irqs.next; n != &dev_irqs; n = n->next)
            others |= container_of(n, struct kinterrupt, dev_node)->dev == d;
        if (o->maskable)
            pci_msi_mask(d, msix, o->index, true);
        if (!others) {
            uint64_t cf = pci_cmd_lock();   /* INTx Disable goes back: see dev_claim */
            pci_msi_enable(d, msix, false);
            pci_cmd_unlock(cf);
        }
    }
    spin_unlock_irqrestore(&dev_lock, f);
}

/* ---- firing (interrupt context) -------------------------------------------- */

/* Lock held: count it, mask it, and give the observers one edge. */
static void deliver_locked(struct kinterrupt *o)
{
    o->fires++;
    if (o->maskable && !o->masked) {
        o->masked = true;
        dev_mask(o, true);
    }
    if (o->base.signals & SIG_INTERRUPT)
        kobject_signal_locked(&o->base, SIG_INTERRUPT, 0);
    kobject_signal_locked(&o->base, 0, SIG_INTERRUPT);
}

static void fire(void *ctx)
{
    struct kinterrupt *o = ctx;
    if (o->magic != IRQ_MAGIC)
        panic("interrupt: fired on a freed object (%p, magic %x)", o, o->magic);
    uint64_t f = spin_lock_irqsave(&o->base.lock);
    if (o->dead)
        o->late++;
    else if (o->masked && o->kind == IK_VIRTUAL)
        o->pending = true;   /* what a masked MSI-X vector's pending bit does */
    else
        deliver_locked(o);   /* a real device's message in flight when masked counts too */
    spin_unlock_irqrestore(&o->base.lock, f);
}

void interrupt_fire_virtual(struct kobject *irq)
{
    struct kinterrupt *o = to_irq(irq);
    ASSERT(o && o->kind == IK_VIRTUAL);
    fire(o);
}

status_t interrupt_ack(struct kobject *irq)
{
    struct kinterrupt *o = to_irq(irq);
    if (!o)
        return ERR_WRONG_TYPE;
    uint64_t f = spin_lock_irqsave(&o->base.lock);
    if (o->dead) {
        spin_unlock_irqrestore(&o->base.lock, f);
        return ERR_BAD_STATE;
    }
    kobject_signal_locked(&o->base, SIG_INTERRUPT, 0);
    if (o->masked) {
        o->masked = false;
        dev_mask(o, false);   /* a device with its pending bit set sends it now */
        if (o->pending) {
            o->pending = false;
            deliver_locked(o);
        }
    }
    spin_unlock_irqrestore(&o->base.lock, f);
    return OK;
}

uint64_t interrupt_fire_count(struct kobject *irq)
{
    struct kinterrupt *o = to_irq(irq);
    return o ? __atomic_load_n(&o->fires, __ATOMIC_RELAXED) : 0;
}

/* ---- lifetime --------------------------------------------------------------- */

enum { TORN_NO, TORN_BUSY, TORN_DONE };

/* Once, from whichever of on_zero_handles / destroy comes first. They can
 * run at the same time on two CPUs (object.c td_run: a zero-handles run
 * deferred on a CPU that is draining a cascade, while another CPU drops
 * the last reference), so destroy waits for a teardown in progress
 * elsewhere before it frees the memory (wait = true). */
static void teardown(struct kinterrupt *o, bool wait)
{
    uint8_t st = TORN_NO;
    if (!__atomic_compare_exchange_n(&o->torn, &st, TORN_BUSY, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        while (wait && __atomic_load_n(&o->torn, __ATOMIC_ACQUIRE) != TORN_DONE)
            cpu_relax();
        return;
    }
    uint64_t f = spin_lock_irqsave(&o->base.lock);
    o->dead = true;
    o->masked = false;
    o->pending = false;
    spin_unlock_irqrestore(&o->base.lock, f);
    if (o->kind != IK_VIRTUAL)
        dev_release(o);
    vector_free(o->cpu, o->vec);   /* no CPU is in fire() for it after this */
    __atomic_store_n(&o->torn, TORN_DONE, __ATOMIC_RELEASE);
}

static void interrupt_zero_handles(struct kobject *obj)
{
    teardown(container_of(obj, struct kinterrupt, base), false);   /* nobody can ack it now */
}

static void interrupt_destroy(struct kobject *obj)
{
    struct kinterrupt *o = container_of(obj, struct kinterrupt, base);
    teardown(o, true);
    o->magic = IRQ_MAGIC_DEAD;
    job_uncharge(o->job, JOB_LIMIT_HANDLES, 1);
    job_unref(o->job);
    kfree(o);
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
}

static const struct kobject_ops interrupt_ops = {
    .name = "interrupt",
    .destroy = interrupt_destroy,
    .on_zero_handles = interrupt_zero_handles,
};

/* A new object routed to a fresh vector (fire armed from here on: a stray
 * message to the vector may call it at once, so it is complete first). */
static status_t create(struct job *job, enum irq_kind kind, bool maskable, struct pci_dev *d,
                       uint32_t index, struct kinterrupt **out)
{
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct kinterrupt *o = kzalloc(sizeof(*o));
    if (!o) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    kobject_init(&o->base, OBJ_INTERRUPT, &interrupt_ops, "interrupt", 0);
    o->magic = IRQ_MAGIC;
    o->kind = (uint8_t)kind;
    o->maskable = maskable;
    o->dev = d;
    o->index = index;
    st = vector_alloc(fire, o, &o->cpu, &o->vec);
    if (st != OK) {
        kfree(o);
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        return st;
    }
    job_ref(job);
    o->job = job;
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    *out = o;
    return OK;
}

status_t interrupt_create_msi(struct pci_dev *d, uint32_t index, uint32_t flags,
                              struct kobject **out)
{
    if (!d || (flags & ~IRQ_MSIX))
        return ERR_INVALID_ARGS;
    if (d->info.flags & (PCI_INFO_DISPLAY | PCI_INFO_BRIDGE))
        return ERR_ACCESS_DENIED;   /* never touched (M6-PLAN.md) */
    bool msix = flags & IRQ_MSIX;
    if (msix ? !d->cap_msix : !d->cap_msi)
        return ERR_NOT_SUPPORTED;
    if (msix ? index >= d->info.msix_vectors : index != 0)
        return ERR_OUT_OF_RANGE;
    struct kinterrupt *o;
    status_t st = create(job_current(), msix ? IK_MSIX : IK_MSI, msix || d->msi_maskable, d,
                         index, &o);
    if (st != OK)
        return st;
    st = dev_claim(o);
    if (st != OK) {
        kobject_unref(&o->base);   /* not listed: teardown only frees the vector */
        return st;
    }
    *out = &o->base;
    return OK;
}

status_t interrupt_create_virtual_ex(struct job *job, bool maskable, struct kobject **out)
{
    struct kinterrupt *o;
    status_t st = create(job, IK_VIRTUAL, maskable, NULL, 0, &o);
    if (st == OK)
        *out = &o->base;
    return st;
}

status_t interrupt_create_virtual(struct kobject **out)
{
    return interrupt_create_virtual_ex(job_current(), false, out);
}

/* ---- tests ------------------------------------------------------------------ */

bool interrupt_vector_of(struct kobject *irq, uint32_t *cpu, uint8_t *vec)
{
    struct kinterrupt *o = to_irq(irq);
    if (!o || __atomic_load_n(&o->torn, __ATOMIC_ACQUIRE) != TORN_NO)
        return false;
    *cpu = o->cpu;
    *vec = o->vec;
    return true;
}

uint64_t interrupt_late_fires(struct kobject *irq)
{
    struct kinterrupt *o = to_irq(irq);
    return o ? __atomic_load_n(&o->late, __ATOMIC_RELAXED) : 0;
}

bool interrupt_is_masked(struct kobject *irq)
{
    struct kinterrupt *o = to_irq(irq);
    uint64_t f = spin_lock_irqsave(&o->base.lock);
    bool m = o->masked;
    spin_unlock_irqrestore(&o->base.lock, f);
    return m;
}

uint64_t interrupt_live_count(void)
{
    return __atomic_load_n(&live, __ATOMIC_RELAXED);
}
