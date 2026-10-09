/* hid: the keyboard layer. A boot keyboard report is 8 bytes:
 * the modifier byte (INPUT_MOD_*), a reserved byte, and up to six usages of
 * the Keyboard/Keypad page (0x07) that are down, 0 in the empty slots.
 * Events come from diffing each report against the keys held before it:
 *
 *   1. modifiers newly down (usages 0xe0..0xe7, DOWN);
 *   2. keys no longer in the report (UP);
 *   3. modifiers released (UP);
 *   4. keys new in the report (DOWN).
 * Every event carries the modifier byte as it is at that point, so a key
 * pressed in the same report that lets go of Shift is unshifted (the
 * report is the state at one instant; the new key's DOWN comes last).
 *
 * Phantom state: a keyboard that sees more keys than it can tell apart
 * fills every slot with ErrorRollOver (0x01); 0x02 (POSTFail) and 0x03
 * (ErrorUndefined) mean the same kind of thing. Such a report is ignored
 * whole (modifiers too): the keys held before it stay held, as Linux does.
 *
 * Each key goes as its usage alone (input.idl): no character, no layout,
 * no repeats (M12's Q15 and review W6: the layout is libos's keymap, which
 * the compositor's clients decode with, and they repeat keys themselves),
 * and the keypad's keys as themselves (Num Lock is the compositor's
 * to apply). Ctrl+Alt+Delete is not special here: usage 0x4c DOWN with
 * CTRL and ALT bits in mods; the compositor decides what it means.
 *
 * Lock keys (Caps Lock 0x39, Num Lock 0x53, Scroll Lock 0x47) are keys
 * like any other: the compositor keeps the locks (review W7) and sends the
 * keyboard its lights (input.leds), which kbd_leds sets (SET_REPORT). The
 * keyboard starts dark. */
#include "hid.h"

#define U_LCTRL 0xe0

void kbd_init(struct hid *h)
{
    struct kbd *k = &h->kbd;
    k->mods = 0;
    k->leds = 0;
    k->nheld = 0;
    hid_set_leds(h, k->leds);   /* dark until the compositor says otherwise */
}

void kbd_leds(struct hid *h, uint8_t leds)
{
    leds &= INPUT_LED_NUM | INPUT_LED_CAPS | INPUT_LED_SCROLL;
    if (h->kind != HID_KEYBOARD || leds == h->kbd.leds)
        return;
    h->kbd.leds = leds;
    hid_set_leds(h, leds);
}

/* The usages in report r (n bytes, slots 2..7), each once, into keys;
 * how many. -1: a phantom report (see the header), to be ignored whole. */
static int report_keys(const uint8_t *r, uint32_t n, uint8_t keys[6])
{
    int nk = 0;
    for (uint32_t i = 2; i < n && i < 8; i++) {
        uint8_t u = r[i];
        if (u >= 0x01 && u <= 0x03)
            return -1;
        bool dup = u == 0;
        for (int j = 0; j < nk && !dup; j++)
            dup = keys[j] == u;
        if (!dup)
            keys[nk++] = u;
    }
    return nk;
}

/* Step 1 (down) or 3 (up): each modifier of `want` that changes from
 * *cur, sent with the modifier byte as it is after it. */
static void mods_change(struct hid *h, uint8_t want, uint8_t *cur, bool down)
{
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (down && (want & bit) && !(*cur & bit)) {
            *cur |= bit;
            hid_key(h, (uint16_t)(U_LCTRL + i), INPUT_KEY_DOWN, *cur);
        } else if (!down && !(want & bit) && (*cur & bit)) {
            *cur &= (uint8_t)~bit;
            hid_key(h, (uint16_t)(U_LCTRL + i), INPUT_KEY_UP, *cur);
        }
    }
}

/* Step 2: the keys held that are not in keys[0..nk) go UP. */
static void keys_released(struct hid *h, const uint8_t *keys, int nk, uint8_t cur)
{
    struct kbd *k = &h->kbd;
    for (uint32_t j = k->nheld; j-- > 0;) {
        bool still = false;
        for (int i = 0; i < nk && !still; i++)
            still = keys[i] == k->held[j];
        if (still)
            continue;
        uint8_t gone = k->held[j];
        for (uint32_t m = j; m + 1 < k->nheld; m++)
            k->held[m] = k->held[m + 1];
        k->nheld--;
        hid_key(h, gone, INPUT_KEY_UP, cur);
    }
}

/* Step 4, for one key of the report: DOWN if it is new. */
static void key_pressed(struct hid *h, uint8_t raw, uint8_t cur)
{
    struct kbd *k = &h->kbd;
    for (uint32_t j = 0; j < k->nheld; j++)
        if (k->held[j] == raw)
            return;
    if (k->nheld == 6)
        return;
    k->held[k->nheld++] = raw;
    hid_key(h, raw, INPUT_KEY_DOWN, cur);
}

void kbd_report(struct hid *h, const uint8_t *r, uint32_t n)
{
    struct kbd *k = &h->kbd;
    if (n < 3) {
        k->short_reports++;
        return;
    }
    uint8_t keys[6];
    int nk = report_keys(r, n, keys);
    if (nk < 0) {
        k->rollover++;   /* phantom state: ignore the report */
        return;
    }
    uint8_t cur = k->mods;
    mods_change(h, r[0], &cur, true);    /* 1. modifiers down */
    keys_released(h, keys, nk, cur);     /* 2. keys released */
    mods_change(h, r[0], &cur, false);   /* 3. modifiers up */
    for (int i = 0; i < nk; i++)         /* 4. keys pressed */
        key_pressed(h, keys[i], cur);
    k->mods = cur;
}
