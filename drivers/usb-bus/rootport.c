/* usb-bus: the controller's root ports (xHCI 1.2 4.19, 5.4.8).
 *
 * root_port handles one port after a Port Status Change event (or the
 * first scan), in a task of its own (work.c), so every root port goes at
 * once: clear its change bits, deal with an over-current, detach what
 * left, and for a new device: debounce (USB 2), take the default address
 * (task.c), reset (USB 2: PR; USB 3: the link trains by itself, warm
 * reset if it gets stuck), read the speed and enumerate it.
 *
 * A failed attempt is tried again from the reset, sooner at first: after
 * 100 ms, then twice as long each time (port_retry_ms; root_retries marks
 * the port changed when the time comes, as an event would), PORT_TRIES
 * attempts in all, then the port waits for an unplug. A device that left
 * or reconnected during the attempt (the PC's gaming mouse drops off the
 * bus and comes back while it starts) costs no attempt: its own port
 * change starts it over. */
#include "usbbus.h"

static uint8_t root_fail[256];       /* failed attach attempts in a row, per root port */
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

uint64_t port_retry_ms(unsigned fails)
{
    return fails && fails < PORT_TRIES ? 100ull << (fails - 1) : 0;
}

/* An attempt on root port p failed. A device that went or reconnected
 * meanwhile starts over with its port change; otherwise the attempt
 * counts, and the port is tried again after port_retry_ms. */
static void root_failed(struct hc *h, uint32_t p)
{
    uint32_t v = hc_portsc(h, p);
    if (v != 0xffffffffu && (!(v & PS_CCS) || (v & PS_CSC))) {
        drv_log("usb %u: the device %s during the attempt (PORTSC %08x): starting over", p,
                v & PS_CCS ? "reconnected" : "went away", v);
        h->port_changed[p / 32] |= 1u << (p % 32);
        return;
    }
    root_fail[p]++;
    uint64_t ms = port_retry_ms(root_fail[p]);
    if (ms) {
        root_retry_at[p] = drv_clock_ns() + ms * NS_PER_MS;
        drv_log("usb %u: attempt %u failed: trying again in %lu ms", p, root_fail[p],
                (unsigned long)ms);
    } else {
        drv_log("usb %u: %u attempts failed: the port waits for the device to be unplugged", p,
                root_fail[p]);
    }
}

/* Wait (up to 2 s) until no entry of a device that was on this port is
 * left: one that went may still be held by a task finishing a request to
 * it, and its slot is disabled only then. */
bool wait_port_free(int parent, uint8_t port)
{
    uint64_t end = drv_clock_ns() + 2000 * NS_PER_MS;
    while (port_entry_left(parent, port) && drv_clock_ns() < end && !g_hc.stopping)
        hc_wait(&g_hc, end);
    return !port_entry_left(parent, port);
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

/* Reset the newly connected device on p and enumerate it. */
static void root_attach(struct hc *h, uint32_t p)
{
    bool usb3 = hc_port_is_usb3(h, p);
    uint32_t v = 0;
    if (!usb3) {
        hc_sleep(h, 100);   /* debounce (USB 2.0 7.1.7.3) */
        v = hc_portsc(h, p);
        if (!(v & PS_CCS))
            return;
    }
    addr0_take((uint8_t)p);
    if (!root_reset(h, p, &v)) {
        addr0_give((uint8_t)p);
        g_failed++;
        root_failed(h, p);
        return;
    }
    if (!usb3)
        hc_sleep(h, 10);   /* reset recovery (USB 2.0 7.1.7.5) */
    v = hc_portsc(h, p);
    uint8_t speed = (uint8_t)PS_SPEED(v);
    if (!(v & PS_PED) || !(v & PS_CCS) || !speed || speed > SPEED_SUPERPLUS) {
        addr0_give((uint8_t)p);
        if ((v & PS_PED) && (v & PS_CCS)) {
            drv_report("usb %u: unknown port speed ID %u (PORTSC %08x): not enumerated", p, speed,
                       v);
            g_failed++;
        }
        root_failed(h, p);
        return;
    }
    if (enumerate(-1, (uint8_t)p, speed) || child_at(-1, (uint8_t)p))
        root_fail[p] = 0;
    else
        root_failed(h, p);
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
    if (d || root_fail[p] >= PORT_TRIES || h->stopping)
        return;
    root_retry_at[p] = 0;   /* this is the retry, if one was waiting */
    if (!wait_port_free(-1, (uint8_t)p)) {
        drv_log("usb %u: the device that was here is still in use: trying again in 100 ms", p);
        root_retry_at[p] = drv_clock_ns() + 100 * NS_PER_MS;
        return;
    }
    root_attach(h, p);
}
