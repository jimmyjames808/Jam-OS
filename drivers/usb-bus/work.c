/* usb-bus: the tasks' work, and starting them (task.c has the model).
 *
 * work_dispatch, from the main loop, starts a port task for every port
 * with a change and no task yet: a root port marked in port_changed (a
 * Port Status Change event, the first scan, a retry that is due) or a
 * hub's port marked in its hub_change (its status-change endpoint, the
 * scan after hub_setup, a retry). serve.c starts a device task for every
 * device with requests queued on its interface channels; work_dispatch
 * also starts one for a device with endpoint upkeep queued (intr.c).
 * usb_busy says whether port work is still pending, running or waiting
 * for a failed port's retry (the RESULTS lines wait until it settles). */
#include "usbbus.h"

#define STOP_WAIT_MS 2000   /* how long the tasks get to end when the driver stops */

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

/* Has hub d work: a change to look at, or a failed port waiting for its
 * retry? */
static bool hub_pending(const struct usbdev *d)
{
    if (d->hub_scan_all)
        return true;
    for (int k = 0; k < 8; k++)
        if (d->hub_change[k])
            return true;
    for (int p = 1; p < 16; p++)
        if (d->port_retry_at[p])
            return true;
    return false;
}

bool usb_busy(void)
{
    for (int i = 0; i < 8; i++)
        if (g_hc.port_changed[i])
            return true;
    if (root_retry_waiting())
        return true;
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (d->used && !d->gone && d->is_hub && hub_pending(d))
            return true;
    }
    return tasks_live(TASK_PORT) != 0;
}

/* ---- the tasks ---------------------------------------------------------------- */

/* A port's work: t->dev_id the hub (0: a root port), t->port the port. */
static void port_task(struct task *t)
{
    if (!t->dev_id) {
        root_port(&g_hc, t->port);
        return;
    }
    struct usbdev *hub = dev_find(t->dev_id);
    if (!hub)
        return;
    dev_hold(hub);
    hub_port_work(hub, t->port);
    dev_put(hub);
}

/* A device's queued requests and endpoint upkeep, round after round
 * until there are none (each round lets the other tasks run first). */
void device_task(struct task *t)
{
    struct usbdev *d = dev_find(t->dev_id);
    if (!d)
        return;
    dev_hold(d);
    for (int round = 0; round < 1000 && !d->gone && !g_hc.stopping; round++) {
        bool did = dev_upkeep(&g_hc, d);
        if (serve_device_chans(d->id))
            did = true;
        if (!did)
            break;
        task_yield();
    }
    dev_put(d);
}

/* ---- starting them -------------------------------------------------------------- */

/* A task for each marked bit of `bits` (ports 0..nports) with none yet;
 * a bit stays marked while no task slot is free. */
static void start_ports(uint32_t *bits, uint32_t hub_id, unsigned first, unsigned nports)
{
    for (unsigned p = first; p <= nports && p < 256; p++) {
        uint32_t bit = 1u << (p % 32);
        if (!(bits[p / 32] & bit) || task_find(TASK_PORT, hub_id, (uint8_t)p))
            continue;
        bits[p / 32] &= ~bit;
        if (!task_start(TASK_PORT, hub_id, (uint8_t)p, port_task)) {
            bits[p / 32] |= bit;
            return;
        }
    }
}

void work_dispatch(struct hc *h)
{
    if (!started || h->dead || h->stopping)
        return;
    start_ports(h->port_changed, 0, 1, h->ports);
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || d->gone)
            continue;
        if (d->is_hub && d->configured && d->hub_ports) {
            if (d->hub_scan_all) {
                d->hub_scan_all = false;
                for (uint8_t p = 1; p <= d->hub_ports; p++)
                    d->hub_change[p / 32] |= 1u << (p % 32);
            }
            start_ports(d->hub_change, d->id, 0, d->hub_ports);
        }
        if ((d->ep_recover | d->ep_drop) && !task_find(TASK_DEVICE, d->id, 0))
            (void)task_start(TASK_DEVICE, d->id, 0, device_task);   /* no slot: next round */
    }
}

/* Shutdown: the tasks first (their waits end at once on h->stopping, and
 * the controller's events are still taken meanwhile), then every device
 * detached. No commands are sent once stopping: slots and endpoints stay
 * as they are until hc_shutdown halts and resets the controller right
 * after, which clears them all (a Disable Slot per device could outlast
 * devmgr's STOP_WAIT). Their DMA pages are kept, not reused, until then
 * (dev_free). */
void usb_stop_all(struct hc *h)
{
    task_kick();
    uint64_t end = drv_clock_ns() + STOP_WAIT_MS * NS_PER_MS;
    while ((tasks_live(TASK_PORT) || tasks_live(TASK_DEVICE)) && drv_clock_ns() < end) {
        tasks_run();
        hc_wait_idle(h, tasks_next_wake(end));
    }
    if (tasks_live(TASK_PORT) || tasks_live(TASK_DEVICE))
        drv_log("%u task(s) still running after %u ms: stopping anyway",
                tasks_live(TASK_PORT) + tasks_live(TASK_DEVICE), STOP_WAIT_MS);
    for (int i = 0; g_devs && i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent < 0)
            detach(&g_devs[i], "driver stopping", true);
    if (g_devs)
        dev_reap();   /* entries already gone, whose tasks have ended since */
}
