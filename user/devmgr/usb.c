/* devmgr: USB interfaces (M7). usb-bus writes `usbbus.interface_attached`
 * (abi/idl/usbbus.idl) on its DR_SERVE channel, txid 0, by itself, for
 * each interface of a device it configured, with ONE handle: that
 * interface's `usb` channel. devmgr keeps the channel in usb_ifs until the
 * device goes (usb-bus closes its end: PEER_CLOSED) or the usb-bus that
 * reported it dies. */
#include "internal.h"
#include <idl/usbbus.h>

struct usb_if {
    handle_t ch;        /* our end of the interface's `usb` channel; 0: a free slot */
    uint32_t bus;       /* devs index of the usb-bus that reported it */
    uint16_t gen;       /* bumped at every use of the slot: in its port key */
    struct usbbus_interface_attached_req info;
};

static struct usb_if usb_ifs[MAX_USB_IFS];

static const char *if_path(const struct usb_if *u)
{
    static char p[25];
    memcpy(p, u->info.path, 24);
    p[24] = 0;
    return p;
}

static void drop(struct usb_if *u)
{
    jam_port_unbind(port, u->ch, KEY_IF_OF(u - usb_ifs, u->gen));
    jam_handle_close(u->ch);
    u->ch = HANDLE_INVALID;
}

/* One message is queued on h that doesn't fit: take it off anyway. */
static void discard(handle_t h, uint32_t n, uint32_t nh)
{
    uint8_t *big = malloc(n ? n : 1);
    handle_t *bh = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = h, .bytes_cap = n, .bytes = (uint64_t)(uintptr_t)big,
        .actual_bytes = (uint64_t)(uintptr_t)&n2, .handles = (uint64_t)(uintptr_t)bh,
        .handles_cap = nh, .actual_handles = (uint64_t)(uintptr_t)&nh2,
    };
    if (big && bh && jam_channel_read(&a) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            jam_handle_close(bh[i]);
    free(big);
    free(bh);
}

static void attached(struct binding *bus, const struct usbbus_interface_attached_req *m,
                     handle_t ch)
{
    unsigned slot = MAX_USB_IFS;
    for (unsigned i = 0; i < MAX_USB_IFS && slot == MAX_USB_IFS; i++)
        if (!usb_ifs[i].ch)
            slot = i;
    if (slot == MAX_USB_IFS) {
        say(true, "devmgr: usb %.24s %04x:%04x if%u: more than %u interfaces: ignored",
            (const char *)m->path, m->vendor, m->product, m->interface_number, MAX_USB_IFS);
        jam_handle_close(ch);
        return;
    }
    struct usb_if *u = &usb_ifs[slot];
    u->ch = ch;
    u->bus = (uint32_t)(bus - devs);
    u->gen++;
    u->info = *m;
    if (jam_port_bind(port, ch, KEY_IF_OF(slot, u->gen), SIG_PEER_CLOSED, PORT_BIND_ONCE) != OK)
        say(false, "devmgr: usb %s if%u: can't watch its channel", if_path(u),
            m->interface_number);
    say(false, "devmgr: usb %s %04x:%04x if%u (%02x/%02x/%02x) attached; no class driver",
        if_path(u), m->vendor, m->product, m->interface_number, m->class_code, m->subclass,
        m->protocol);
}

void usb_driver_events(struct binding *b)
{
    for (int guard = 0; guard < 1024 && b->client; guard++) {
        _Alignas(8) uint8_t buf[sizeof(struct usbbus_interface_attached_req)];
        handle_t hs[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = b->client, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(b->client, n, nh);   /* not ours (a late reply nobody waits for) */
            continue;
        }
        if (st != OK)
            return;
        const struct usbbus_interface_attached_req *m = (const void *)buf;
        if (n != sizeof(*m) || m->ordinal != USBBUS_INTERFACE_ATTACHED || nh != 1) {
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(hs[i]);
            continue;
        }
        attached(b, m, hs[0]);
    }
}

void usb_if_closed(uint64_t key)
{
    uint32_t slot = KEY_INDEX(key);
    if (slot >= MAX_USB_IFS || !usb_ifs[slot].ch || KEY_GEN(key) != usb_ifs[slot].gen)
        return;   /* stale */
    struct usb_if *u = &usb_ifs[slot];
    say(false, "devmgr: usb %s %04x:%04x if%u gone", if_path(u), u->info.vendor,
        u->info.product, u->info.interface_number);
    drop(u);
}

void usb_bus_gone(struct binding *b)
{
    for (unsigned i = 0; i < MAX_USB_IFS; i++)
        if (usb_ifs[i].ch && usb_ifs[i].bus == (uint32_t)(b - devs))
            drop(&usb_ifs[i]);
}
