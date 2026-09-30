/* usb-bus: the controller's root ports (xHCI 1.2 4.19, 5.4.8).
 *
 * root_port handles one port after a Port Status Change event (or the
 * first scan): clear its change bits, deal with an over-current, detach
 * what left, and for a new device: debounce (USB 2), reset (USB 2: PR;
 * USB 3: the link trains by itself, warm reset if it gets stuck), read the
 * speed and enumerate it. A failed attempt is tried again after 1 s, then
 * 5 s (root_failed, root_retries: the retry marks the port changed, as an
 * event would); after the third failure the port waits for an unplug. */
#include "usbbus.h"

static uint8_t root_fail[256];       /* failed attach attempts, per root port */
static uint64_t root_retry_at[256];  /* when to try a failed port again (ns); 0: not waiting */
static uint8_t root_oc[256];         /* port power restores after over-current, per root port */

void root_ports_reset(void)
{
    __builtin_memset(root_fail, 0, sizeof(root_fail));
    __builtin_memset(root_oc, 0, sizeof(root_oc));
    __builtin_memset(root_retry_at, 0, sizeof(root_retry_at));
}

static bool root_reset(struct hc *h, uint32_t p, uint32_t *v)
{
    if (!hc_port_is_usb3(h, p)) {
        hc_portsc_write(h, p, PS_PR);
        uint64_t end = drv_clock_ns() + 500 * NS_PER_MS;
        while (drv_clock_ns() < end && !h->stopping) {
            hc_sleep(h, 2);
            *v = hc_portsc(h, p);
            if ((*v & PS_PRC) && !(*v & PS_PR))
                break;
        }
        *v = hc_portsc(h, p);
        hc_portsc_write(h, p, *v & (PS_PRC | PS_PEC | PS_CSC));
        if (!(*v & PS_PRC)) {
            drv_report("usb %u: FAILED at port reset: no PRC 500 ms after PR (PORTSC %08x)", p, *v);
            return false;
        }
        return true;
    }
    /* USB 3: the link trains by itself; a port stuck in Inactive or
     * Compliance (or not enabled after 1 s) gets a warm reset. */
    uint64_t end = drv_clock_ns() + 1000 * NS_PER_MS;
    for (;;) {
        *v = hc_portsc(h, p);
        if ((*v & PS_PED) && PS_PLS(*v) == PLS_U0)
            return true;
        if (!(*v & PS_CCS))
            return false;
        uint32_t pls = PS_PLS(*v);
        if (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE || drv_clock_ns() > end || h->stopping)
            break;
        hc_sleep(h, 10);
    }
    drv_log("usb %u: USB 3 port not in U0 (PORTSC %08x, link state %u): warm reset", p, *v,
            PS_PLS(*v));
    hc_portsc_write(h, p, PS_WPR);
    end = drv_clock_ns() + 1000 * NS_PER_MS;
    while (drv_clock_ns() < end && !h->stopping) {
        hc_sleep(h, 10);
        *v = hc_portsc(h, p);
        if ((*v & (PS_WRC | PS_PRC)) && (*v & PS_PED))
            break;
    }
    *v = hc_portsc(h, p);
    hc_portsc_write(h, p, *v & (PS_WRC | PS_PRC | PS_PEC | PS_CSC | PS_PLC));
    if (!(*v & PS_PED)) {
        drv_report("usb %u: FAILED: USB 3 port not enabled after a warm reset (PORTSC %08x)", p,
                   *v);
        return false;
    }
    return true;
}

/* A root port's over-current (see OC_RESTORES in usbbus.h): with Port
 * Power Control the controller turned PP off. */
static void root_over_current(struct hc *h, uint32_t p)
{
    hc_sleep(h, 100);
    uint32_t v = hc_portsc(h, p);
    if (v & PS_PP) {
        drv_log("usb %u: over-current (PORTSC %08x); its power is still on", p, v);
        return;
    }
    if ((v & PS_OCA) || root_oc[p] >= OC_RESTORES) {
        drv_report("usb %u: over-current: port power left off (%s; PORTSC %08x)", p,
                   v & PS_OCA ? "still over current" : "restored 3 times already", v);
        return;
    }
    root_oc[p]++;
    hc_portsc_write(h, p, PS_PP);
    hc_sleep(h, 20);
    drv_report("usb %u: over-current: port power restored (%u of %u; PORTSC %08x)", p, root_oc[p],
               OC_RESTORES, hc_portsc(h, p));
}

/* A failed attempt on root port p, whose device is still connected: try
 * again after 1 s, then 5 s (a device can be slow to wake); after the
 * third failure the port waits for an unplug. */
static void root_failed(uint32_t p)
{
    root_fail[p]++;
    if (root_fail[p] < 3) {
        uint64_t wait = root_fail[p] == 1 ? 1 * NS_PER_S : 5 * NS_PER_S;
        root_retry_at[p] = drv_clock_ns() + wait;
        drv_log("usb %u: attempt %u failed: trying again in %lu s", p, root_fail[p],
                (unsigned long)(wait / NS_PER_S));
    }
}

/* Root ports whose retry time has come get their port_changed bit set, as
 * if the port had changed, so usb_work runs root_port on them again. The
 * earliest retry still waiting comes back (UINT64_MAX: none), so the main
 * loop can sleep until then and no longer. */
uint64_t root_retries(struct hc *h)
{
    uint64_t now = drv_clock_ns(), next = UINT64_MAX;
    for (uint32_t p = 1; p <= h->ports && p < 256; p++) {
        if (!root_retry_at[p])
            continue;                                   /* not waiting */
        if (root_retry_at[p] <= now) {
            root_retry_at[p] = 0;                       /* due: once */
            h->port_changed[p / 32] |= 1u << (p % 32);  /* as a real change would */
        } else if (root_retry_at[p] < next) {
            next = root_retry_at[p];                    /* still waiting: the earliest */
        }
    }
    return next;
}

void root_port(struct hc *h, uint32_t p)
{
    uint32_t v = hc_portsc(h, p);
    if (v == 0xffffffffu)
        return;
    if (v & PS_CHANGES)
        hc_portsc_write(h, p, v & PS_CHANGES);
    if (v & PS_OCC) {
        drv_log("usb %u: over-current (PORTSC %08x)", p, v);
        root_over_current(h, p);
        uint32_t w = hc_portsc(h, p);
        if (w & PS_CHANGES)
            hc_portsc_write(h, p, w & PS_CHANGES);
        v = (w & ~PS_CHANGES) | ((v | w) & PS_CHANGES);   /* keep every change seen */
    }
    struct usbdev *d = child_at(-1, (uint8_t)p);
    if (!(v & PS_CCS)) {
        root_fail[p] = 0;
        root_retry_at[p] = 0;
        if (d)
            detach(d, "unplugged", false);
        return;
    }
    if (d && ((v & PS_CSC) || !(v & PS_PED))) {
        detach(d, v & PS_CSC ? "replugged" : "port disabled", false);
        d = NULL;
    }
    if (d || root_fail[p] >= 3 || h->stopping)
        return;
    bool usb3 = hc_port_is_usb3(h, p);
    if (!usb3) {
        hc_sleep(h, 100);   /* debounce */
        v = hc_portsc(h, p);
        if (!(v & PS_CCS))
            return;
    }
    if (!root_reset(h, p, &v)) {
        root_failed(p);
        g_failed++;
        return;
    }
    if (!usb3)
        hc_sleep(h, 10);   /* reset recovery */
    v = hc_portsc(h, p);
    if (!(v & PS_PED) || !(v & PS_CCS)) {
        root_failed(p);
        return;
    }
    uint8_t speed = (uint8_t)PS_SPEED(v);
    if (!speed || speed > SPEED_SUPERPLUS) {
        drv_report("usb %u: unknown port speed ID %u (PORTSC %08x): not enumerated", p, speed, v);
        root_failed(p);
        g_failed++;
        return;
    }
    if (!enumerate(-1, (uint8_t)p, speed))
        root_failed(p);
}
