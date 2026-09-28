/* DMA capabilities: the permission to pin memory for device DMA
 * (vmo_pin). A driver gets one from devmgr in M6, and the IOMMU domain will
 * hang off it once VT-d/AMD-Vi arrive. For now it carries no state. */
#include <jam/mm.h>
#include <jam/vmo.h>

struct dma_cap {
    struct kobject base;
};

static void dma_cap_destroy(struct kobject *obj)
{
    kfree(obj);
}

static const struct kobject_ops dma_cap_ops = {
    .name = "dma_cap",
    .destroy = dma_cap_destroy,
};

status_t dma_cap_create(struct kobject **out)
{
    struct dma_cap *c = kzalloc(sizeof(*c));
    if (!c)
        return ERR_NO_MEMORY;
    kobject_init(&c->base, OBJ_DMA_CAP, &dma_cap_ops, "dma_cap", 0);
    *out = &c->base;
    return OK;
}
