/* libos: keys from a terminal's bytes (<termkeys.h>). A small state
 * machine over VT100's input sequences; pure, one struct termkeys per
 * terminal. */
#include <termkeys.h>

/* HID usages (USB HID Usage Tables 1.4, section 10) of the keys a terminal
 * names. */
#define U_ENTER     0x28
#define U_ESCAPE    0x29
#define U_BACKSPACE 0x2a
#define U_TAB       0x2b
#define U_HOME      0x4a
#define U_PAGE_UP   0x4b
#define U_DELETE    0x4c
#define U_END       0x4d
#define U_PAGE_DOWN 0x4e
#define U_RIGHT     0x4f
#define U_LEFT      0x50
#define U_DOWN      0x51
#define U_UP        0x52

static unsigned put(struct termkey *out, uint16_t usage, uint32_t cp)
{
    out->usage = usage;
    out->cp = cp;
    return 1;
}

/* The key of ESC [ <params> ~ (the number before any ';'), 0 for none. */
static uint16_t tilde_key(const struct termkeys *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < t->np && t->params[i] >= '0' && t->params[i] <= '9' && n < 1000; i++)
        n = n * 10 + (unsigned)(t->params[i] - '0');
    switch (n) {
    case 1: case 7: return U_HOME;
    case 4: case 8: return U_END;
    case 3: return U_DELETE;
    case 5: return U_PAGE_UP;
    case 6: return U_PAGE_DOWN;
    default: return 0;
    }
}

/* The key a sequence's final byte names, 0 for none. */
static uint16_t final_key(const struct termkeys *t, uint8_t final)
{
    switch (final) {
    case 'A': return U_UP;
    case 'B': return U_DOWN;
    case 'C': return U_RIGHT;
    case 'D': return U_LEFT;
    case 'H': return U_HOME;
    case 'F': return U_END;
    case '~': return tilde_key(t);
    default: return 0;
    }
}

/* An ordinary byte (no sequence open) as a key: 0 or 1 of them. */
static unsigned plain(struct termkeys *t, uint8_t b, struct termkey *out)
{
    bool cr = t->last_cr;
    t->last_cr = b == '\r';
    if (b == 0x1b) {
        t->esc = 1;
        return 0;
    }
    if (b == '\r' || (b == '\n' && !cr))
        return put(out, U_ENTER, '\n');
    if (b == '\n')
        return 0;   /* the LF of a CR LF */
    if (b == 0x7f || b == 0x08)
        return put(out, U_BACKSPACE, 0x08);
    if (b == '\t')
        return put(out, U_TAB, '\t');
    return put(out, 0, b);   /* printable, or a control character (Ctrl+C = 3) */
}

unsigned termkeys_byte(struct termkeys *t, uint8_t b, struct termkey out[2])
{
    if (t->esc == 1) {
        if (b == '[' || b == 'O') {
            t->esc = 2;
            t->np = 0;
            return 0;
        }
        t->esc = 0;   /* a lone ESC: the Escape key, then b as itself */
        unsigned n = put(&out[0], U_ESCAPE, 0x1b);
        return n + plain(t, b, &out[n]);
    }
    if (t->esc == 2) {
        if ((b >= '0' && b <= '9') || b == ';') {
            if (t->np < sizeof(t->params))
                t->params[t->np++] = (char)b;
            return 0;
        }
        t->esc = 0;
        uint16_t usage = final_key(t, b);
        return usage ? put(&out[0], usage, 0) : 0;
    }
    return plain(t, b, &out[0]);
}
