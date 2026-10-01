/* usb-bus: attaching a device (enumeration) and detaching it (xHCI 1.2
 * chapter 4, USB 2.0 chapter 9).
 *
 * Enumeration, one device at a time: port connect -> debounce -> reset
 * (USB 2 root ports: PORTSC.PR; USB 3 root ports train by themselves, warm
 * reset if stuck; hub ports: SET_FEATURE(PORT_RESET)) -> speed; that much
 * is rootport.c's and hub.c's. Then enumerate(), here: Enable Slot ->
 * input context (slot: route string, speed, root port, and for a
 * full/low-speed device behind a high-speed hub its Transaction
 * Translator: that hub's slot and port, MTT; EP0 max packet by speed) ->
 * Address Device (BSR=0) -> GET_DESCRIPTOR(device, 8) -> Evaluate Context
 * if EP0's max packet differs -> the full device descriptor, the
 * configuration descriptor, the product string -> Configure Endpoint for
 * the interrupt-IN endpoints (Linux's order: the controller's bandwidth
 * check before the device is configured) -> SET_CONFIGURATION. A hub then
 * gets hub_setup (hub.c).
 *
 * Hubs run single-TT (alternate setting 0, which every multi-TT hub
 * supports), so MTT is 0 for them and for the devices behind them.
 *
 * Every wait is bounded; a device that fails is logged with the step and
 * the completion code, its slot is disabled, and the port is tried at
 * most three times until it disconnects. Nothing here can hang the
 * driver: commands abort after their timeout, control transfers stop
 * their endpoint after theirs. */
#include "usbbus.h"

/* One enumeration in progress: the device, and when a step fails, which
 * one and its completion code. */
struct attach {
    struct usbdev *d;      /* the device being enumerated */
    const char *failed_at; /* the step that failed, NULL while none has */
    uint32_t cc;           /* that step's completion code */
    uint8_t iproduct;      /* the product string's index, from the device descriptor */
};

static bool failed(struct attach *a, const char *step, uint32_t cc)
{
    a->failed_at = step;
    a->cc = cc;
    return false;
}

/* A descriptor read that "succeeded" with too few bytes. */
static uint32_t short_cc(uint32_t cc)
{
    return cc == CC_SUCCESS ? CC_SHORT_PACKET : cc;
}

/* Where d sits: its path, root port, tier and route string, and the TT a
 * full/low-speed device behind a high-speed hub goes through. */
static void dev_place(struct usbdev *d, int parent, uint8_t port, uint8_t speed)
{
    struct usbdev *p = parent >= 0 ? &g_devs[parent] : NULL;
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    dev_set_path(d, p, port);
    if (!p) {
        d->root_port = port;
        d->level = 1;
        d->route = 0;
        return;
    }
    d->root_port = p->root_port;
    d->level = (uint8_t)(p->level + 1);
    d->route = p->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * (p->level - 1)));
    if ((speed == SPEED_FULL || speed == SPEED_LOW) && p->speed == SPEED_HIGH) {
        d->tt_slot = p->slot;
        d->tt_port = port;
        d->tt_mtt = p->hub_mtt;
    } else if (p->tt_slot && speed < SPEED_HIGH) {
        d->tt_slot = p->tt_slot;
        d->tt_port = p->tt_port;
        d->tt_mtt = p->tt_mtt;
    }
}

/* ---- the steps -------------------------------------------------------------- */

/* Enable Slot, with the slot type of the root port's protocol. A success
 * with a slot id outside 1..MaxSlotsEn is a controller fault and fails the
 * step; a real id (not 0, which names no slot) is disabled first, or the
 * controller would keep it enabled with nobody tracking it. It never
 * reaches d->slot: dev_free would write its DCBAA entry, which for an id
 * past MaxSlotsEn is outside the array. */
static bool enable_slot(struct attach *a)
{
    struct hc *h = &g_hc;
    struct usbdev *d = a->d;
    uint32_t slot_type = 0;
    for (unsigned i = 0; i < h->nproto; i++)
        if (d->root_port >= h->proto[i].first &&
            d->root_port < h->proto[i].first + h->proto[i].count)
            slot_type = h->proto[i].slot_type;
    uint32_t slot = 0;
    uint32_t cc = hc_command(h, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT) | slot_type << 16, &slot, 1000);
    if (cc != CC_SUCCESS)
        return failed(a, "Enable Slot", cc);
    if (!slot || slot > h->max_slots_en) {
        drv_log("usb %s: Enable Slot gave slot %u (MaxSlotsEn %u)", d->path, slot,
                h->max_slots_en);
        if (slot && !disable_slot_id(d->path, slot))
            drv_log("usb %s: slot %u stays enabled", d->path, slot);
        return failed(a, "Enable Slot", CC_BAD_SLOT);
    }
    d->slot = (uint8_t)slot;
    return true;
}

/* The input and output contexts, EP0's ring and its control transfers'
 * bounce page; the output context goes into the DCBAA. */
static bool alloc_contexts(struct attach *a)
{
    struct hc *h = &g_hc;
    struct usbdev *d = a->d;
    d->out_page = pool_alloc(h);
    d->in_page = pool_alloc(h);
    d->ctl.page = pool_alloc(h);
    if (d->out_page < 0 || d->in_page < 0 || d->ctl.page < 0 || !ring_init(h, &d->ep0))
        return failed(a, "memory for the contexts", CC_RESOURCE);
    hc_set_dcbaa(h, d->slot, pool_dev(h, d->out_page));
    return true;
}

/* Address Device: the slot context and EP0 (its max packet guessed from
 * the speed), tried twice. */
static bool address_device(struct attach *a)
{
    struct hc *h = &g_hc;
    struct usbdev *d = a->d;
    d->mps0 = d->speed >= SPEED_SUPER ? 512 : d->speed == SPEED_HIGH ? 64 : 8;
    zero(pool_va(h, d->in_page), PAGE);
    volatile uint32_t *ctl = in_ctx(d, 0), *sc = in_ctx(d, 1), *e0 = in_ctx(d, 2);
    ctl[1] = 3;   /* A0 | A1 */
    sc[0] = (d->route & 0xfffff) | ((uint32_t)d->speed << 20) | ((uint32_t)d->tt_mtt << 25) |
            (1u << 27);
    sc[1] = (uint32_t)d->root_port << 16;
    sc[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    sc[3] = 0;
    e0[0] = 0;
    e0[1] = (3u << 1) | (EPT_CONTROL << 3) | ((uint32_t)d->mps0 << 16);
    e0[2] = (uint32_t)d->ep0.dev | 1;
    e0[3] = (uint32_t)(d->ep0.dev >> 32);
    e0[4] = 8;
    uint32_t cc = CC_TIMEOUT;
    for (int attempt = 0; attempt < 2 && !h->stopping; attempt++) {
        cc = hc_command(h, (uint32_t)in_dev(d), (uint32_t)(in_dev(d) >> 32), 0,
                        TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)d->slot << 24), NULL, 3000);
        if (cc == CC_SUCCESS)
            break;
        drv_log("usb %s: Address Device: %s; %s", d->path, cc_str(cc),
                attempt ? "giving up" : "trying again");
        hc_sleep(h, 20);
    }
    if (cc != CC_SUCCESS)
        return failed(a, "Address Device (SET_ADDRESS)", cc);
    d->address = (uint8_t)(out_ctx(d, 0)[3] & 0xff);
    hc_sleep(h, 10);
    return true;
}

/* The first 8 bytes of the device descriptor hold EP0's real max packet;
 * if the guess was wrong, Evaluate Context fixes it. */
static bool ep0_max_packet(struct attach *a)
{
    struct hc *h = &g_hc;
    struct usbdev *d = a->d;
    uint8_t dd[8];
    uint32_t n = 0;
    uint32_t cc = get_desc(d, 1, 0, 0, dd, 8, &n);
    if (cc != CC_SUCCESS || n < 8)
        return failed(a, "GET_DESCRIPTOR(device, 8)", short_cc(cc));
    uint16_t mps = dd[7];
    if (d->speed >= SPEED_SUPER)
        mps = (uint16_t)(1u << (dd[7] & 15));   /* an exponent from USB 3.0 on */
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64 && mps != 512)
        mps = d->mps0;
    if (mps == d->mps0)
        return true;
    d->mps0 = mps;
    zero(pool_va(h, d->in_page), PAGE);
    volatile uint32_t *o0 = out_ctx(d, 1), *ctl = in_ctx(d, 0), *e0 = in_ctx(d, 2);
    ctl[1] = 2;   /* A1 */
    for (int i = 0; i < 5; i++)
        e0[i] = o0[i];
    e0[1] = (e0[1] & 0xffffu) | ((uint32_t)mps << 16);
    cc = hc_command(h, (uint32_t)in_dev(d), (uint32_t)(in_dev(d) >> 32), 0,
                    TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t)d->slot << 24), NULL, 1000);
    if (cc != CC_SUCCESS)
        return failed(a, "Evaluate Context (EP0 max packet)", cc);
    return true;
}

static bool device_descriptor(struct attach *a)
{
    struct usbdev *d = a->d;
    uint8_t dd[18];
    uint32_t n = 0;
    uint32_t cc = get_desc(d, 1, 0, 0, dd, 18, &n);
    if (cc != CC_SUCCESS || n < 18)
        return failed(a, "GET_DESCRIPTOR(device)", short_cc(cc));
    d->bcd = le16(dd + 2);
    d->cls = dd[4];
    d->sub = dd[5];
    d->proto = dd[6];
    d->vid = le16(dd + 8);
    d->pid = le16(dd + 10);
    d->nconfigs = dd[17];
    d->iserial = dd[16];
    a->iproduct = dd[15];
    return true;
}

/* Configuration 0: its header, then all of it (up to CFG_MAX), parsed. */
static bool configuration(struct attach *a)
{
    struct usbdev *d = a->d;
    uint8_t ch[9];
    uint32_t n = 0;
    uint32_t cc = get_desc(d, 2, 0, 0, ch, 9, &n);
    if (cc != CC_SUCCESS || n < 9 || ch[1] != 2)
        return failed(a, "GET_DESCRIPTOR(configuration, 9)", short_cc(cc));
    uint16_t total = le16(ch + 2);
    if (total < 9)
        total = 9;
    if (total > CFG_MAX)
        total = CFG_MAX;
    d->cfg = drv_malloc(total);
    if (!d->cfg)
        return failed(a, "memory for the configuration", CC_RESOURCE);
    cc = get_desc(d, 2, 0, 0, d->cfg, total, &n);
    if (cc != CC_SUCCESS || n < 9)
        return failed(a, "GET_DESCRIPTOR(configuration)", short_cc(cc));
    d->cfg_len = (uint16_t)n;
    parse_config(d);
    return true;
}

/* Strings, for the log only: the first language, the product and the
 * serial number. Never fails. */
static bool strings(struct attach *a)
{
    struct usbdev *d = a->d;
    uint8_t langs[8];
    uint32_t n = 0;
    if (usb_control(d, 0x80, 6, 3 << 8, 0, sizeof(langs), langs, &n, 500) == CC_SUCCESS &&
        n >= 4 && langs[1] == 3) {
        uint16_t lang = le16(langs + 2);
        get_string(d, a->iproduct, lang, d->product, sizeof(d->product));
        get_string(d, d->iserial, lang, d->serial, sizeof(d->serial));
    }
    return true;
}

/* A hub by its device or first interface class, unless it would be one
 * tier too many. Never fails. */
static bool hub_or_not(struct attach *a)
{
    struct usbdev *d = a->d;
    d->is_hub = d->cls == 9 || (d->nifs && d->ifs[0].cls == 9);
    d->ss_hub = d->is_hub && d->speed >= SPEED_SUPER;
    if (d->is_hub && d->level >= MAX_LEVEL) {
        d->problem = "hub too deep (5 tiers)";
        d->is_hub = false;
    }
    return true;
}

/* Configure Endpoint (the interrupt-IN endpoints), then SET_CONFIGURATION.
 * A refused Configure Endpoint is not fatal: the device stays, unconfigured,
 * with a problem shown on its line. */
static bool set_configuration(struct attach *a)
{
    struct usbdev *d = a->d;
    uint32_t add = 0;
    for (int k = 2; k < 32; k++)
        if (d->eps[k].dci && d->eps[k].type == EPT_INTR_IN)
            add |= 1u << k;
    bool hub = d->is_hub;
    d->is_hub = false;   /* the hub fields go in once the hub descriptor is read */
    uint32_t cc = add ? configure_eps(d, add, 0) : CC_SUCCESS;
    d->is_hub = hub;
    if (cc != CC_SUCCESS) {
        d->problem = cc == CC_BANDWIDTH ? "Configure Endpoint: no bandwidth"
                                        : "Configure Endpoint failed";
        drv_log("usb %s: Configure Endpoint: %s", d->path, cc_str(cc));
        return true;
    }
    d->cfg_value = d->cfg[5];
    uint32_t n = 0;
    cc = usb_control(d, 0x00, 9, d->cfg_value, 0, 0, NULL, &n, 1000);
    if (cc != CC_SUCCESS)
        return failed(a, "SET_CONFIGURATION", cc);
    d->configured = true;
    return true;
}

/* A configured hub: its descriptor, ports and status-change endpoint. If
 * that fails the device stays, as a non-hub. Never fails. */
static bool setup_hub(struct attach *a)
{
    struct usbdev *d = a->d;
    if (d->is_hub && d->configured && !hub_setup(d))
        d->is_hub = false;
    return true;
}

/* The steps of the enumeration, in order; each is false if the device
 * can't go on. */
static bool (*const steps[])(struct attach *a) = {
    enable_slot,
    alloc_contexts,
    address_device,
    ep0_max_packet,
    device_descriptor,
    configuration,
    strings,
    hub_or_not,
    set_configuration,
    setup_hub,
};

/* ---- attach and detach ------------------------------------------------------ */

static void attach_failed(struct usbdev *d, const char *step, uint32_t cc)
{
    g_failed++;
    const char *tt = d->tt_slot ? " via TT" : "";
    if (g_failed <= 8)
        drv_report("usb %s: %s device%s: FAILED at %s: %s (cc %u)", d->path, speed_long(d->speed),
                   tt, step, cc_str(cc), cc);
    else
        drv_log("usb %s: %s device%s: FAILED at %s: %s (cc %u)", d->path, speed_long(d->speed), tt,
                step, cc_str(cc), cc);
    if (d->tt_slot)
        drv_log("usb %s: slot context had TT hub slot %u, TT port %u, MTT %u, route %05x",
                d->path, d->tt_slot, d->tt_port, d->tt_mtt, d->route);
    d->gone = true;
    serve_iface_gone(d->id);
    bool off = disable_slot(d);
    dev_free(d, off);
    g_last_change_ns = drv_clock_ns();
}

/* The device is as far as it goes: counted, logged, and (configured, not
 * a hub) its interfaces offered to devmgr. */
static void attached(struct usbdev *d)
{
    g_generation++;
    g_last_change_ns = drv_clock_ns();
    if (g_first_report_done) {
        g_attached++;
        dev_line(d, false, "attached: ");
    }
    dev_log_detail(d);
    if (d->configured && !d->is_hub)
        serve_device_ready(d);
}

/* The steps in order. The first that fails ends it: the failure is
 * reported with the step and completion code, the slot disabled and the
 * entry freed. */
bool enumerate(int parent, uint8_t port, uint8_t speed)
{
    struct usbdev *d = dev_alloc();
    if (!d) {
        g_failed++;
        drv_report("usb: port %u: more than %u devices; not enumerated", port, MAX_DEVS);
        return false;
    }
    dev_place(d, parent, port, speed);
    struct attach a = { .d = d };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        if (!steps[i](&a)) {
            attach_failed(d, a.failed_at, a.cc);
            return false;
        }
    attached(d);
    return d->configured;
}

void detach(struct usbdev *d, const char *why, bool quiet)
{
    int me = dev_index(d);
    d->gone = true;
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent == me)
            detach(&g_devs[i], "its hub went away", quiet);
    serve_iface_gone(d->id);   /* its interface and report channels: the peers see PEER_CLOSED */
    if (!quiet)
        drv_log("usb %s: %04x:%04x detached (%s)", d->path, d->vid, d->pid, why);
    if (d->vid && g_first_report_done && !quiet)
        g_detached++;
    /* Stopping: no Disable Slot per device. hc_shutdown's halt and reset,
     * right after, clear every slot; one unanswered command per device
     * could otherwise outlast devmgr's STOP_WAIT. */
    bool off = g_hc.stopping ? false : disable_slot(d);
    dev_free(d, off);
    g_generation++;
    g_last_change_ns = drv_clock_ns();
}
