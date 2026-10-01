/* hid: the mouse layer. A boot mouse report (HID 1.11 appendix B.2) is 3
 * bytes: the buttons (bit 0 left, 1 right, 2 middle; higher bits are
 * extra buttons, not passed on), X and Y as signed 8-bit counts moved
 * since the last report; a device may send more bytes, and many (QEMU's
 * usb-mouse among them) put the wheel in a fourth one, which is taken
 * when it is there.
 *
 * An event goes to the console only when something changed: a report
 * with no movement, no wheel and the same buttons as the last one sends
 * nothing (some mice repeat their last report). */
#include "hid.h"

void mouse_report(struct hid *h, const uint8_t *r, uint32_t n)
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
    hid_mouse(h, dx, dy, wheel, buttons, buttons != was);
}
