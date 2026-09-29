/* DMA capabilities: the permission to pin memory for device DMA
 * (vmo_pin). The IOMMU domain will hang off it once VT-d/AMD-Vi arrive.
 *
 * M6: a cap is bound to one PCI function (dma_cap_create_for; the
 * dma_cap_create system call needs its RES_PCI_DEV). vmo_pin with a bound
 * cap needs the function's Bus Master Enable on. Every pin made with a cap
 * is on the cap's `pins` list (vmo.c links and unlinks it; lock order: the
 * cap's object lock, then the VMO's).
 *
 * Closing the last handle (a process kill closes them the same way):
 *   1. `closed` is set, so no new pin can finish (vmo_pin re-checks it
 *      before it publishes the pin);
 *   2. for a bound cap, Bus Master Enable goes off and the command register
 *      is read back (flushing the posted write), under the command-filter
 *      lock so no racing filtered config write can turn it back on;
 *   3. only then every pin made with it is released: the device can no
 *      longer reach the pages when they go back to their VMO.
 * The unbound caps kernel tests make (dma_cap_create) behave the same,
 * minus step 2. A pin holds a reference on its cap, so the struct outlives
 * the handles until the last pin is gone. */
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/vmo.h>

#define PCI_COMMAND 0x04
#define CMD_BME     0x04

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
    if (c->dev) {
        f = pci_cmd_lock();
        pci_set_bus_master(c->dev, false);
        (void)pci_cfg_read(c->dev, PCI_COMMAND, 2);   /* read back: the write has landed */
        pci_cmd_unlock(f);
    }
    vmo_release_cap_pins(c);
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
    if (!d)
        return ERR_INVALID_ARGS;
    /* The kernel never writes their command register, so it couldn't take
     * Bus Master Enable away again. */
    if (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return ERR_ACCESS_DENIED;
    return cap_new(d, out);
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
    uint32_t cmd = pci_cfg_read(c->dev, PCI_COMMAND, 2);
    return cmd != 0xffff && cmd != 0xffffffffu && (cmd & CMD_BME);   /* all ones: gone */
}
