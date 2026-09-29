/* usb-bus: hubs (USB 2.0 chapter 11, USB 3.2 chapter 10).
 *
 * A configured hub gets hub_setup: SET_HUB_DEPTH (SuperSpeed), the hub
 * descriptor (USB 2 0x29 / SuperSpeed 0x2A), a second Configure Endpoint
 * setting the slot's Hub bit, Number of Ports and TT Think Time, power on
 * every port, the status-change endpoint, and a scan of every port.
 *
 * After that the hub's work is driven by its change bitmap (hub_change:
 * bit 0 the hub itself, bit n port n), filled by the status-change
 * endpoint (intr.c) or a scan. hub_work does one unit at a time: the hub's
 * own change, or one port: read and clear its status, handle an
 * over-current, detach what left, and for a new device debounce, reset
 * (warm for a stuck SuperSpeed link), find the speed and enumerate it. */
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
    uint16_t st = 0, chg = 0;
    if (hub_port_status(hub, port, &st, &chg) != CC_SUCCESS)
        return;
    uint16_t power = hub->ss_hub ? 1u << 9 : 1u << 8;
    if (st & power) {
        drv_log("usb %s: port %u over-current; its power is still on", hub->path, port);
        return;
    }
    if ((st & (1u << 3)) || hub->port_oc[port] >= OC_RESTORES) {
        drv_report("usb %s: port %u over-current: port power left off (%s)", hub->path, port,
                   st & (1u << 3) ? "still over current" : "restored 3 times already");
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
static bool hub_port_read(struct usbdev *hub, uint8_t port, uint16_t *st, uint16_t *chg)
{
    uint32_t cc = hub_port_status(hub, port, st, chg);
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
        if (hub_port_status(hub, port, st, chg) != CC_SUCCESS)
            return false;
    }
    return true;
}

/* The device on the port, if any, against the status: detached when it
 * left, was replugged or its port got disabled. True if a new device
 * should be attached now. */
static bool hub_port_wants_attach(struct usbdev *hub, uint8_t port, uint16_t st, uint16_t chg)
{
    struct usbdev *c = child_at(dev_index(hub), port);
    bool connected = st & 1, enabled = st & 2;
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
    return !c && hub->port_fail[port] < 3 && !g_hc.stopping;
}

/* Wait (up to 800 ms) for the reset to finish, then clear its change
 * bits. False if it never did. */
static bool hub_port_reset_wait(struct usbdev *hub, uint8_t port, bool warm, uint16_t *st,
                                uint16_t *chg)
{
    struct hc *h = &g_hc;
    uint64_t end = drv_clock_ns() + 800 * NS_PER_MS;
    bool done = false;
    uint16_t reset_chg = warm ? (1u << 5) | (1u << 4) : (1u << 4);
    while (drv_clock_ns() < end && !hub->gone && !h->stopping) {
        hc_sleep(h, 10);
        if (hub_port_status(hub, port, st, chg) != CC_SUCCESS)
            continue;
        if ((*chg & reset_chg) && !(*st & (1u << 4))) {
            done = true;
            break;
        }
    }
    if (!done) {
        hub->port_fail[port]++;
        drv_report("usb %s.%u: FAILED at hub port reset: no reset change in 800 ms (status %04x "
                   "change %04x)", hub->path, port, *st, *chg);
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

/* A new device: debounce, reset, speed. False (with the port's failure
 * counted where it is the device's fault) if it can't be enumerated. */
static bool hub_port_reset(struct usbdev *hub, uint8_t port, uint8_t *speed)
{
    uint16_t st = 0, chg = 0;
    hc_sleep(&g_hc, 100);
    if (hub_port_status(hub, port, &st, &chg) != CC_SUCCESS || !(st & 1))
        return false;
    /* A SuperSpeed port whose link is stuck (SS.Inactive, Compliance)
     * needs a warm reset (BH_PORT_RESET); the rest a (hot) PORT_RESET. */
    uint32_t link = (st >> 5) & 0xf;
    bool warm = hub->ss_hub && (link == PLS_INACTIVE || link == PLS_COMPLIANCE);
    uint32_t cc = hub_feature(hub, true, warm ? HUB_BH_PORT_RESET : HUB_PORT_RESET, port);
    if (cc != CC_SUCCESS) {
        hub->port_fail[port]++;
        drv_log("usb %s: port %u: SET_FEATURE(%s): %s", hub->path, port,
                warm ? "BH_PORT_RESET" : "PORT_RESET", cc_str(cc));
        return false;
    }
    if (!hub_port_reset_wait(hub, port, warm, &st, &chg))
        return false;
    if (!(st & 2)) {
        hub->port_fail[port]++;
        drv_report("usb %s.%u: FAILED: port not enabled after reset (status %04x)", hub->path,
                   port, st);
        g_failed++;
        return false;
    }
    if (hub->ss_hub)
        *speed = SPEED_SUPER;
    else if (st & (1u << 9))
        *speed = SPEED_LOW;
    else if (st & (1u << 10))
        *speed = SPEED_HIGH;
    else
        *speed = SPEED_FULL;
    return true;
}

static void hub_port(struct usbdev *hub, uint8_t port)
{
    uint16_t st = 0, chg = 0;
    uint8_t speed;
    if (!hub_port_read(hub, port, &st, &chg) || !hub_port_wants_attach(hub, port, st, chg) ||
        !hub_port_reset(hub, port, &speed))
        return;
    hc_sleep(&g_hc, 10);
    if (!enumerate(dev_index(hub), port, speed))
        hub->port_fail[port]++;
}

/* One unit of a hub's work: its own status change, or one port. The
 * main loop serves channels between units, so a class driver's request
 * never waits behind a whole hub's worth of enumerations. */
void hub_work(struct usbdev *hub)
{
    if (hub->hub_scan_all) {
        hub->hub_scan_all = false;
        for (uint8_t p = 1; p <= hub->hub_ports; p++)
            hub->hub_change[p / 32] |= 1u << (p % 32);
    }
    if (hub->hub_change[0] & 1) {
        hub->hub_change[0] &= ~1u;
        uint8_t b[4];
        uint32_t n = 0;
        if (usb_control(hub, 0xa0, 0, 0, 0, 4, b, &n, 1000) == CC_SUCCESS && n == 4) {
            uint16_t chg = le16(b + 2);
            if (chg & 1)
                usb_control(hub, 0x20, 1, 0, 0, 0, NULL, &n, 1000);   /* C_HUB_LOCAL_POWER */
            if (chg & 2) {
                drv_log("usb %s: hub over-current", hub->path);
                usb_control(hub, 0x20, 1, 1, 0, 0, NULL, &n, 1000);   /* C_HUB_OVER_CURRENT */
            }
        }
        return;
    }
    for (unsigned p = 1; p < 32 * 8; p++)
        if (hub->hub_change[p / 32] & (1u << (p % 32))) {
            hub->hub_change[p / 32] &= ~(1u << (p % 32));
            if (p <= hub->hub_ports)
                hub_port(hub, (uint8_t)p);
            return;
        }
}
