/* usb-bus: the `usb` protocol (abi/idl/usb.idl), one interface per
 * channel: what a class driver may ask of its interface. The channel's
 * struct chan is the context: its device id and interface number, looked
 * up again on every request, so a device that went away answers
 * ERR_PEER_CLOSED. A client reaches only its own interface and endpoints,
 * and of the standard requests only the ones that can't change what
 * usb-bus keeps (check_request). serve.c reads the requests. */
#include <idl/usb.h>
#include "usbbus.h"

static struct usbdev *ctx_dev(void *ctx, struct iface **f)
{
    struct chan *c = ctx;
    struct usbdev *d = dev_find(c->dev_id);
    *f = d ? usb_iface(d, c->a) : NULL;
    return *f ? d : NULL;
}

static status_t cc_status(uint32_t cc)
{
    switch (cc) {
    case CC_SUCCESS: return OK;
    case CC_STALL: return ERR_NOT_SUPPORTED;   /* the device refused the request */
    case CC_TIMEOUT: return ERR_TIMED_OUT;
    case CC_GONE: return ERR_PEER_CLOSED;
    case CC_PARAMETER: return ERR_INVALID_ARGS;
    case CC_BANDWIDTH: case CC_RESOURCE: return ERR_NO_RESOURCES;
    default: return ERR_INTERNAL;              /* a transfer error (logged) */
    }
}

static status_t u_info(void *ctx, uint16_t *vendor, uint16_t *product, uint8_t *speed,
                       uint8_t *ifnum, uint8_t *cls, uint8_t *sub, uint8_t *proto, uint8_t *nep,
                       uint8_t *alt, uint8_t *address)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    *vendor = d->vid;
    *product = d->pid;
    *speed = d->speed;
    *ifnum = f->number;
    *cls = f->cls;
    *sub = f->sub;
    *proto = f->proto;
    *nep = f->nep;
    *alt = f->alt;
    *address = d->address;
    return OK;
}

static status_t u_get_descriptor(void *ctx, uint8_t type, uint8_t index, uint16_t lang,
                                 uint16_t length, uint8_t ir, uint16_t *actual, uint8_t data[1024])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if (!length || length > 1024 || ir > 1)
        return ERR_INVALID_ARGS;
    uint32_t n = 0;
    uint32_t cc = usb_control(d, ir ? 0x81 : 0x80, 6, (uint16_t)(type << 8 | index),
                              ir ? f->number : lang, length, data, &n, 1000);
    *actual = (uint16_t)n;
    return cc_status(cc);
}

/* May this interface's client send this request? Its own interface or
 * one of its endpoints only; of the standard requests only the harmless
 * ones (the rest would change state usb-bus keeps: SET_INTERFACE goes
 * through set_interface, CLEAR_FEATURE(HALT) would desync the xHC). */
static status_t check_request(struct iface *f, uint8_t rt, uint8_t req, uint16_t index)
{
    uint8_t recip = rt & 0x1f, type = (rt >> 5) & 3;
    if (recip == 1) {
        if ((index & 0xff) != f->number)
            return ERR_ACCESS_DENIED;
    } else if (recip == 2) {
        bool mine = false;
        for (int i = 0; i < f->nep; i++)
            mine |= f->ep_addr[i] == (index & 0xff);
        if (!mine)
            return ERR_ACCESS_DENIED;
    } else {
        return ERR_ACCESS_DENIED;
    }
    if (type == 0 && !(req == 0 || req == 6 || req == 10))   /* GET_STATUS/DESCRIPTOR/INTERFACE */
        return ERR_ACCESS_DENIED;
    if (type == 3)
        return ERR_ACCESS_DENIED;
    return OK;
}

static status_t u_control_in(void *ctx, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                             uint16_t length, uint16_t *actual, uint8_t data[1024])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if (!(rt & 0x80) || length > 1024)
        return ERR_INVALID_ARGS;
    status_t st = check_request(f, rt, req, index);
    if (st != OK)
        return st;
    uint32_t n = 0;
    uint32_t cc = usb_control(d, rt, req, value, index, length, data, &n, 1000);
    *actual = (uint16_t)n;
    return cc_status(cc);
}

static status_t u_control_out(void *ctx, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                              uint16_t length, const uint8_t data[64])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if ((rt & 0x80) || length > 64)
        return ERR_INVALID_ARGS;
    status_t st = check_request(f, rt, req, index);
    if (st != OK)
        return st;
    uint8_t buf[64];
    __builtin_memcpy(buf, data, length);
    uint32_t n = 0;
    return cc_status(usb_control(d, rt, req, value, index, length, buf, &n, 1000));
}

static struct ep *iface_ep(struct usbdev *d, struct iface *f, uint8_t addr)
{
    for (int i = 0; i < f->nep; i++)
        if (f->ep_addr[i] == addr) {
            uint8_t dci = ep_dci(addr);
            if (dci >= 2 && d->eps[dci].dci == dci)
                return &d->eps[dci];
        }
    return NULL;
}

static status_t u_open_interrupt_in(void *ctx, uint8_t endpoint, handle_t *reports,
                                    uint16_t *max_packet, uint8_t *interval_ms)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    struct ep *e = (endpoint & 0x80) ? iface_ep(d, f, endpoint) : NULL;
    if (!e || e->type != EPT_INTR_IN)
        return ERR_INVALID_ARGS;
    if (!e->configured)
        return ERR_BAD_STATE;
    if (e->open) {
        /* A class driver that died and was restarted may ask before the
         * main loop reaped its old report channel: if that one's reader is
         * gone, the endpoint is free. */
        signals_t seen = 0;
        handle_t old = chan_handle(e->chan);
        if (e->owner != EP_OWNER_CLIENT || !old ||
            drv_object_wait_one(old, SIG_PEER_CLOSED, 0, &seen) != OK)
            return ERR_ALREADY_BOUND;
        ep_close(d, e);
    }
    handle_t a, b;
    status_t st = drv_channel_create(&a, &b);
    if (st != OK)
        return st;
    int c = chan_add(a, CHAN_REPORTS, d->id, e->dci);
    if (c < 0) {
        drv_handle_close(a);
        drv_handle_close(b);
        return ERR_NO_RESOURCES;
    }
    if (ep_open_intr(d, e, EP_OWNER_CLIENT, c) != 0) {
        chan_close(c);
        drv_handle_close(b);
        return ERR_NO_RESOURCES;
    }
    uint32_t us = (1u << e->interval) * 125u;
    *reports = b;
    *max_packet = e->mps;
    *interval_ms = (uint8_t)(us < 1000 ? 1 : us / 1000 > 255 ? 255 : us / 1000);
    return OK;
}

static status_t u_endpoint_stats(void *ctx, uint8_t endpoint, uint64_t *reports,
                                 uint64_t *dropped, uint64_t *errors, uint8_t *open)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    struct ep *e = iface_ep(d, f, endpoint);
    if (!e)
        return ERR_INVALID_ARGS;
    *reports = e->reports;
    *dropped = e->dropped;
    *errors = e->errors;
    /* Open for a reader that is still there (a dead class driver's report
     * channel may not be reaped yet). */
    *open = 0;
    if (e->open) {
        signals_t seen = 0;
        handle_t c = chan_handle(e->chan);
        *open = c && drv_object_wait_one(c, SIG_PEER_CLOSED, 0, &seen) != OK;
    }
    return OK;
}

static status_t u_set_interface(void *ctx, uint8_t alt)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    return cc_status(dev_set_interface(d, f, alt));
}

static const struct usb_ops usb_ops = {
    .info = u_info,
    .get_descriptor = u_get_descriptor,
    .control_in = u_control_in,
    .control_out = u_control_out,
    .open_interrupt_in = u_open_interrupt_in,
    .endpoint_stats = u_endpoint_stats,
    .set_interface = u_set_interface,
};

status_t iface_serve_one(struct chan *c)
{
    return usb_serve_one(c->h, &usb_ops, c);
}
