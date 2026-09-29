/* hid: the keyboard layer (M7 Track B). A boot keyboard report is 8 bytes:
 * the modifier byte (INPUT_MOD_*), a reserved byte, and up to six usages of
 * the Keyboard/Keypad page (0x07) that are down, 0 in the empty slots.
 * Events come from diffing each report against the keys held before it:
 *
 *   1. modifiers newly down (usages 0xe0..0xe7, DOWN);
 *   2. keys no longer in the report (UP, with the usage and codepoint their
 *      DOWN had);
 *   3. modifiers released (UP);
 *   4. keys new in the report (DOWN, codepoint from the US layout).
 * Every event carries the modifier byte as it is at that point, so a key
 * pressed in the same report that lets go of Shift is unshifted (the
 * report is the state at one instant; the new key's DOWN comes last).
 *
 * Phantom state: a keyboard that sees more keys than it can tell apart
 * fills every slot with ErrorRollOver (0x01); 0x02 (POSTFail) and 0x03
 * (ErrorUndefined) mean the same kind of thing. Such a report is ignored
 * whole (modifiers too): the keys held before it stay held, as Linux does.
 *
 * US layout (AU keyboards are US): letters (Shift XOR Caps Lock), the digit
 * row and every symbol key with and without Shift, space, and the keypad
 * (operators always; digits and '.' while Num Lock is on, otherwise the
 * keypad keys are sent as the navigation keys printed on them: KP8 as Up,
 * KP1 as End, KP0 as Insert, KP. as Delete, ...). Codepoints of the
 * control keys: Enter and keypad Enter '\n', Tab '\t', Backspace '\b',
 * Escape 0x1b. Everything else (arrows, Home/End, PgUp/PgDn, Insert,
 * Delete, F1-F24, Print Screen, Pause, the lock keys, the modifiers, the
 * Menu key, and any usage not listed here) is sent as its usage with
 * codepoint 0. Ctrl, Alt and GUI don't change the codepoint: Ctrl+C is
 * usage 0x06, codepoint 'c', mods with a CTRL bit; the console decides
 * what it means.
 *
 * Ctrl+Alt+Delete is not special here: it is usage 0x4c (Delete) DOWN with
 * CTRL and ALT bits in mods, sent like any other key. The console (or the
 * shell) decides what to do with it; the HID driver never reboots.
 *
 * Lock keys: Caps Lock (0x39), Num Lock (0x53) and Scroll Lock (0x47) are
 * sent as keys and also toggle the state here; each toggle sends the LED
 * byte to the keyboard (SET_REPORT). Num Lock starts on (its LED is set at
 * start), Caps Lock and Scroll Lock off.
 *
 * Key repeat: the most recently pressed key (not a modifier, not a lock
 * key) repeats while it is held: the first REPEAT 500 ms after its DOWN,
 * then one every 1/30 s. The repeat stops when that key is released
 * (another key still held doesn't take over, as on a PC). A REPEAT carries
 * the modifiers of the moment and the codepoint they give (hold 'a', then
 * press Shift: 'A' repeats). A late wakeup never sends a burst: the next
 * REPEAT is at least one period after the one just sent. */
#include "hid.h"

#define U_A          0x04
#define U_Z          0x1d
#define U_1          0x1e
#define U_SLASH      0x38
#define U_CAPS       0x39
#define U_SCROLL     0x47
#define U_NUMLOCK    0x53
#define U_KP_SLASH   0x54
#define U_KP_ENTER   0x58
#define U_KP_1       0x59
#define U_KP_DOT     0x63
#define U_NONUS_BSL  0x64
#define U_KP_EQUAL   0x67
#define U_LCTRL      0xe0

/* 0x1e..0x38: 1..0, Enter, Escape, Backspace, Tab, space, then the symbols. */
static const uint8_t row_plain[U_SLASH - U_1 + 1] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 0x1b, '\b', '\t', ' ',
    '-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/',
};
static const uint8_t row_shift[U_SLASH - U_1 + 1] = {
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0x1b, '\b', '\t', ' ',
    '_', '+', '{', '}', '|', '|', ':', '"', '~', '<', '>', '?',
};
/* 0x54..0x58: keypad / * - + Enter. */
static const uint8_t kp_ops[U_KP_ENTER - U_KP_SLASH + 1] = { '/', '*', '-', '+', '\n' };
/* 0x59..0x63: keypad 1..9, 0, '.' with Num Lock on ... */
static const uint8_t kp_num[U_KP_DOT - U_KP_1 + 1] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
};
/* ... and the key printed on them with it off (0: KP5, nothing). */
static const uint8_t kp_nav[U_KP_DOT - U_KP_1 + 1] = {
    0x4d /* End */, 0x51 /* Down */, 0x4e /* PgDn */, 0x50 /* Left */, 0,
    0x4f /* Right */, 0x4a /* Home */, 0x52 /* Up */, 0x4b /* PgUp */,
    0x49 /* Insert */, 0x4c /* Delete */,
};

/* The usage and codepoint key `raw` gives with these modifiers and LEDs. */
static void translate(uint8_t raw, uint8_t mods, uint8_t leds, uint16_t *usage, uint32_t *cp)
{
    bool shift = (mods & INPUT_MOD_SHIFT) != 0;
    *usage = raw;
    *cp = 0;
    if (raw >= U_A && raw <= U_Z) {
        bool upper = shift != ((leds & LED_CAPS) != 0);
        *cp = (uint32_t)((upper ? 'A' : 'a') + (raw - U_A));
    } else if (raw >= U_1 && raw <= U_SLASH) {
        *cp = shift ? row_shift[raw - U_1] : row_plain[raw - U_1];
    } else if (raw == U_NONUS_BSL) {
        *cp = shift ? '|' : '\\';
    } else if (raw >= U_KP_SLASH && raw <= U_KP_ENTER) {
        *cp = kp_ops[raw - U_KP_SLASH];
    } else if (raw >= U_KP_1 && raw <= U_KP_DOT) {
        if (leds & LED_NUM)
            *cp = kp_num[raw - U_KP_1];
        else if (kp_nav[raw - U_KP_1])
            *usage = kp_nav[raw - U_KP_1];
    } else if (raw == U_KP_EQUAL) {
        *cp = '=';
    }
}

static bool repeats(uint8_t raw)
{
    return raw < U_LCTRL && raw != U_CAPS && raw != U_NUMLOCK && raw != U_SCROLL;
}

void kbd_init(struct hid *h)
{
    struct kbd *k = &h->kbd;
    k->mods = 0;
    k->leds = LED_NUM;
    k->nheld = 0;
    k->repeating = false;
    hid_set_leds(h, k->leds);
}

void kbd_report(struct hid *h, const uint8_t *r, uint32_t n, uint64_t now)
{
    struct kbd *k = &h->kbd;
    if (n < 3) {
        k->short_reports++;
        return;
    }
    uint8_t keys[6];
    uint32_t nk = 0;
    for (uint32_t i = 2; i < n && i < 8; i++) {
        uint8_t u = r[i];
        if (u >= 0x01 && u <= 0x03) {   /* phantom state: ignore the report */
            k->rollover++;
            return;
        }
        bool dup = u == 0;
        for (uint32_t j = 0; j < nk && !dup; j++)
            dup = keys[j] == u;
        if (!dup)
            keys[nk++] = u;
    }

    uint8_t want = r[0], cur = k->mods;
    uint8_t leds = k->leds;
    for (int i = 0; i < 8; i++) {                 /* 1. modifiers down */
        uint8_t bit = (uint8_t)(1u << i);
        if ((want & bit) && !(cur & bit)) {
            cur |= bit;
            hid_key(h, (uint16_t)(U_LCTRL + i), INPUT_KEY_DOWN, cur, 0);
        }
    }
    for (uint32_t j = k->nheld; j-- > 0;) {       /* 2. keys released */
        bool still = false;
        for (uint32_t i = 0; i < nk && !still; i++)
            still = keys[i] == k->held[j].raw;
        if (still)
            continue;
        struct kbd_held gone = k->held[j];
        for (uint32_t m = j; m + 1 < k->nheld; m++)
            k->held[m] = k->held[m + 1];
        k->nheld--;
        if (k->repeating && k->rep_raw == gone.raw)
            k->repeating = false;
        hid_key(h, gone.usage, INPUT_KEY_UP, cur, gone.codepoint);
    }
    for (int i = 0; i < 8; i++) {                 /* 3. modifiers up */
        uint8_t bit = (uint8_t)(1u << i);
        if (!(want & bit) && (cur & bit)) {
            cur &= (uint8_t)~bit;
            hid_key(h, (uint16_t)(U_LCTRL + i), INPUT_KEY_UP, cur, 0);
        }
    }
    for (uint32_t i = 0; i < nk; i++) {           /* 4. keys pressed */
        bool held = false;
        for (uint32_t j = 0; j < k->nheld && !held; j++)
            held = k->held[j].raw == keys[i];
        if (held || k->nheld == 6)
            continue;
        uint8_t raw = keys[i];
        struct kbd_held nh = { .raw = raw };
        translate(raw, cur, k->leds, &nh.usage, &nh.codepoint);
        k->held[k->nheld++] = nh;
        if (raw == U_CAPS)
            k->leds ^= LED_CAPS;
        else if (raw == U_NUMLOCK)
            k->leds ^= LED_NUM;
        else if (raw == U_SCROLL)
            k->leds ^= LED_SCROLL;
        if (repeats(raw)) {
            k->repeating = true;
            k->rep_raw = raw;
            k->rep_usage = nh.usage;
            k->rep_next = now + REPEAT_DELAY_NS;
        }
        hid_key(h, nh.usage, INPUT_KEY_DOWN, cur, nh.codepoint);
    }
    k->mods = cur;
    if (k->leds != leds && !h->stop)
        hid_set_leds(h, k->leds);
}

uint64_t kbd_repeat_deadline(const struct hid *h)
{
    if (h->kind != HID_KEYBOARD || !h->kbd.repeating)
        return DEADLINE_NEVER;
    return h->kbd.rep_next;
}

void kbd_repeat(struct hid *h, uint64_t now)
{
    struct kbd *k = &h->kbd;
    if (h->kind != HID_KEYBOARD || !k->repeating || now < k->rep_next)
        return;
    uint16_t usage;
    uint32_t cp;
    translate(k->rep_raw, k->mods, k->leds, &usage, &cp);
    hid_key(h, k->rep_usage, INPUT_KEY_REPEAT, k->mods, cp);
    k->rep_next += REPEAT_PERIOD_NS;
    if (k->rep_next <= now)
        k->rep_next = now + REPEAT_PERIOD_NS;
}
