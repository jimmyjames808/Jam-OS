/* hid: the USB HID class driver. One process per HID
 * interface: devmgr starts it when usb-bus reports an interface of class 3,
 * so each interface of a composite device (a keyboard with a media-key
 * interface, a keyboard+mouse receiver) gets a hid process of its own.
 * It is drv/hid in bootfs, a process like every driver.
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_USB    the `usb` channel for this one interface (abi/idl/usb.idl);
 *             usb-bus closes it when the device is unplugged
 *   DR_INPUT  an `input` channel to the console (abi/idl/input.idl).
 *             Optional: without it every key DOWN (and mouse button change)
 *             goes to the log instead, a bring-up aid for a PC run that
 *             has usb-bus but no console yet
 *
 * Args (from devmgr, <jam/driver.h> drv_has_arg): "hidboot" keeps every
 * mouse in the boot protocol (the boot word of that name); "selftest" (no
 * handles needed) runs the report descriptor parser's self-test with
 * fuzzing and exits 0 if it passed, 6 if not.
 *
 * Start: usb.info (class 3, or it exits 2), the configuration descriptor
 * (this interface's HID descriptor: the report descriptor's length; its
 * interrupt IN endpoint), the report descriptor (read because some devices
 * only behave once it has been, as Windows and Linux always read it; its
 * top-level collections are logged). Then:
 *   - subclass 1 (boot), protocol 1 keyboard: SET_PROTOCOL(boot),
 *     SET_IDLE(0) (report only on change) and the LED byte (Num Lock on),
 *     then open_interrupt_in and serve reports. A failed SET_PROTOCOL or
 *     SET_IDLE is logged and ignored (many devices stall SET_IDLE; a
 *     device that refuses SET_PROTOCOL was already in boot mode or can't
 *     leave it);
 *   - subclass 1, protocol 2 mouse: its report descriptor is logged in hex
 *     and parsed (mouse.c, report.c). A mouse with a wheel in it goes to
 *     the report protocol (SET_PROTOCOL(report); refused: back to boot),
 *     since in the boot protocol a real mouse sends no wheel at all; any
 *     other mouse, and every mouse with "hidboot", gets SET_PROTOCOL(boot)
 *     as a keyboard does (no SET_IDLE, no LEDs);
 *   - anything else (subclass 0: consumer control, system control, a
 *     vendor interface, an NKRO keyboard in report protocol): logged as
 *     "not a boot keyboard/mouse: skipped" with its collections, exit 0.
 *     Keyboards that have such a second interface still type through
 *     their boot interface (every one the PC has is boot capable: the
 *     firmware uses them), so keyboards are never driven in report
 *     protocol.
 *
 * Reports arrive as one message each on the channel open_interrupt_in
 * returns. The loop waits on a port for that channel (readable or closed),
 * DR_USB closed and DR_INPUT closed, with the next key repeat as deadline.
 * The keyboard layer is keyboard.c, the mouse layer mouse.c (input.mouse,
 * sent only when something changed); the descriptors are walked in
 * desc.c.
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
 * lost with the device still there; 6: the self-test ("selftest") failed.
 *
 * Every call is bounded: USB requests USB_TIMEOUT, input calls
 * INPUT_TIMEOUT (a late console costs that event, logged, not the
 * driver). */
#include <idl/input.h>
#include <idl/usb.h>
#include "hid.h"

#define USB_TIMEOUT   (2000 * NS_PER_MS)
#define INPUT_TIMEOUT (2000 * NS_PER_MS)
#define REPORTS_GRACE (50 * NS_PER_MS)

#define REQ_OUT_CLASS_IFACE 0x21   /* host to device, class, interface */
#define HID_SET_REPORT      0x09
#define HID_SET_IDLE        0x0a
#define HID_SET_PROTOCOL    0x0b
#define HID_REPORT_OUTPUT   0x02
#define HID_PROTOCOL_BOOT   0
#define HID_PROTOCOL_REPORT 1

#define K_REPORTS 1
#define K_USB     2
#define K_INPUT   3


/* The "ready" and final lines go to the RESULTS box when devmgr started
 * us for a real interface: it names each hid after it ("hid-10:0"); utest's
 * mock runs are plain "hid" and stay in the log. */
static bool to_results;

static void say_result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_result(const char *fmt, ...)
{
    char line[256];   /* a log line's size: drv_log and drv_report cut there too */
    va_list ap;
    va_start(ap, fmt);
    drv_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (to_results)
        drv_report("%s", line);
    else
        drv_log("%s", line);
}

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

void hid_mouse(struct hid *h, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons,
               bool buttons_changed)
{
    if (h->input == HANDLE_INVALID) {
        if (buttons_changed)
            drv_log("hid %04x:%04x if %u: mouse buttons 0x%x", h->vendor, h->product, h->iface,
                    buttons);
        h->events++;
        return;
    }
    input_result(h, input_mouse_until(h->input, deadline(INPUT_TIMEOUT), dx, dy, wheel, buttons),
                 "mouse");
}

/* ---- start ------------------------------------------------------------------ */

static status_t class_out(struct hid *h, uint8_t request, uint16_t value)
{
    uint8_t none[64] = { 0 };
    return usb_control_out_until(h->usb, deadline(USB_TIMEOUT), REQ_OUT_CLASS_IFACE, request,
                                 value, h->iface, 0, none);
}

/* The steps of setup() return what it does: 0..: the driver ends with
 * that code; -1: go on. */

/* usb.info: the device's ids and our interface. */
static int read_info(struct hid *h)
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
    return -1;
}

/* The configuration descriptor: our HID descriptor and endpoint. */
static int read_config(struct hid *h)
{
    uint16_t n = 0;
    status_t st = usb_get_descriptor_until(h->usb, deadline(USB_TIMEOUT), DESC_CONFIG, 0, 0, 9,
                                           0, &n, h->buf);
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
    hid_parse_config(h, n < BUF_SIZE ? n : BUF_SIZE);   /* whatever usb-bus says, buf ends there */
    return -1;
}

/* The report descriptor, if the HID descriptor names one: its summary
 * into coll (cap bytes; "" without one). Unreadable is logged, not fatal. */
static int read_report_desc(struct hid *h, char *coll, uint32_t cap)
{
    coll[0] = 0;
    if (!h->report_desc_len)
        return -1;
    uint16_t want = h->report_desc_len < BUF_SIZE ? h->report_desc_len : BUF_SIZE, n = 0;
    /* wIndex of an interface-recipient GET_DESCRIPTOR is the
     * interface; `lang` carries it too for a usb-bus that passes it. */
    status_t st = usb_get_descriptor_until(h->usb, deadline(USB_TIMEOUT), DESC_REPORT, 0,
                                           h->iface, want, 1, &n, h->buf);
    if (gone(h, st))
        return 0;
    if (st == OK) {
        h->report_desc_read = n < want ? n : want;   /* never past what was asked (and buf) */
        hid_summarise_report(h, h->report_desc_read, coll, cap);
    } else {
        drv_log("hid %04x:%04x if %u: report descriptor unreadable (%s): going on",
                h->vendor, h->product, h->iface, status_str(st));
    }
    return -1;
}

/* A mouse mouse_choose put in report protocol: SET_PROTOCOL(report). A
 * device refusing it may still be in the boot protocol the firmware left
 * it in, whose reports the report layout would misread: it goes back to
 * the boot protocol. */
static int report_protocol(struct hid *h)
{
    status_t st = class_out(h, HID_SET_PROTOCOL, HID_PROTOCOL_REPORT);
    if (gone(h, st))
        return 0;
    if (st != OK) {
        drv_log("hid %04x:%04x if %u: SET_PROTOCOL(report) failed (%s): boot protocol",
                h->vendor, h->product, h->iface, status_str(st));
        h->report_mode = false;
    }
    return -1;
}

/* SET_PROTOCOL(boot), and for a keyboard SET_IDLE(0): a refusal is logged
 * and ignored. A mouse in report protocol gets SET_PROTOCOL(report)
 * instead. */
static int set_protocol(struct hid *h)
{
    if (h->report_mode) {
        int r = report_protocol(h);
        if (r >= 0 || h->report_mode)
            return r;
    }
    status_t st = class_out(h, HID_SET_PROTOCOL, HID_PROTOCOL_BOOT);
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
    return -1;
}

/* open_interrupt_in: the report channel; then the keyboard layer starts. */
static int open_reports(struct hid *h)
{
    uint16_t maxp = 0;
    status_t st = usb_open_interrupt_in_until(h->usb, deadline(USB_TIMEOUT), h->ep_in,
                                              &h->reports, &maxp, &h->interval_ms);
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
    return -1;
}

/* "boot keyboard", "boot mouse" or "report mouse" (the protocol it speaks). */
static const char *kind_name(const struct hid *h)
{
    if (h->kind == HID_KEYBOARD)
        return "boot keyboard";
    return h->report_mode ? "report mouse" : "boot mouse";
}

/* Tell the console that this keyboard or mouse works from now on (it logs
 * when). A console that doesn't know the call is fine: it is only for the
 * log. */
static void tell_ready(struct hid *h)
{
    if (h->input == HANDLE_INVALID)
        return;
    uint8_t kind = h->kind == HID_KEYBOARD ? INPUT_READY_KEYBOARD : INPUT_READY_MOUSE;
    status_t st = input_ready_until(h->input, deadline(INPUT_TIMEOUT), kind, h->vendor,
                                    h->product);
    if (st != OK && st != ERR_NOT_SUPPORTED)
        input_result(h, st, "ready");
}

/* 0..: the driver ends with that code; -1: serve reports. */
static int setup(struct hid *h)
{
    int r;
    if ((r = read_info(h)) >= 0 || (r = read_config(h)) >= 0)
        return r;
    char coll[160];
    if ((r = read_report_desc(h, coll, sizeof(coll))) >= 0)
        return r;
    bool boot = h->subclass == 1 && (h->protocol == 1 || h->protocol == 2);
    if (!boot) {
        drv_log("hid %04x:%04x if %u: subclass %u protocol %u (%s): not a boot keyboard/mouse: "
                "skipped", h->vendor, h->product, h->iface, h->subclass, h->protocol,
                coll[0] ? coll : "no report descriptor");
        return 0;
    }
    h->kind = h->protocol == 1 ? HID_KEYBOARD : HID_MOUSE;
    const char *what = kind_name(h);
    if (!h->ep_in) {
        drv_log("hid %04x:%04x if %u: %s without an interrupt IN endpoint", h->vendor,
                h->product, h->iface, what);
        return 3;
    }
    if (h->kind == HID_MOUSE)
        mouse_choose(h);   /* before the reports reuse h->buf */
    if ((r = set_protocol(h)) >= 0 || (r = open_reports(h)) >= 0)
        return r;
    what = kind_name(h);
    /* In the RESULTS box too: which keyboards and mice are live. */
    drv_log("hid %04x:%04x if %u: %s: endpoint 0x%x, %u-byte packets every %u ms, "
            "report descriptor %u bytes (%s)", h->vendor, h->product, h->iface, what, h->ep_in,
            h->max_packet, h->interval_ms, h->report_desc_len, coll[0] ? coll : "-");
    /* Short: the RESULTS box is 120 columns. */
    say_result("hid %04x:%04x if %u: %s ready%s", h->vendor, h->product, h->iface, what,
               h->input == HANDLE_INVALID ? " (no console: keys go to the log)" : "");
    tell_ready(h);
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

/* The next message on the reports channel didn't fit (n bytes, nh
 * handles): not a report of ours, but take it off anyway. False if there
 * was no memory to read it into. */
static bool drop_message(struct hid *h, uint32_t n, uint32_t nh)
{
    uint8_t *big = drv_malloc(n);
    handle_t *bh = drv_malloc((nh ? nh : 1) * sizeof(handle_t));
    bool have = big && bh;
    uint32_t n2 = 0, nh2 = 0;
    if (have && drv_channel_read(h->reports, big, n, &n2, bh, nh, &nh2) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            drv_handle_close(bh[i]);
    drv_free(bh);
    drv_free(big);
    return have;
}

/* Every report queued (the port binding fires on the edge to readable, so
 * the channel is emptied each time). */
static void drain(struct hid *h)
{
    while (!h->stop) {
        uint32_t n = 0, nh = 0;
        handle_t hs[4];
        status_t st = drv_channel_read(h->reports, h->buf, BUF_SIZE, &n, hs, 4, &nh);
        if (st == ERR_BUFFER_TOO_SMALL) {
            if (!drop_message(h, n, nh))
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
    /* `drv/hid selftest` (utest): the report descriptor parser's self-test
     * with fuzzing, nothing else. */
    if (drv_has_arg(s, "selftest"))
        return hid_rd_selftest(true) ? 0 : 6;
    struct hid *h = drv_malloc(sizeof(*h));
    uint8_t *buf = drv_malloc(BUF_SIZE);
    if (!h || !buf) {
        drv_log("hid: out of memory");
        return 4;
    }
    *h = (struct hid){ 0 };
    h->buf = buf;
    h->force_boot = drv_has_arg(s, "hidboot");
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
    /* How it went, into the RESULTS box (the hidden keytest boot word
     * counts keys this way). */
    if (h->kind && r == 0) {
        drv_log("hid %04x:%04x if %u: %s after %lu report(s), %lu event(s) (%lu phantom, "
                "%lu short, %lu of another id, %lu input error(s))", h->vendor, h->product,
                h->iface, h->stop == STOP_CONSOLE_GONE ? "console gone" : "device gone",
                (unsigned long)h->nreports, (unsigned long)h->events,
                (unsigned long)h->kbd.rollover, (unsigned long)h->kbd.short_reports,
                (unsigned long)h->other_reports, (unsigned long)h->input_errors);
        const char *why =
            h->stop == STOP_CONSOLE_GONE ? "console gone" : "unplugged or usb-bus stopped";
        if (h->kind == HID_KEYBOARD)
            say_result("hid %04x:%04x if %u: boot keyboard: %lu key(s) down, %lu report(s) (%s)",
                       h->vendor, h->product, h->iface, (unsigned long)h->keys_down,
                       (unsigned long)h->nreports, why);
        else
            say_result("hid %04x:%04x if %u: %s: %lu report(s) (%s)", h->vendor, h->product,
                       h->iface, kind_name(h), (unsigned long)h->nreports, why);
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
