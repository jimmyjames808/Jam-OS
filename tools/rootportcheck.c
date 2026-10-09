/* rootportcheck: usb-bus's root port handling (drivers/usb-bus/rootport.c,
 * the same file the driver links) against a model of an xHCI root port's
 * PORTSC with the controller's event rule (xHCI 1.2 4.19.2): a Port Status
 * Change event is posted only when the OR of the port's change bits (CSC,
 * PEC, WRC, OCC, PRC, PLC, CEC) goes from 0 to 1. A change that comes
 * while another change bit is still set posts none. QEMU's xHCI doesn't
 * model that (it rewrites PORTSC on every attach and detach, so each one
 * posts an event): this is the only place the rule is tested.
 *
 * Each case puts a hardware change (a stick pushed back in, a connector
 * bouncing) at every point of usb-bus's handling of a port in turn, one
 * run per point: before or after each PORTSC read, write and sleep. Then
 * the port's events are served until none is left (as work.c's port tasks
 * do), and the port must be right: the device attached if one is
 * connected, no change bit left set (or every later plug and pull of the
 * port would post no event), and a pull and a plug after that each seen.
 *   replug3, replug2  a USB 3 and a USB 2 port: the stick pulled, and
 *           pushed back in while the pull is being handled
 *   bounce3, bounce2  a device plugged in that disconnects and reconnects
 *           while it is attached (USB 2: debounce, reset, enumeration)
 * Prints the runs that failed; exit 0 if none did. `make check` runs it. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/usb-bus/usbbus.h"

/* ---- the model ---------------------------------------------------------------- */

#define PORT_USB3 1   /* ports 1-4 USB 3, 5-8 USB 2, as QEMU's qemu-xhci */
#define PORT_USB2 5

struct hc g_hc;
uint32_t g_failed;

static struct {
    uint32_t sc;        /* PORTSC as software reads it */
    bool resetting;     /* PR written: it finishes at the next sleep */
    bool present;       /* usb-bus has a device on it */
} port[9];
static struct usbdev devs[9];
static uint64_t now_ns;
static unsigned step, inject_at;     /* model accesses so far; the one before which to act */
static void (*inject)(uint32_t p);   /* the change */
static uint32_t inject_port;
static unsigned events;
static bool verbose;

/* The change bits' OR going 0 -> 1 posts an event: work.c's bit. */
static void set_sc(uint32_t p, uint32_t v)
{
    if (!(port[p].sc & PS_CHANGES) && (v & PS_CHANGES)) {
        g_hc.port_changed[p / 32] |= 1u << (p % 32);
        events++;
    }
    port[p].sc = v;
}

static void tick(void)
{
    if (inject && ++step == inject_at) {
        inject(inject_port);
        inject = NULL;
    }
}

static void plug(uint32_t p)
{
    uint32_t v = port[p].sc & PS_CHANGES;
    v |= PS_PP | PS_CCS | PS_CSC;
    if (p < PORT_USB2)
        v |= PS_PED | (uint32_t)SPEED_SUPER << 10 | (uint32_t)PLS_U0 << 5;   /* trained */
    else
        v |= 7u << 5;   /* Polling: disabled until reset */
    set_sc(p, v);
}

static void pull(uint32_t p)
{
    port[p].resetting = false;
    /* Disabled, not connected, CSC; a disconnect doesn't set PEC. */
    set_sc(p, (port[p].sc & PS_CHANGES) | PS_PP | PS_CSC | 5u << 5);   /* RxDetect */
}

static void bounce(uint32_t p)
{
    pull(p);
    plug(p);
}

uint32_t hc_portsc(struct hc *h, uint32_t p)
{
    (void)h;
    tick();
    return port[p].sc;
}

void hc_portsc_write(struct hc *h, uint32_t p, uint32_t set)
{
    (void)h;
    tick();
    uint32_t v = port[p].sc & ~(set & PS_CHANGES);
    if ((set & PS_PR) && (v & PS_CCS)) {
        v |= PS_PR;
        port[p].resetting = true;
    }
    port[p].sc = v;   /* a clear never posts an event */
    tick();
}

bool hc_port_is_usb3(struct hc *h, uint32_t p)
{
    (void)h;
    return p < PORT_USB2;
}

void hc_sleep(struct hc *h, uint64_t ms)
{
    (void)h;
    tick();
    now_ns += ms * NS_PER_MS;
    for (uint32_t p = 1; p <= 8; p++)
        if (port[p].resetting) {   /* the reset ends: enabled, high speed */
            port[p].resetting = false;
            set_sc(p, (port[p].sc & ~(PS_PR | 0xfu << 5 | 0xfu << 10)) | PS_PED | PS_PRC |
                          (uint32_t)SPEED_HIGH << 10);
        }
    tick();
}

void hc_wait(struct hc *h, uint64_t deadline)
{
    (void)h;
    now_ns = deadline;
}

uint64_t drv_clock_ns(void)
{
    return now_ns;
}

void drv_log(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void drv_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (verbose) {
        vprintf(fmt, ap);
        putchar('\n');
    }
    va_end(ap);
}

void addr0_take(uint8_t rp)
{
    (void)rp;
}

void addr0_give(uint8_t rp)
{
    (void)rp;
}

struct usbdev *child_at(int parent, uint8_t p)
{
    return parent < 0 && p <= 8 && port[p].present ? &devs[p] : NULL;
}

bool port_entry_left(int parent, uint8_t p)
{
    (void)parent, (void)p;
    return false;
}

bool enumerate(int parent, uint8_t p, uint8_t speed)
{
    (void)parent, (void)speed;
    tick();
    port[p].present = true;
    return true;
}

void detach(struct usbdev *d, const char *why, bool quiet)
{
    (void)why, (void)quiet;
    port[d - devs].present = false;
}

/* ---- running it ----------------------------------------------------------------- */

/* Every port with an event gets a pass, as work.c's tasks do, until none
 * is left (or 50 passes: a port that never settles). */
static void serve(void)
{
    for (int guard = 0; guard < 50; guard++) {
        uint32_t p = 1;
        while (p <= 8 && !(g_hc.port_changed[p / 32] & (1u << (p % 32))))
            p++;
        if (p > 8)
            return;
        g_hc.port_changed[p / 32] &= ~(1u << (p % 32));
        root_port(&g_hc, p);
    }
}

static void fresh(void)
{
    memset(port, 0, sizeof(port));
    memset(&g_hc, 0, sizeof(g_hc));
    root_ports_reset();
    for (uint32_t p = 1; p <= 8; p++)
        port[p].sc = PS_PP | 5u << 5;
    g_hc.ports = 8;
    inject = NULL;
    step = 0;
    events = 0;
}

/* The port settled right: what's connected is attached, no change bit
 * left set; and a pull and a plug after that are each seen. */
static const char *settled(uint32_t p)
{
    if (!!(port[p].sc & PS_CCS) != port[p].present)
        return port[p].present ? "a device attached that isn't there" :
                                 "the connected device never attached";
    if (port[p].sc & PS_CHANGES)
        return "a change bit left set (no more events for the port)";
    pull(p);
    serve();
    if (port[p].present)
        return "the next pull wasn't seen";
    plug(p);
    serve();
    if (!port[p].present)
        return "the next plug wasn't seen";
    return NULL;
}

/* Run `setup` then `start` on port p with `change` before model access k,
 * for every k the handling has (and one past it): the failures. */
static unsigned sweep(const char *name, uint32_t p, void (*setup)(uint32_t),
                      void (*start)(uint32_t), void (*change)(uint32_t))
{
    unsigned bad = 0, k;
    for (k = 1; k < 4000; k++) {
        fresh();
        if (setup) {
            setup(p);
            serve();
        }
        step = 0;
        inject = change;
        inject_at = k;
        inject_port = p;
        start(p);
        serve();
        bool happened = inject == NULL;
        if (!happened)
            change(p);   /* past the end: the change after it all */
        serve();
        const char *why = settled(p);
        if (why) {
            bad++;
            printf("rootportcheck: %s: the change before access %u: %s (PORTSC %08x)\n", name,
                   k, why, port[p].sc);
        }
        if (!happened)
            break;
    }
    printf("rootportcheck: %s: %u of %u runs failed\n", name, bad, k);
    return bad;
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    unsigned bad = 0;
    /* replug: a stick attached, pulled; pushed back in at each point of
     * the pull's handling. */
    bad += sweep("replug3", PORT_USB3, plug, pull, plug);
    bad += sweep("replug2", PORT_USB2, plug, pull, plug);
    /* bounce: a device plugged in; at each point of its attach it
     * disconnects and reconnects. */
    bad += sweep("bounce3", PORT_USB3, NULL, plug, bounce);
    bad += sweep("bounce2", PORT_USB2, NULL, plug, bounce);
    if (bad) {
        printf("rootportcheck: FAILED\n");
        return 1;
    }
    printf("rootportcheck: ok\n");
    return 0;
}
