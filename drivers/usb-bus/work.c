/* usb-bus: the device work the main loop drives (serve.c).
 *
 * usb_work does the cheap endpoint upkeep first, then at most ONE port: a
 * root port with a Port Status Change, or one unit of a hub's work.
 * Enumerating takes a while, and the main loop serves channel requests
 * between calls, so a class driver never waits behind a whole bus's worth
 * of enumerations. usb_busy says whether port work is still pending (the
 * RESULTS lines wait until it settles). */
#include "usbbus.h"

static bool started;

void usb_reset_state(void)
{
    devices_reset();
    root_ports_reset();
    started = false;
}

void usb_start(struct hc *h)
{
    for (uint32_t p = 1; p <= h->ports && p < 256; p++)
        h->port_changed[p / 32] |= 1u << (p % 32);
    g_last_change_ns = drv_clock_ns();
    started = true;
}

bool usb_busy(void)
{
    for (int i = 0; i < 8; i++)
        if (g_hc.port_changed[i])
            return true;
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || !d->is_hub)
            continue;
        if (d->hub_scan_all)
            return true;
        for (int k = 0; k < 8; k++)
            if (d->hub_change[k])
                return true;
    }
    return false;
}

bool usb_work(struct hc *h)
{
    if (!started || h->dead)
        return false;
    /* Endpoint upkeep first: cheap, and keeps input flowing. */
    bool did = intr_upkeep(h);
    /* Then ONE port (a root port, or a hub's): enumerating takes a while,
     * and the main loop serves requests between ports. */
    for (uint32_t p = 1; p <= h->ports && p < 256 && !h->stopping; p++)
        if (h->port_changed[p / 32] & (1u << (p % 32))) {
            h->port_changed[p / 32] &= ~(1u << (p % 32));
            root_port(h, p);
            return true;
        }
    for (int i = 0; i < MAX_DEVS && !h->stopping; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || !d->is_hub || d->gone)
            continue;
        bool any = d->hub_scan_all;
        for (int k = 0; k < 8; k++)
            any |= d->hub_change[k] != 0;
        if (any) {
            hub_work(d);
            return true;
        }
    }
    return did;
}

/* Shutdown: every endpoint stopped, every slot disabled (the controller
 * is reset right after, but this keeps the DMA story simple: nothing is
 * queued when the halt comes). */
void usb_stop_all(const struct hc *h)
{
    (void)h;
    for (int i = 0; g_devs && i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent < 0)
            detach(&g_devs[i], "driver stopping", true);
}
