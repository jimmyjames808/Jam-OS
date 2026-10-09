/* libos: keys into the bytes xterm sends (<keybytes.h> has the table).
 * Pure: a key in, its bytes out. */
#include <idl/input.h>
#include <keybytes.h>

/* HID usages (USB HID Usage Tables 1.4, section 10). */
#define U_A         0x04
#define U_Z         0x1d
#define U_ENTER     0x28
#define U_ESCAPE    0x29
#define U_BACKSPACE 0x2a
#define U_TAB       0x2b
#define U_F1        0x3a
#define U_F12       0x45
#define U_INSERT    0x49
#define U_HOME      0x4a
#define U_PAGE_UP   0x4b
#define U_DELETE    0x4c
#define U_END       0x4d
#define U_PAGE_DOWN 0x4e
#define U_RIGHT     0x4f
#define U_LEFT      0x50
#define U_DOWN      0x51
#define U_UP        0x52
#define U_KP_ENTER  0x58
#define ESC         0x1b

/* xterm's modifier parameter: 1 + Shift 1 + Alt 2 + Ctrl 4 (1: none). */
static unsigned mod_param(uint8_t mods)
{
    return 1 + ((mods & INPUT_MOD_SHIFT) ? 1 : 0) + ((mods & INPUT_MOD_ALT) ? 2 : 0) +
           ((mods & INPUT_MOD_CTRL) ? 4 : 0);
}

static unsigned put_num(uint8_t *out, unsigned n, unsigned v)
{
    if (v >= 10)
        out[n++] = (uint8_t)('0' + v / 10);
    out[n++] = (uint8_t)('0' + v % 10);
    return n;
}

/* ESC [ <num> [; <m>] <final>, or for num 0 ESC [ [1 ; <m>] <final>
 * (ESC O <final> with ss3 and no modifiers). */
static unsigned csi(uint8_t *out, unsigned num, uint8_t mods, uint8_t final, bool ss3)
{
    unsigned n = 0, m = mod_param(mods);
    out[n++] = ESC;
    if (ss3 && m == 1) {
        out[n++] = 'O';
        out[n++] = final;
        return n;
    }
    out[n++] = '[';
    if (num || m > 1)
        n = put_num(out, n, num ? num : 1);
    if (m > 1) {
        out[n++] = ';';
        n = put_num(out, n, m);
    }
    out[n++] = final;
    return n;
}

/* The F-keys' numbers in ESC [ <n> ~ (F5 to F12; F1 to F4 are SS3). */
static const uint8_t fkey_num[12] = { 0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24 };

/* A key named by a sequence: its bytes, or 0 if usage isn't one. */
static unsigned special(uint16_t usage, uint8_t mods, bool app, uint8_t *out)
{
    static const uint8_t cursor[] = { 'C', 'D', 'B', 'A' };   /* Right Left Down Up */
    if (usage >= U_RIGHT && usage <= U_UP)
        return csi(out, 0, mods, cursor[usage - U_RIGHT], app);
    if (usage == U_HOME || usage == U_END)
        return csi(out, 0, mods, usage == U_HOME ? 'H' : 'F', app);
    if (usage == U_INSERT || usage == U_DELETE)
        return csi(out, usage == U_INSERT ? 2 : 3, mods, '~', false);
    if (usage == U_PAGE_UP || usage == U_PAGE_DOWN)
        return csi(out, usage == U_PAGE_UP ? 5 : 6, mods, '~', false);
    if (usage >= U_F1 && usage <= U_F12) {
        unsigned f = usage - U_F1;
        return f < 4 ? csi(out, 0, mods, (uint8_t)('P' + f), true)
                     : csi(out, fkey_num[f], mods, '~', false);
    }
    return 0;
}

/* The character cp as UTF-8 at out[n]: the new length. */
static unsigned utf8_put(uint8_t *out, unsigned n, uint32_t cp)
{
    if (cp < 0x80) {
        out[n++] = (uint8_t)cp;
    } else if (cp < 0x800) {
        out[n++] = (uint8_t)(0xc0 | cp >> 6);
        out[n++] = (uint8_t)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out[n++] = (uint8_t)(0xe0 | cp >> 12);
        out[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        out[n++] = (uint8_t)(0x80 | (cp & 0x3f));
    } else if (cp < 0x110000) {
        out[n++] = (uint8_t)(0xf0 | cp >> 18);
        out[n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
        out[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        out[n++] = (uint8_t)(0x80 | (cp & 0x3f));
    }
    return n;
}

/* The byte Ctrl makes of the key that types cp (or of letter usage): -1
 * for none. */
static int ctrl_byte(uint16_t usage, uint32_t cp)
{
    if (usage >= U_A && usage <= U_Z)
        return (int)(usage - U_A) + 1;
    if (cp >= 'a' && cp <= 'z')
        return (int)(cp - 'a') + 1;
    if (cp >= '@' && cp <= '_')   /* @ A..Z [ \ ] ^ _ */
        return (int)(cp - '@');
    if (cp == ' ' || cp == '2')
        return 0;
    if (cp == '?')
        return 0x7f;
    return -1;
}

/* A key that types a byte of its own (Enter, Backspace, Tab, Escape), or
 * -1. Shift+Tab is a sequence (out, *n). */
static int plain_key(uint16_t usage, uint8_t mods, uint8_t *out, unsigned *n)
{
    switch (usage) {
    case U_ENTER:
    case U_KP_ENTER:
        return '\r';
    case U_BACKSPACE:
        return (mods & INPUT_MOD_CTRL) ? 0x08 : 0x7f;
    case U_ESCAPE:
        return ESC;
    case U_TAB:
        if (mods & INPUT_MOD_SHIFT) {
            *n = csi(out, 0, 0, 'Z', false);
            return -2;
        }
        return '\t';
    default:
        return -1;
    }
}

unsigned keybytes(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool app_cursor,
                  uint8_t out[KEYBYTES_MAX])
{
    if (state == INPUT_KEY_UP)
        return 0;
    unsigned n = special(usage, mods, app_cursor, out);
    if (n)
        return n;
    unsigned at = 0;
    if (mods & INPUT_MOD_ALT)
        out[at++] = ESC;   /* Alt: ESC before the key's own bytes */
    int b = plain_key(usage, mods, out + at, &n);
    if (b == -2)
        return at + n;
    if (b < 0 && (mods & INPUT_MOD_CTRL))
        b = ctrl_byte(usage, cp);
    if (b >= 0) {
        out[at++] = (uint8_t)b;
        return at;
    }
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;   /* a modifier alone, or a key that types nothing */
    return utf8_put(out, at, cp);
}
