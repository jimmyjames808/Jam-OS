/* The markers of a bracketed paste, and what pasted keys put on the line
 * (sh_paste.h).
 *
 * The shell asks its terminal for bracketed paste (ESC [ ? 2004 h, main.c)
 * once at its start; from then on the console types pasted text between
 * ESC [ 2 0 0 ~ and ESC [ 2 0 1 ~ (user/services/console/paste.c), each
 * byte a key: ESC as the Escape key or the byte 0x1b, the rest as their
 * characters. The line editor takes everything in between as text for
 * the line, with line breaks and tabs as spaces, and runs nothing: the
 * safest simple choice, since a pasted command of several lines is never
 * run line by line and the user sees it all before pressing Enter. */
#include "sh_paste.h"

#define U_ESC 0x29   /* HID usage: Escape */

bool sh_paste_is_esc(const struct input_key_event *k)
{
    return k->usage == U_ESC || (!k->usage && k->codepoint == 0x1b);
}

enum sh_paste_mark sh_paste_marker(const struct input_key_event *k, unsigned n)
{
    static const char begin[] = "\033[200~", end[] = "\033[201~";
    bool b = true, e = true;
    if (!n || n > SH_PASTE_MARK_LEN || !sh_paste_is_esc(&k[0]))
        return SH_PASTE_NO;
    for (unsigned i = 1; i < n; i++) {
        uint32_t cp = k[i].codepoint;
        b = b && cp == (uint8_t)begin[i];
        e = e && cp == (uint8_t)end[i];
    }
    if (!b && !e)
        return SH_PASTE_NO;
    if (n < SH_PASTE_MARK_LEN)
        return SH_PASTE_MORE;
    return b ? SH_PASTE_BEGIN : SH_PASTE_END;
}

char sh_paste_byte(const struct input_key_event *k)
{
    uint32_t cp = k->codepoint;
    if (cp == '\n' || cp == '\r' || cp == '\t' || k->usage == 0x28 || k->usage == 0x58 ||
        k->usage == 0x2b)
        return ' ';   /* Enter, keypad Enter, Tab: a space */
    return cp >= 0x20 && cp < 0x7f ? (char)cp : 0;
}
