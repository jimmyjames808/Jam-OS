/* usb-bus: hubs (USB 2.0 chapter 11, USB 3.2 chapter 10).
 *
 * A configured hub gets hub_setup: SET_HUB_DEPTH (SuperSpeed), the hub
 * descriptor (USB 2 0x29 / SuperSpeed 0x2A), a second Configure Endpoint
 * setting the slot's Hub bit, Number of Ports and TT Think Time, power on
 * every port, the status-change endpoint, and a scan of every port.
 *
 * After that the hub's work is driven by its change bitmap (hub_change:
 * bit 0 the hub itself, bit n port n), filled by the status-change
 * endpoint (intr.c), a scan or a retry that is due. Each bit becomes a
 * port task (work.c), so the hub's ports go side by side; hub_port_work
 * does one: the hub's own change, or one port: read and clear its status,
 * handle an over-current, detach what left, and for a new device debounce,
 * take the default address (task.c: one device of the tree at a time),
 * reset (warm for a stuck SuperSpeed link), find the speed and enumerate
 * it. A failed attempt is tried again as a root port's is (rootport.c):
 * sooner at first, PORT_TRIES attempts in all, and a device that left or
 * reconnected meanwhile starts over with its own change. */
#include "usbbus.h"

#define HUB_PORT_CONNECTION 0
#define HUB_PORT_ENABLE     1
#define HUB_PORT_RESET      4
#define HUB_PORT_POWER      8
#define HUB_C_CONNECTION    16
#define HUB_C_ENABLE        17
#define HUB_C_SUSPEND       18
#define HUB_C_OVER_CURRENT  19
#define HUB_C_RESET         20
#define HUB_C_LINK_STATE    25
#define HUB_C_CONFIG_ERROR  26
#define HUB_C_BH_RESET      29
#define HUB_BH_PORT_RESET   28

static uint32_t hub_feature(struct usbdev *d, bool set, uint16_t feature, uint16_t port)
{
    uint32_t n;
    return usb_control(d, 0x23, set ? 3 : 1, feature, port, 0, NULL, &n, 1000);
}

static uint32_t hub_port_status(struct usbdev *d, uint8_t port, uint16_t *status,
                                uint16_t *change)
{
    uint8_t b[4];
    uint32_t n = 0;
    uint32_t cc = usb_control(d, 0xa3, 0, 0, port, 4, b, &n, 1000);
    if (cc == CC_SUCCESS && n < 4)
        cc = CC_SHORT_PACKET;
    if (cc == CC_SUCCESS) {
        *status = le16(b);
        *change = le16(b + 2);
    }
    return cc;
}

bool hub_port_lost(struct usbdev *hub, uint8_t port)
{
    uint16_t ps = 0, chg = 0;
    if (hub_port_status(hub, port, &ps, &chg) != CC_SUCCESS)
        return false;
    return !(ps & 1) || (chg & 1);
}

bool hub_setup(struct usbdev *d)
{
    struct hc *h = &g_hc;
    uint8_t b[72];
    uint32_t n = 0, cc;
    if (d->ss_hub) {
        cc = usb_control(d, 0x20, 12, (uint16_t)(d->level - 1), 0, 0, NULL, &n, 1000);
        if (cc != CC_SUCCESS)
            drv_log("usb %s: SET_HUB_DEPTH %u: %s", d->path, d->level - 1, cc_str(cc));
        cc = usb_control(d, 0xa0, 6, 0x2a00, 0, 12, b, &n, 1000);
    } else {
        cc = usb_control(d, 0xa0, 6, 0x2900, 0, 71, b, &n, 1000);
    }
    if (cc != CC_SUCCESS || n < 7) {
        d->problem = "no hub descriptor";
        drv_log("usb %s: hub descriptor: %s (%u bytes)", d->path, cc_str(cc), n);
        return false;
    }
    d->hub_ports = b[2];
    d->hub_chars = le16(b + 3);
    d->pgood_ms = b[5] * 2u;
    d->ttt = (uint8_t)((d->hub_chars >> 5) & 3);
    d->hub_mtt = false;   /* single TT (alternate setting 0) */
    if (d->hub_ports > 15) {
        drv_log("usb %s: hub has %u ports; using the first 15", d->path, d->hub_ports);
        d->hub_ports = 15;
    }
    /* Hub bit, Number of Ports, TT Think Time: a Configure Endpoint of
     * just the slot context (what Linux does). */
    cc = configure_eps(d, 0, 0);
    if (cc != CC_SUCCESS) {
        d->problem = "Configure Endpoint (hub fields) failed";
        drv_log("usb %s: Configure Endpoint (hub fields): %s", d->path, cc_str(cc));
        return false;
    }
    for (uint8_t p = 1; p <= d->hub_ports; p++)
        hub_feature(d, true, HUB_PORT_POWER, p);
    hc_sleep(h, d->pgood_ms > 100 ? d->pgood_ms : 100);
    /* The status-change endpoint. */
    for (int k = 2; k < 32; k++)
        if (d->eps[k].configured && d->eps[k].type == EPT_INTR_IN) {
            if (ep_open_intr(d, &d->eps[k], EP_OWNER_HUB, -1) == 0)
                d->hub_intr_dci = (uint8_t)k;
            break;
        }
    if (!d->hub_intr_dci)
        drv_log("usb %s: hub without a status-change endpoint; ports are scanned on attach only",
                d->path);
    d->hub_scan_all = true;
    return true;
}

/* A hub port's over-current (see OC_RESTORES in usbbus.h). */
static void hub_over_current(struct usbdev *hub, uint8_t port)
{
    hc_sleep(&g_hc, 100);
    uint16_t ps = 0, chg = 0;
    if (hub_port_status(hub, port, &ps, &chg) != CC_SUCCESS)
        return;
    uint16_t power = hub->ss_hub ? 1u << 9 : 1u << 8;
    if (ps & power) {
        drv_log("usb %s: port %u over-current; its power is still on", hub->path, port);
        return;
    }
    if ((ps & (1u << 3)) || hub->port_oc[port] >= OC_RESTORES) {
        drv_report("usb %s: port %u over-current: port power left off (%s)", hub->path, port,
                   ps & (1u << 3) ? "still over current" : "restored 3 times already");
        return;
    }
    hub->port_oc[port]++;
    uint32_t cc = hub_feature(hub, true, HUB_PORT_POWER, port);
    hc_sleep(&g_hc, hub->pgood_ms > 100 ? hub->pgood_ms : 100);
    drv_report("usb %s: port %u over-current: port power restored (%u of %u): %s", hub->path,
               port, hub->port_oc[port], OC_RESTORES, cc_str(cc));
}

/* ---- one port --------------------------------------------------------------- */

/* The port's status and changes, with every change bit we saw cleared
 * (wPortChange bit -> C_* feature) and an over-current dealt with (then
 * the status is read again). False if it can't be read. */
static bool hub_port_read(struct usbdev *hub, uint8_t port, uint16_t *ps, uint16_t *chg)
{
    uint32_t cc = hub_port_status(hub, port, ps, chg);
    if (cc != CC_SUCCESS) {
        if (!hub->gone)
            drv_log("usb %s: port %u status: %s", hub->path, port, cc_str(cc));
        return false;
    }
    static const uint8_t usb2[16] = { HUB_C_CONNECTION, HUB_C_ENABLE, HUB_C_SUSPEND,
                                      HUB_C_OVER_CURRENT, HUB_C_RESET };
    static const uint8_t usb3[16] = { HUB_C_CONNECTION, 0, 0, HUB_C_OVER_CURRENT, HUB_C_RESET,
                                      HUB_C_BH_RESET, HUB_C_LINK_STATE, HUB_C_CONFIG_ERROR };
    const uint8_t *tab = hub->ss_hub ? usb3 : usb2;
    for (unsigned i = 0; i < 16; i++)
        if ((*chg & (1u << i)) && tab[i])
            hub_feature(hub, false, tab[i], port);
    if (*chg & (1u << 3)) {
        drv_log("usb %s: port %u over-current", hub->path, port);
        hub_over_current(hub, port);
        if (hub_port_status(hub, port, ps, chg) != CC_SUCCESS)
            return false;
    }
    return true;
}

/* The device on the port, if any, against the status: detached when it
 * left, was replugged or its port got disabled. True if a new device
 * should be attached now. */
static bool hub_port_wants_attach(struct usbdev *hub, uint8_t port, uint16_t ps, uint16_t chg)
{
    struct usbdev *c = child_at(dev_index(hub), port);
    bool connected = ps & 1, enabled = ps & 2;
    if (!connected) {
        hub->port_fail[port] = 0;
        if (c)
            detach(c, "unplugged", false);
        return false;
    }
    if (c && ((chg & 1) || !enabled)) {
        detach(c, chg & 1 ? "replugged" : "port disabled", false);
        c = NULL;
    }
    if (chg & 1)
        hub->port_fail[port] = 0;   /* a connection of its own: its attempts start over */
    return !c && hub->port_fail[port] < PORT_TRIES && !g_hc.stopping;
}

/* Wait (up to 800 ms) for the reset to finish, then clear its change
 * bits. False if it never did. */
static bool hub_port_reset_wait(struct usbdev *hub, uint8_t port, bool warm, uint16_t *ps,
                                uint16_t *chg)
{
    struct hc *h = &g_hc;
    uint64_t end = drv_clock_ns() + 800 * NS_PER_MS;
    bool done = false;
    uint16_t reset_chg = warm ? (1u << 5) | (1u << 4) : (1u << 4);
    while (drv_clock_ns() < end && !hub->gone && !h->stopping) {
        hc_sleep(h, 10);
        if (hub_port_status(hub, port, ps, chg) != CC_SUCCESS)
            continue;
        if ((*chg & reset_chg) && !(*ps & (1u << 4))) {
            done = true;
            break;
        }
    }
    if (!done) {
        drv_report("usb %s.%u: FAILED at hub port reset: no reset change in 800 ms (status %04x "
                   "change %04x)", hub->path, port, *ps, *chg);
        g_failed++;
        return false;
    }
    hub_feature(hub, false, HUB_C_RESET, port);
    if (hub->ss_hub && (*chg & (1u << 5)))
        hub_feature(hub, false, HUB_C_BH_RESET, port);
    if (*chg & 1)
        hub_feature(hub, false, HUB_C_CONNECTION, port);
    return true;
}

/* Reset the port's new device and find its speed. False if that failed. */
static bool hub_port_reset(struct usbdev *hub, uint8_t port, uint8_t *speed)
{
    uint16_t ps = 0, chg = 0;
    if (hub_port_status(hub, port, &ps, &chg) != CC_SUCCESS || !(ps & 1))
        return false;
    /* A SuperSpeed port whose link is stuck (SS.Inactive, Compliance)
     * needs a warm reset (BH_PORT_RESET); the rest a (hot) PORT_RESET. */
    uint32_t link = (ps >> 5) & 0xf;
    bool warm = hub->ss_hub && (link == PLS_INACTIVE || link == PLS_COMPLIANCE);
    uint32_t cc = hub_feature(hub, true, warm ? HUB_BH_PORT_RESET : HUB_PORT_RESET, port);
    if (cc != CC_SUCCESS) {
        drv_log("usb %s: port %u: SET_FEATURE(%s): %s", hub->path, port,
                warm ? "BH_PORT_RESET" : "PORT_RESET", cc_str(cc));
        return false;
    }
    if (!hub_port_reset_wait(hub, port, warm, &ps, &chg))
        return false;
    if (!(ps & 2)) {
        drv_report("usb %s.%u: FAILED: port not enabled after reset (status %04x)", hub->path,
                   port, ps);
        g_failed++;
        return false;
    }
    if (hub->ss_hub)
        *speed = SPEED_SUPER;
    else if (ps & (1u << 9))
        *speed = SPEED_LOW;
    else if (ps & (1u << 10))
        *speed = SPEED_HIGH;
    else
        *speed = SPEED_FULL;
    return true;
}

/* An attempt on the hub's port failed: as root_failed (rootport.c). */
static void hub_port_failed(struct usbdev *hub, uint8_t port)
{
    uint16_t ps = 0, chg = 0;
    if (hub->gone || g_hc.stopping)
        return;
    if (hub_port_status(hub, port, &ps, &chg) == CC_SUCCESS && (!(ps & 1) || (chg & 1))) {
        drv_log("usb %s.%u: the device %s during the attempt (status %04x change %04x): "
                "starting over", hub->path, port, ps & 1 ? "reconnected" : "went away", ps, chg);
        hub->hub_change[port / 32] |= 1u << (port % 32);
        return;
    }
    hub->port_fail[port]++;
    uint64_t ms = port_retry_ms(hub->port_fail[port]);
    if (ms) {
        hub->port_retry_at[port] = drv_clock_ns() + ms * NS_PER_MS;
        drv_log("usb %s.%u: attempt %u failed: trying again in %lu ms", hub->path, port,
                hub->port_fail[port], (unsigned long)ms);
    } else {
        drv_log("usb %s.%u: %u attempts failed: the port waits for the device to be replugged",
                hub->path, port, hub->port_fail[port]);
    }
}

static void hub_port(struct usbdev *hub, uint8_t port)
{
    uint16_t ps = 0, chg = 0;
    uint8_t speed;
    if (!hub_port_read(hub, port, &ps, &chg) || !hub_port_wants_attach(hub, port, ps, chg))
        return;
    hub->port_retry_at[port] = 0;   /* this is the retry, if one was waiting */
    int me = dev_index(hub);
    if (!wait_port_free(me, port)) {
        hub->port_retry_at[port] = drv_clock_ns() + 100 * NS_PER_MS;
        return;
    }
    hc_sleep(&g_hc, 100);   /* debounce (USB 2.0 7.1.7.3) */
    if (hub_port_status(hub, port, &ps, &chg) != CC_SUCCESS || !(ps & 1))
        return;
    addr0_take(hub->root_port);
    if (!hub_port_reset(hub, port, &speed)) {
        addr0_give(hub->root_port);
        hub_port_failed(hub, port);
        return;
    }
    hc_sleep(&g_hc, 10);   /* reset recovery (USB 2.0 7.1.7.5) */
    if (enumerate(me, port, speed) || child_at(me, port))
        hub->port_fail[port] = 0;
    else
        hub_port_failed(hub, port);
}

/* The hub's own change: clear it (local power, over-current). */
static void hub_self(struct usbdev *hub)
{
    uint8_t b[4];
    uint32_t n = 0;
    if (usb_control(hub, 0xa0, 0, 0, 0, 4, b, &n, 1000) != CC_SUCCESS || n != 4)
        return;
    uint16_t chg = le16(b + 2);
    if (chg & 1)
        usb_control(hub, 0x20, 1, 0, 0, 0, NULL, &n, 1000);   /* C_HUB_LOCAL_POWER */
    if (chg & 2) {
        drv_log("usb %s: hub over-current", hub->path);
        usb_control(hub, 0x20, 1, 1, 0, 0, NULL, &n, 1000);   /* C_HUB_OVER_CURRENT */
    }
}

void hub_port_work(struct usbdev *hub, uint8_t port)
{
    if (hub->gone)
        return;
    if (port == 0)
        hub_self(hub);
    else if (port <= hub->hub_ports)
        hub_port(hub, port);
}

uint64_t hub_retries(uint64_t next)
{
    uint64_t now = drv_clock_ns();
    for (int i = 0; g_devs && i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || d->gone || !d->is_hub)
            continue;
        for (unsigned p = 1; p <= d->hub_ports && p < 16; p++) {
            if (!d->port_retry_at[p])
                continue;
            if (d->port_retry_at[p] <= now) {
                d->port_retry_at[p] = 0;
                d->hub_change[p / 32] |= 1u << (p % 32);
            } else if (d->port_retry_at[p] < next) {
                next = d->port_retry_at[p];
            }
        }
    }
    return next;
}
