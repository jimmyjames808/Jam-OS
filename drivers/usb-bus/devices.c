/* usb-bus: the device table and the device contexts.
 *
 * g_devs holds every device usb-bus tracks, hubs included (MAX_DEVS). A
 * device is found by its slot or by the hub port it hangs on; dev_alloc
 * hands out a cleared entry with a new id, dev_free takes it back and
 * returns its DMA pages to the pool, but only once the controller has let
 * go of the slot (disable_slot): until then the controller may still
 * write into them.
 *
 * Each device has two pool pages of context (xHCI 6.2): the input context
 * a command reads (index 0 the control context, 1 the slot, dci + 1 an
 * endpoint) and the output (device) context the controller keeps (index 0
 * the slot, dci an endpoint). Both are g_hc.csz bytes per entry. */
#include "usbbus.h"

struct usbdev *g_devs;
uint32_t g_generation, g_attached, g_detached, g_failed, g_report_generation;
uint64_t g_last_change_ns;
bool g_first_report_done;
static uint32_t next_id;

void devices_reset(void)
{
    g_generation = g_attached = g_detached = g_failed = g_report_generation = 0;
    g_last_change_ns = 0;
    g_first_report_done = false;
    next_id = 0;
}

/* ---- the table -------------------------------------------------------------- */

int dev_index(const struct usbdev *d)
{
    return (int)(d - g_devs);
}

struct usbdev *dev_by_slot(uint8_t slot)
{
    if (!slot || !g_devs)
        return NULL;
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].slot == slot)
            return &g_devs[i];
    return NULL;
}

struct usbdev *dev_find(uint32_t id)
{
    for (int i = 0; id && i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].id == id && !g_devs[i].gone)
            return &g_devs[i];
    return NULL;
}

struct usbdev *child_at(int parent, uint8_t port)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent == parent && g_devs[i].port == port)
            return &g_devs[i];
    return NULL;
}

struct usbdev *dev_alloc(void)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (d->used)
            continue;
        zero(d, sizeof(*d));
        d->used = true;
        d->id = ++next_id;
        d->parent = -1;
        d->out_page = d->in_page = -1;
        d->ep0.page = -1;
        for (int k = 0; k < 32; k++) {
            d->eps[k].ring.page = -1;
            d->eps[k].buf_page = -1;
            d->eps[k].chan = -1;
        }
        for (int k = 0; k < MAX_IFS; k++)
            d->ifs[k].devmgr_chan = -1;
        return d;
    }
    return NULL;
}

void dev_free(struct usbdev *d, bool slot_disabled)
{
    if (!slot_disabled && !g_hc.dead) {
        /* Disable Slot failed, or was skipped because the driver is
         * stopping: the controller may still own the slot and run its
         * endpoints (queued TRBs into our buffers), so none of its DMA
         * pages can go back to the pool. Leaked, like a quarantine; when
         * stopping that is expected, so it isn't logged. */
        if(!g_hc.stopping)
            drv_log("usb %s: slot %u not disabled: keeping its DMA pages", d->path, d->slot);
        if (d->cfg)
            drv_free(d->cfg);
        d->cfg = NULL;
        d->used = false;
        return;
    }
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        e->chan = -1;   /* closed by serve_iface_gone() */
        e->open = false;
        ring_free(&g_hc, &e->ring);
        if (e->buf_page >= 0)
            pool_free(&g_hc, e->buf_page);
        e->buf_page = -1;
    }
    ring_free(&g_hc, &d->ep0);
    if (d->slot)
        hc_set_dcbaa(&g_hc, d->slot, 0);
    if (d->out_page >= 0)
        pool_free(&g_hc, d->out_page);
    if (d->in_page >= 0)
        pool_free(&g_hc, d->in_page);
    d->out_page = d->in_page = -1;
    if (d->cfg)
        drv_free(d->cfg);
    d->cfg = NULL;
    d->used = false;
}

bool disable_slot_id(const char *path, uint32_t slot)
{
    uint32_t cc = hc_command(&g_hc, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | slot << 24, NULL, 1000);
    if (cc != CC_SUCCESS)
        drv_log("usb %s: Disable Slot %u: %s", path, slot, cc_str(cc));
    return cc == CC_SUCCESS || cc == 11;   /* 11: Slot Not Enabled */
}

bool disable_slot(struct usbdev *d)
{
    return !d->slot || disable_slot_id(d->path, d->slot);
}

/* ---- contexts --------------------------------------------------------------- */

volatile uint32_t *in_ctx(struct usbdev *d, unsigned index)
{
    return (volatile uint32_t *)((uint8_t *)pool_va(&g_hc, d->in_page) + index * g_hc.csz);
}

volatile uint32_t *out_ctx(struct usbdev *d, unsigned index)
{
    return (volatile uint32_t *)((uint8_t *)pool_va(&g_hc, d->out_page) + index * g_hc.csz);
}

/* A fresh input context: control flags cleared, the slot context copied
 * from the output (device) context. */
void in_reset(struct usbdev *d)
{
    zero(pool_va(&g_hc, d->in_page), PAGE);
    volatile uint32_t *s = in_ctx(d, 1), *o = out_ctx(d, 0);
    for (int i = 0; i < 4; i++)
        s[i] = o[i];
    s[3] = 0;   /* address and slot state: the xHC's */
}

uint64_t in_dev(struct usbdev *d)
{
    return pool_dev(&g_hc, d->in_page);
}
