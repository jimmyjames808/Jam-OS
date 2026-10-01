/* hid: the mouse layer. A mouse speaks one of two protocols (USB HID 1.11,
 * 7.2.5 and appendix B.2):
 *
 *   - boot: every report is the same 3 bytes, the buttons (bit 0 left, 1
 *     right, 2 middle; higher bits are extra buttons, not passed on), then
 *     X and Y as signed 8-bit counts moved since the last report. There is
 *     no wheel in it. Some devices send more bytes anyway, and many (QEMU's
 *     usb-mouse among them) put the wheel in a fourth one, which is taken
 *     when it is there; real mice mostly send exactly 3.
 *   - report: the layout the device's report descriptor gives (report.c):
 *     wider X and Y (12 or 16 bits on gaming mice), the wheel, the
 *     horizontal wheel (AC Pan), a report id byte first if the interface
 *     has several kinds of report (a keyboard and a mouse on one
 *     interface). Only the mouse's own reports are taken.
 *
 * mouse_choose picks the report protocol for a mouse whose descriptor
 * parses (and the parser's self-test passes) and has a wheel; anything
 * else stays in the boot protocol, as does every mouse with the boot word
 * hidboot (the way back if a mouse misbehaves in report protocol). The
 * input protocol has no horizontal wheel: AC Pan is found and logged, not
 * sent. The wheel's sign is the HID one, which is what the console and
 * the apps expect: + is a notch away from the user (scroll up).
 *
 * For a mouse from another machine, the log has all it takes to see what
 * happened: its report descriptor in hex, the layout found (or why none),
 * the protocol chosen, and its first report in hex. fixtures.c takes the
 * descriptor lines as they are.
 *
 * An event goes to the console only when something changed: a report
 * with no movement, no wheel and the same buttons as the last one sends
 * nothing (some mice repeat their last report). */
#include "hid.h"

#define DUMP_PER_LINE 40   /* descriptor bytes per log line: 3 characters each, and the
                            * kernel splits log lines at 200 */
#define DUMP_LINES    8    /* lines at most: 320 bytes; the rest is counted, not shown */
#define FIRST_REPORT  16   /* bytes of the first report logged */

/* n bytes of p as "05 01 09 ..." into out (cap bytes with the NUL). */
static void hex(const uint8_t *p, uint32_t n, char *out, uint32_t cap)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t len = 0;
    for (uint32_t i = 0; i < n && len + 4 <= cap; i++) {
        if (i)
            out[len++] = ' ';
        out[len++] = digits[p[i] >> 4];
        out[len++] = digits[p[i] & 15];
    }
    out[len] = 0;
}

/* The report descriptor in hex, DUMP_PER_LINE bytes a line. */
static void dump_descriptor(const struct hid *h)
{
    uint32_t n = h->report_desc_read, lines = (n + DUMP_PER_LINE - 1) / DUMP_PER_LINE;
    char text[3 * DUMP_PER_LINE + 1];
    for (uint32_t k = 0; k < lines && k < DUMP_LINES; k++) {
        uint32_t at = k * DUMP_PER_LINE, len = n - at < DUMP_PER_LINE ? n - at : DUMP_PER_LINE;
        hex(h->buf + at, len, text, sizeof(text));
        drv_log("hid %04x:%04x if %u: report descriptor %u/%u (%u bytes): %s", h->vendor,
                h->product, h->iface, k + 1, lines, n, text);
    }
    if (lines > DUMP_LINES)
        drv_log("hid %04x:%04x if %u: report descriptor: %u more bytes not shown", h->vendor,
                h->product, h->iface, n - DUMP_LINES * DUMP_PER_LINE);
}

/* " x 16 at 8 signed" for one field into out. */
static void field_str(const char *name, const struct rd_field *f, char *out, uint32_t cap)
{
    if (!f->size)
        drv_snprintf(out, cap, ", no %s", name);
    else
        drv_snprintf(out, cap, ", %s %u at %u%s", name, f->size, f->bit,
                     f->is_signed ? "" : " unsigned");
}

/* The layout found, in one line, with the protocol chosen. */
static void log_layout(const struct hid *h, const char *verdict)
{
    const struct rd_mouse *m = &h->layout;
    char x[40], y[40], w[40], p[40], id[16];
    field_str("x", &m->x, x, sizeof(x));
    field_str("y", &m->y, y, sizeof(y));
    field_str("wheel", &m->wheel, w, sizeof(w));
    field_str("pan", &m->pan, p, sizeof(p));
    if (m->report_id)
        drv_snprintf(id, sizeof(id), "id %u", m->report_id);
    else
        drv_snprintf(id, sizeof(id), "no id");
    drv_log("hid %04x:%04x if %u: mouse report: %s, %u bits; %u buttons%s%s%s%s: %s", h->vendor,
            h->product, h->iface, id, m->bits, m->nbuttons, x, y, w, p, verdict);
}

void mouse_choose(struct hid *h)
{
    h->report_mode = false;
    if (!h->report_desc_read) {
        drv_log("hid %04x:%04x if %u: no report descriptor: boot protocol", h->vendor,
                h->product, h->iface);
        return;
    }
    dump_descriptor(h);
    if (h->report_desc_read < h->report_desc_len) {
        drv_log("hid %04x:%04x if %u: report descriptor: %u of %u bytes read: boot protocol",
                h->vendor, h->product, h->iface, h->report_desc_read, h->report_desc_len);
        return;
    }
    enum rd_status st = hid_rd_parse(h->buf, h->report_desc_read, &h->layout);
    if (st != RD_OK) {
        drv_log("hid %04x:%04x if %u: report descriptor: %s: boot protocol", h->vendor,
                h->product, h->iface, hid_rd_status_str(st));
        return;
    }
    if (!h->layout.wheel.size) {
        log_layout(h, "no wheel: boot protocol");
        return;
    }
    if (h->force_boot) {
        log_layout(h, "boot protocol (hidboot)");
        return;
    }
    if (!hid_rd_selftest(false)) {   /* its failures are in the log */
        log_layout(h, "the parser's self-test failed: boot protocol");
        return;
    }
    log_layout(h, "report protocol");
    h->report_mode = true;
}

/* The first report in hex: what the mouse really sends. */
static void log_first(const struct hid *h, const uint8_t *r, uint32_t n)
{
    char text[3 * FIRST_REPORT + 1];
    hex(r, n < FIRST_REPORT ? n : FIRST_REPORT, text, sizeof(text));
    drv_log("hid %04x:%04x if %u: first report (%u bytes, %s protocol): %s", h->vendor,
            h->product, h->iface, n, h->report_mode ? "report" : "boot", text);
}

/* Send what moved, if anything did. */
static void moved(struct hid *h, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    if (!dx && !dy && !wheel && buttons == h->mouse_buttons)
        return;
    uint8_t was = h->mouse_buttons;
    h->mouse_buttons = buttons;
    hid_mouse(h, dx, dy, wheel, buttons, buttons != was);
}

void mouse_report(struct hid *h, const uint8_t *r, uint32_t n)
{
    if (h->nreports == 1)
        log_first(h, r, n);
    if (h->report_mode) {
        struct rd_move m;
        enum rd_decoded got = hid_rd_decode(&h->layout, r, n, &m);
        if (got == RD_OTHER_ID)
            h->other_reports++;
        else if (got == RD_SHORT)
            h->kbd.short_reports++;
        else
            moved(h, m.dx, m.dy, m.wheel, m.buttons);
        return;
    }
    if (n < 3) {
        h->kbd.short_reports++;
        return;
    }
    moved(h, (int8_t)r[1], (int8_t)r[2], n >= 4 ? (int8_t)r[3] : 0, r[0] & 0x07);
}
