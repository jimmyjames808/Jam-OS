/* hid: the USB HID class driver. One process per HID
 * interface: devmgr starts it when usb-bus reports an interface of class 3,
 * so each interface of a composite device (a keyboard with a media-key
 * interface, a keyboard+mouse receiver) gets a hid process of its own.
 * Built both ways like every driver (a kernel process with `drivers=kernel`,
 * drv/hid in bootfs otherwise).
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_USB    the `usb` channel for this one interface (abi/idl/usb.idl);
 *             usb-bus closes it when the device is unplugged
 *   DR_INPUT  an `input` channel to the console (abi/idl/input.idl).
 *             Optional: without it every key DOWN (and mouse button change)
 *             goes to the log instead, a bring-up aid for a PC run that
 *             has usb-bus but no console yet
 *
 * Start: usb.info (class 3, or it exits 2), the configuration descriptor
 * (this interface's HID descriptor: the report descriptor's length; its
 * interrupt IN endpoint), the report descriptor (read because some devices
 * only behave once it has been, as Windows and Linux always read it; its
 * top-level collections are logged). Then:
 *   - subclass 1 (boot), protocol 1 keyboard / 2 mouse: SET_PROTOCOL(boot),
 *     for a keyboard SET_IDLE(0) (report only on change) and the LED byte
 *     (Num Lock on), then open_interrupt_in and serve reports. A failed
 *     SET_PROTOCOL or SET_IDLE is logged and ignored (many devices stall
 *     SET_IDLE; a device that refuses SET_PROTOCOL was already in boot
 *     mode or can't leave it);
 *   - anything else (subclass 0: consumer control, system control, a
 *     vendor interface, an NKRO keyboard in report protocol): logged as
 *     "not a boot keyboard/mouse: skipped" with its collections, exit 0.
 *     Report protocol isn't parsed: keyboards that have such a second
 *     interface still type through their boot interface (every one the
 *     PC has is boot capable: the firmware uses them), so no
 *     report-descriptor parsing is attempted even when the second
 *     interface's report looks like a plain keyboard's.
 *
 * Reports arrive as one message each on the channel open_interrupt_in
 * returns. The loop waits on a port for that channel (readable or closed),
 * DR_USB closed and DR_INPUT closed, with the next key repeat as deadline.
 * The keyboard layer is keyboard.c; a boot mouse report is buttons, dx, dy
 * and, when the report has a fourth byte, the wheel (input.mouse, sent
 * only when something changed).
 *
 * Ending (the reconnect rule of input.idl and usb.idl): DR_USB closed =
 * "device gone", exit 0 (devmgr binds a new hid when the interface comes
 * back); DR_INPUT closed = "console gone", exit 0 (devmgr restarts it
 * connected to the new console). The reports channel closing while DR_USB
 * stays open is NOT the device going: usb-bus stopped polling the endpoint
 * (20 errors in a row) but the interface is still there, so exit 5 and
 * devmgr restarts hid, which opens the endpoint again (an exit 0 there
 * would leave the keyboard dead until it is replugged). usb-bus closes a
 * gone device's interface channel before its report channels; hid still
 * gives DR_USB REPORTS_GRACE to follow before it decides. Exit 1: no
 * DR_USB, or a wait failed; 2: not a HID interface; 3: the device's
 * descriptors or endpoint couldn't be used; 4: out of memory; 5: reports
 * lost with the device still there.
 *
 * Every call is bounded: USB requests USB_TIMEOUT, input calls
 * INPUT_TIMEOUT (a late console costs that event, logged, not the
 * driver). */
#include "hid.h"
#include <idl/input.h>
#include <idl/usb.h>

#define USB_TIMEOUT   (2000 * MS)
#define INPUT_TIMEOUT (2000 * MS)
#define REPORTS_GRACE (50 * MS)

#define DESC_CONFIG    0x02
#define DESC_INTERFACE 0x04
#define DESC_ENDPOINT  0x05
#define DESC_HID       0x21
#define DESC_REPORT    0x22

#define REQ_OUT_CLASS_IFACE 0x21   /* host to device, class, interface */
#define HID_SET_REPORT      0x09
#define HID_SET_IDLE        0x0a
#define HID_SET_PROTOCOL    0x0b
#define HID_REPORT_OUTPUT   0x02
#define HID_PROTOCOL_BOOT   0

#define K_REPORTS 1
#define K_USB     2
#define K_INPUT   3

#define BUF_SIZE 1024

/* The "ready" and final lines go to the RESULTS box when devmgr started
 * us for a real interface: it names each hid after it ("hid-10:0"); utest's
 * mock runs are plain "hid" and stay in the log. */
static bool to_results;

#define say_result(...)                 \
    do {                                \
        if (to_results)                 \
            drv_report(__VA_ARGS__);    \
        else                            \
            drv_log(__VA_ARGS__);       \
    } while (0)

static bool gone(struct hid *h, status_t st)
{
    if (st != ERR_PEER_CLOSED)
        return false;
    h->stop = STOP_DEVICE_GONE;
    return true;
}

/* The reports channel closed: the device went (DR_USB closes with it) or
 * only the endpoint was given up (DR_USB stays open). */
static void reports_closed(struct hid *h)
{
    status_t st = drv_object_wait_one(h->usb, SIG_PEER_CLOSED, drv_clock_ns() + REPORTS_GRACE,
                                      NULL);
    h->stop = st == OK ? STOP_DEVICE_GONE : STOP_REPORTS_LOST;
}

/* Log the 1st, 2nd, 4th, 8th ... occurrence of something. */
static bool log_nth(uint64_t n)
{
    return n && !(n & (n - 1));
}

static uint64_t deadline(uint64_t d)
{
    return drv_clock_ns() + d;
}

/* ---- events out ------------------------------------------------------------ */

static void input_result(struct hid *h, status_t st, const char *what)
{
    if (st == OK) {
        h->events++;
    } else if (st == ERR_PEER_CLOSED) {
        h->stop = STOP_CONSOLE_GONE;
    } else if (log_nth(++h->input_errors)) {
        drv_log("hid %04x:%04x if %u: input.%s failed (%s; %lu so far): event dropped",
                h->vendor, h->product, h->iface, what, status_str(st),
                (unsigned long)h->input_errors);
    }
}

void hid_key(struct hid *h, uint16_t usage, uint8_t state, uint8_t mods, uint32_t codepoint)
{
    if (h->stop)
        return;
    if (state == INPUT_KEY_DOWN)
        h->keys_down++;
    if (h->input == HANDLE_INVALID) {
        if (state == INPUT_KEY_DOWN)
            drv_log("hid %04x:%04x if %u: key 0x%02x down, mods 0x%02x, codepoint 0x%x", h->vendor,
                    h->product, h->iface, usage, mods, codepoint);
        h->events++;
        return;
    }
    input_result(h, input_key_until(h->input, deadline(INPUT_TIMEOUT), usage, state, mods,
                                    codepoint), "key");
}

void hid_set_leds(struct hid *h, uint8_t leds)
{
    uint8_t data[64] = { leds };
    status_t st = usb_control_out_until(h->usb, deadline(USB_TIMEOUT), REQ_OUT_CLASS_IFACE,
                                        HID_SET_REPORT, HID_REPORT_OUTPUT << 8, h->iface, 1,
                                        data);
    if (st != OK && !gone(h, st) && log_nth(++h->led_errors))
        drv_log("hid %04x:%04x if %u: SET_REPORT(LEDs 0x%x) failed (%s)", h->vendor, h->product,
                h->iface, leds, status_str(st));
}

static void mouse_report(struct hid *h, const uint8_t *r, uint32_t n)
{
    if (n < 3) {
        h->kbd.short_reports++;
        return;
    }
    uint8_t buttons = r[0] & 0x07;
    int8_t dx = (int8_t)r[1], dy = (int8_t)r[2], wheel = n >= 4 ? (int8_t)r[3] : 0;
    if (!dx && !dy && !wheel && buttons == h->mouse_buttons)
        return;
    uint8_t was = h->mouse_buttons;
    h->mouse_buttons = buttons;
    if (h->input == HANDLE_INVALID) {
        if (buttons != was)
            drv_log("hid %04x:%04x if %u: mouse buttons 0x%x", h->vendor, h->product, h->iface,
                    buttons);
        h->events++;
        return;
    }
    input_result(h, input_mouse_until(h->input, deadline(INPUT_TIMEOUT), dx, dy, wheel, buttons),
                 "mouse");
}

/* ---- descriptors ------------------------------------------------------------ */

/* This interface's HID descriptor and interrupt IN endpoint, from the
 * configuration descriptor in h->buf (n bytes). */
static void parse_config(struct hid *h, uint32_t n)
{
    const uint8_t *d = h->buf;
    bool ours = false;
    for (uint32_t i = 0; i + 2 <= n;) {
        uint8_t len = d[i], type = d[i + 1];
        if (len < 2 || i + len > n)
            break;
        if (type == DESC_INTERFACE && len >= 9) {
            ours = d[i + 2] == h->iface && d[i + 3] == h->alt;
        } else if (ours && type == DESC_HID && len >= 9) {
            for (uint32_t k = 0; k < d[i + 5] && 6 + 3 * k + 3 <= len; k++)
                if (d[i + 6 + 3 * k] == DESC_REPORT && !h->report_desc_len)
                    h->report_desc_len = (uint16_t)(d[i + 7 + 3 * k] | d[i + 8 + 3 * k] << 8);
        } else if (ours && type == DESC_ENDPOINT && len >= 7) {
            uint8_t addr = d[i + 2], attr = d[i + 3];
            if ((addr & 0x80) && (attr & 3) == 3 && !h->ep_in) {
                h->ep_in = addr;
                h->max_packet = (uint16_t)((d[i + 4] | d[i + 5] << 8) & 0x7ff);
            }
        }
        i += len;
    }
}

static const char *collection_name(uint32_t page, uint32_t usage)
{
    if (page == 0x01) {
        switch (usage) {
        case 0x01: return "pointer";
        case 0x02: return "mouse";
        case 0x04: return "joystick";
        case 0x05: return "gamepad";
        case 0x06: return "keyboard";
        case 0x07: return "keypad";
        case 0x80: return "system control";
        }
    }
    if (page == 0x0c)
        return "consumer control";
    if (page >= 0xff00)
        return "vendor";
    return "other";
}

struct text {
    char    *s;
    uint32_t len, cap;
};

static void put_str(struct text *t, const char *p)
{
    while (*p && t->len + 1 < t->cap)
        t->s[t->len++] = *p++;
    t->s[t->len] = 0;
}

static void put_num(struct text *t, uint32_t v, uint32_t base)
{
    char d[12];
    int n = 0;
    do {
        d[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (base == 16)
        put_str(t, "0x");
    char one[2] = { 0, 0 };
    while (n--) {
        one[0] = d[n];
        put_str(t, one);
    }
}

/* A one-line summary of the report descriptor in h->buf (n bytes): its
 * top-level application collections with their first report id, e.g.
 * "keyboard id 1, consumer control id 3, vendor (page 0xff00 usage 0x1) id 6". */
static void summarise_report(struct hid *h, uint32_t n, char *out, uint32_t cap)
{
    const uint8_t *d = h->buf;
    struct text t = { out, 0, cap };
    uint32_t page = 0, usage = 0, depth = 0, count = 0;
    bool want_id = false;
    out[0] = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xfe) {                          /* long item */
            i += 3u + (i + 1 < n ? d[i + 1] : 0);
            continue;
        }
        uint32_t size = (b & 3) == 3 ? 4 : (b & 3), v = 0;
        if (i + 1 + size > n)
            break;
        for (uint32_t k = 0; k < size; k++)
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x04: page = v; break;               /* Usage Page */
        case 0x08:                                /* Usage (a 4-byte one names its page) */
            usage = size == 4 ? v & 0xffff : v;
            if (size == 4)
                page = v >> 16;
            break;
        case 0xa0:                                /* Collection */
            if (depth++ == 0 && v == 1) {         /* a top-level application one */
                if (count++)
                    put_str(&t, ", ");
                const char *nm = collection_name(page, usage);
                put_str(&t, nm);
                if (nm[0] == 'v' || nm[0] == 'o') {
                    put_str(&t, " (page ");
                    put_num(&t, page, 16);
                    put_str(&t, " usage ");
                    put_num(&t, usage, 16);
                    put_str(&t, ")");
                }
                want_id = true;
            }
            break;
        case 0xc0:                                /* End Collection */
            if (depth && !--depth)
                want_id = false;
            break;
        case 0x84:                                /* Report ID */
            if (want_id) {
                put_str(&t, " id ");
                put_num(&t, v, 10);
                want_id = false;
            }
            break;
        }
        i += 1 + size;
    }
    if (!count)
        put_str(&t, "no collections");
}

/* ---- start ------------------------------------------------------------------ */

static status_t class_out(struct hid *h, uint8_t request, uint16_t value)
{
    uint8_t none[64] = { 0 };
    return usb_control_out_until(h->usb, deadline(USB_TIMEOUT), REQ_OUT_CLASS_IFACE, request,
                                 value, h->iface, 0, none);
}

/* 0..: the driver ends with that code; -1: serve reports. */
static int setup(struct hid *h)
{
    uint8_t cls, nep;
    status_t st = usb_info_until(h->usb, deadline(USB_TIMEOUT), &h->vendor, &h->product,
                                 &h->speed, &h->iface, &cls, &h->subclass, &h->protocol, &nep,
                                 &h->alt, NULL);
    if (gone(h, st)) {
        drv_log("hid: the device went away before it started: device gone");
        return 0;
    }
    if (st != OK) {
        drv_log("hid: usb.info failed (%s)", status_str(st));
        return 3;
    }
    if (cls != 3) {
        drv_log("hid %04x:%04x if %u: class %u is not HID: nothing to do", h->vendor,
                h->product, h->iface, cls);
        return 2;
    }

    uint16_t n = 0;
    st = usb_get_descriptor_until(h->usb, deadline(USB_TIMEOUT), DESC_CONFIG, 0, 0, 9, 0, &n,
                                  h->buf);
    if (st == OK && n >= 4) {
        uint16_t total = (uint16_t)(h->buf[2] | h->buf[3] << 8);
        st = usb_get_descriptor_until(h->usb, deadline(USB_TIMEOUT), DESC_CONFIG, 0, 0,
                                      total < BUF_SIZE ? total : BUF_SIZE, 0, &n, h->buf);
    } else if (st == OK) {
        st = ERR_OUT_OF_RANGE;   /* shorter than its header */
    }
    if (gone(h, st))
        return 0;
    if (st != OK) {
        drv_log("hid %04x:%04x if %u: no configuration descriptor (%s)", h->vendor, h->product,
                h->iface, status_str(st));
        return 3;
    }
    parse_config(h, n);

    char coll[160];
    coll[0] = 0;
    if (h->report_desc_len) {
        uint16_t want = h->report_desc_len < BUF_SIZE ? h->report_desc_len : BUF_SIZE;
        /* wIndex of an interface-recipient GET_DESCRIPTOR is the
         * interface; `lang` carries it too for a usb-bus that passes it. */
        st = usb_get_descriptor_until(h->usb, deadline(USB_TIMEOUT), DESC_REPORT, 0, h->iface,
                                      want, 1, &n, h->buf);
        if (gone(h, st))
            return 0;
        if (st == OK)
            summarise_report(h, n, coll, sizeof(coll));
        else
            drv_log("hid %04x:%04x if %u: report descriptor unreadable (%s): going on",
                    h->vendor, h->product, h->iface, status_str(st));
    }

    bool boot = h->subclass == 1 && (h->protocol == 1 || h->protocol == 2);
    if (!boot) {
        drv_log("hid %04x:%04x if %u: subclass %u protocol %u (%s): not a boot keyboard/mouse: "
                "skipped", h->vendor, h->product, h->iface, h->subclass, h->protocol,
                coll[0] ? coll : "no report descriptor");
        return 0;
    }
    h->kind = h->protocol == 1 ? HID_KEYBOARD : HID_MOUSE;
    const char *what = h->kind == HID_KEYBOARD ? "keyboard" : "mouse";
    if (!h->ep_in) {
        drv_log("hid %04x:%04x if %u: boot %s without an interrupt IN endpoint", h->vendor,
                h->product, h->iface, what);
        return 3;
    }

    st = class_out(h, HID_SET_PROTOCOL, HID_PROTOCOL_BOOT);
    if (gone(h, st))
        return 0;
    if (st != OK)
        drv_log("hid %04x:%04x if %u: SET_PROTOCOL(boot) failed (%s): going on", h->vendor,
                h->product, h->iface, status_str(st));
    if (h->kind == HID_KEYBOARD) {
        st = class_out(h, HID_SET_IDLE, 0);
        if (gone(h, st))
            return 0;
        if (st != OK)
            drv_log("hid %04x:%04x if %u: SET_IDLE(0) failed (%s): going on", h->vendor,
                    h->product, h->iface, status_str(st));
    }

    uint16_t maxp = 0;
    st = usb_open_interrupt_in_until(h->usb, deadline(USB_TIMEOUT), h->ep_in, &h->reports, &maxp,
                                     &h->interval_ms);
    if (gone(h, st))
        return 0;
    if (st != OK) {
        drv_log("hid %04x:%04x if %u: open_interrupt_in(0x%x) failed (%s)", h->vendor,
                h->product, h->iface, h->ep_in, status_str(st));
        return 3;
    }
    if (maxp)
        h->max_packet = maxp;
    if (h->kind == HID_KEYBOARD) {
        kbd_init(h);
        if (h->stop)
            return 0;
    }
    /* In the RESULTS box too: which keyboards and mice are live. */
    drv_log("hid %04x:%04x if %u: boot %s: endpoint 0x%x, %u-byte packets every %u ms, "
            "report descriptor %u bytes (%s)", h->vendor, h->product, h->iface, what, h->ep_in,
            h->max_packet, h->interval_ms, h->report_desc_len, coll[0] ? coll : "-");
    /* Short: the RESULTS box is 120 columns. */
    say_result("hid %04x:%04x if %u: boot %s ready%s", h->vendor, h->product, h->iface, what,
               h->input == HANDLE_INVALID ? " (no console: keys go to the log)" : "");
    return -1;
}

/* ---- reports ----------------------------------------------------------------- */

static void take_report(struct hid *h, const uint8_t *r, uint32_t n)
{
    h->nreports++;
    if (h->kind == HID_KEYBOARD)
        kbd_report(h, r, n, drv_clock_ns());
    else
        mouse_report(h, r, n);
}

/* Every report queued (the port binding fires on the edge to readable, so
 * the channel is emptied each time). */
static void drain(struct hid *h)
{
    while (!h->stop) {
        uint32_t n = 0, nh = 0;
        handle_t hs[4];
        status_t st = drv_channel_read(h->reports, h->buf, BUF_SIZE, &n, hs, 4, &nh);
        if (st == ERR_BUFFER_TOO_SMALL) {   /* not a report of ours: take it off anyway */
            uint8_t *big = drv_malloc(n);
            handle_t *bh = drv_malloc((nh ? nh : 1) * sizeof(handle_t));
            uint32_t n2 = 0, nh2 = 0;
            if (big && bh && drv_channel_read(h->reports, big, n, &n2, bh, nh, &nh2) == OK)
                for (uint32_t i = 0; i < nh2; i++)
                    drv_handle_close(bh[i]);
            drv_free(bh);
            drv_free(big);
            if (!big || !bh)
                return;
            continue;
        }
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st == ERR_PEER_CLOSED) {
            reports_closed(h);
            return;
        }
        if (st != OK) {
            drv_log("hid %04x:%04x if %u: reading reports: %s", h->vendor, h->product,
                    h->iface, status_str(st));
            h->stop = STOP_DEVICE_GONE;
            return;
        }
        for (uint32_t i = 0; i < nh; i++)
            drv_handle_close(hs[i]);
        take_report(h, h->buf, n);
    }
}

static int run(struct hid *h)
{
    status_t st = drv_port_create(&h->port);
    if (st == OK)
        st = drv_port_bind(h->port, h->reports, K_REPORTS, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = drv_port_bind(h->port, h->usb, K_USB, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st == OK && h->input != HANDLE_INVALID)
        st = drv_port_bind(h->port, h->input, K_INPUT, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st != OK) {
        drv_log("hid %04x:%04x if %u: port: %s", h->vendor, h->product, h->iface,
                status_str(st));
        return 1;
    }
    while (!h->stop) {
        struct port_packet pkt;
        st = drv_port_wait(h->port, kbd_repeat_deadline(h), &pkt);
        if (st == OK) {
            if (pkt.key == K_REPORTS)
                drain(h);
            else if (pkt.key == K_USB)
                h->stop = STOP_DEVICE_GONE;
            else if (pkt.key == K_INPUT)
                h->stop = STOP_CONSOLE_GONE;
        } else if (st != ERR_TIMED_OUT) {
            drv_log("hid %04x:%04x if %u: port wait: %s", h->vendor, h->product, h->iface,
                    status_str(st));
            return 1;
        }
        kbd_repeat(h, drv_clock_ns());
    }
    return 0;
}

int driver_main(const struct driver_start *s)
{
    struct hid *h = drv_malloc(sizeof(*h));
    uint8_t *buf = drv_malloc(BUF_SIZE);
    if (!h || !buf) {
        drv_log("hid: out of memory");
        return 4;
    }
    *h = (struct hid){ 0 };
    h->buf = buf;
    to_results = s->name && s->name[0] == 'h' && s->name[1] == 'i' && s->name[2] == 'd' &&
                 s->name[3] == '-';
    h->usb = drv_handle(s, DR_USB);
    h->input = drv_handle(s, DR_INPUT);
    h->reports = h->port = HANDLE_INVALID;
    if (h->usb == HANDLE_INVALID) {
        drv_log("hid: no DR_USB channel: nothing to drive");
        return 1;
    }
    int r = setup(h);
    if (r < 0)
        r = run(h);
    if (r == 0 && h->stop == STOP_REPORTS_LOST) {
        drv_log("hid %04x:%04x if %u: usb-bus closed the reports channel but the device is "
                "still there: exit 5 (devmgr restarts hid)", h->vendor, h->product, h->iface);
        r = 5;
    }
    /* How it went, into the RESULTS box (the keytest boot entry counts
     * keys this way). */
    if (h->kind && r == 0) {
        drv_log("hid %04x:%04x if %u: %s after %lu report(s), %lu event(s) (%lu phantom, "
                "%lu short, %lu input error(s))", h->vendor, h->product, h->iface,
                h->stop == STOP_CONSOLE_GONE ? "console gone" : "device gone",
                (unsigned long)h->nreports, (unsigned long)h->events,
                (unsigned long)h->kbd.rollover, (unsigned long)h->kbd.short_reports,
                (unsigned long)h->input_errors);
        const char *why =
            h->stop == STOP_CONSOLE_GONE ? "console gone" : "unplugged or usb-bus stopped";
        if (h->kind == HID_KEYBOARD)
            say_result("hid %04x:%04x if %u: boot keyboard: %lu key(s) down, %lu report(s) (%s)",
                       h->vendor, h->product, h->iface, (unsigned long)h->keys_down,
                       (unsigned long)h->nreports, why);
        else
            say_result("hid %04x:%04x if %u: boot mouse: %lu report(s) (%s)", h->vendor,
                       h->product, h->iface, (unsigned long)h->nreports, why);
    }
    if (h->port != HANDLE_INVALID)
        drv_handle_close(h->port);
    if (h->reports != HANDLE_INVALID)
        drv_handle_close(h->reports);
    drv_handle_close(h->usb);
    if (h->input != HANDLE_INVALID)
        drv_handle_close(h->input);
    drv_free(buf);
    drv_free(h);
    return r;
}
