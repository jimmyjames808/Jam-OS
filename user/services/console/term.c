/* console: program output as a small terminal (console.h). */
#include "console.h"

/* ---- program output: a small terminal on the current line ------------------ */

struct client *out_writer;

enum { ES_NONE, ES_ESC, ES_CSI };
static int esc_state;
static char esc_buf[16];
static uint32_t esc_len;

/* ESC [ 1 m (out_style S_BOLD): the normal colours (30-37) come out bright,
 * and the smooth font draws the text in bold. */
void sgr(uint32_t p)
{
    uint8_t fg = out_attr & 15, bg = out_attr >> 4;
    if (p == 0) {
        fg = C_WHITE;
        bg = C_BLACK;
        out_style = 0;
    } else if (p == 1) {
        fg |= 8;
        out_style |= S_BOLD;
    } else if (p == 22) {
        out_style &= (uint8_t)~S_BOLD;
    } else if (p >= 30 && p <= 37) {
        fg = (uint8_t)(p - 30) | (out_style & S_BOLD ? 8 : 0);
    } else if (p == 39) {
        fg = C_WHITE;
    } else if (p >= 40 && p <= 47) {
        bg = (uint8_t)(p - 40);
    } else if (p == 49) {
        bg = C_BLACK;
    } else if (p >= 90 && p <= 97) {
        fg = (uint8_t)(p - 90 + 8);
    } else if (p >= 100 && p <= 107) {   /* bright backgrounds */
        bg = (uint8_t)(p - 100 + 8);
    }
    out_attr = ATTR(fg, bg);
}

/* ESC [ ? <modes> h / l: DEC private modes: 1049 the alternate screen, 25
 * its cursor, 2026 a frame (synchronized output), 2004 bracketed paste
 * (the writing client's own: paste.c). */
static void dec_modes(char final, const uint32_t *params, uint32_t np)
{
    if (final != 'h' && final != 'l')
        return;
    for (uint32_t i = 0; i < np; i++) {
        if (params[i] == 1049) {
            if (final == 'h')
                alt_enter();
            else
                alt_leave();
        } else if (params[i] == 25) {
            alt_cursor = final == 'h';
        } else if (params[i] == 2026 && alt_on) {
            alt_sync = final == 'h';
            alt_sync_since = now();
        } else if (params[i] == 2004 && out_writer) {
            out_writer->bracketed = final == 'h';   /* its pastes bracketed (paste.c) */
        }
    }
}

/* A CSI sequence on the alternate screen (but ESC [ m). n: the first
 * parameter, at least 1. */
static void alt_csi(char final, const uint32_t *params, uint32_t np, uint32_t n)
{
    switch (final) {
    case 'H':
    case 'f':
        alt_y = params[0] ? params[0] - 1 : 0;
        alt_x = np > 1 && params[1] ? params[1] - 1 : 0;
        if (alt_y >= rows)
            alt_y = rows - 1;
        if (alt_x >= cols)
            alt_x = cols - 1;
        break;
    case 'J':
        if (params[0] == 2)
            blank(alt, rows * cols, out_attr);
        break;
    case 'K':
        blank(alt + alt_y * cols + alt_x, cols - alt_x, out_attr);
        break;
    case 'A': alt_y = alt_y > n ? alt_y - n : 0; break;
    case 'B': alt_y = alt_y + n < rows ? alt_y + n : rows - 1; break;
    case 'C': alt_x = alt_x + n < cols ? alt_x + n : cols - 1; break;
    case 'D': alt_x = alt_x > n ? alt_x - n : 0; break;
    }
}

static void csi(char final)
{
    uint32_t params[4] = { 0 }, np = 0;
    bool any = false;
    for (uint32_t i = 0; i < esc_len && np < 4; i++) {
        char c = esc_buf[i];
        if (c >= '0' && c <= '9') {
            params[np] = params[np] * 10 + (uint32_t)(c - '0');
            any = true;
        } else if (c == ';') {
            np++;
        }
    }
    if (any || esc_len)
        np++;
    if (np > 4)
        np = 4;   /* ESC [ ; ; ; ; m: the 5th and later are dropped (not read past params) */
    uint32_t n = params[0] ? params[0] : 1;
    if (esc_len && esc_buf[0] == '?') {
        dec_modes(final, params, np);
        return;
    }
    if (alt_on && final != 'm') {
        alt_csi(final, params, np, n);
        return;
    }
    switch (final) {
    case 'm':
        if (np == 0)
            sgr(0);
        for (uint32_t i = 0; i < np; i++)
            sgr(params[i]);
        break;
    case 'K':   /* erase to the end of the line */
        blank(cur + cur_x, cols - cur_x, out_attr);
        break;
    case 'C':
        cur_x = cur_x + n < cols ? cur_x + n : cols - 1;
        break;
    case 'D':
        cur_x = cur_x > n ? cur_x - n : 0;
        break;
    case 'J':
        if (params[0] == 2)
            clear_screen();
        break;
    case 'H':
        cur_x = 0;
        break;
    }
}

/* UTF-8 (<utf8.h>): a character the console's fonts have (ASCII, the Latin
 * letters, the box drawing and block elements) is drawn as itself
 * (cell_glyph; the 8x16 bitmap draws '?' for the box drawing but the
 * block elements full-screen programs draw with); any other character, a control character and each
 * bad piece of a malformed sequence is one '?'. A sequence arrives a byte
 * at a time: its bytes wait in useq until it is complete or turns out bad. */
static uint8_t useq[4];
static unsigned ulen;   /* bytes of a sequence held in useq */

static void put_char(uint16_t ch);

/* A sequence that was cut short (by an ASCII byte or an escape): one '?'. */
static void utf8_flush(void)
{
    if (ulen)
        put_char('?');
    ulen = 0;
}

/* A byte of a UTF-8 sequence (ch >= 0x80). */
static void utf8_byte(uint8_t ch)
{
    if (ulen) {
        useq[ulen++] = ch;
        uint32_t cp = 0;
        int k = utf8_seq(useq, ulen, &cp);
        if (k > 0) {
            ulen = 0;
            put_char(utf8_is_control(cp) ? '?' : cell_glyph(cp));
            return;
        }
        if ((unsigned)-k == ulen)
            return;   /* a valid start so far: wait for the rest */
        ulen = 0;     /* the bytes before ch were a bad piece; ch starts afresh */
        put_char('?');
    }
    if (utf8_lead_len(ch) < 2) {
        put_char('?');   /* a stray continuation byte, or one that never starts a sequence */
        return;
    }
    useq[0] = ch;
    ulen = 1;
}

/* A byte after ESC or inside ESC [ ...: true if it was one. */
static bool escape_byte(uint8_t ch)
{
    if (esc_state == ES_ESC) {
        esc_state = ch == '[' ? ES_CSI : ES_NONE;
        esc_len = 0;
        return true;
    }
    if (esc_state == ES_CSI) {
        if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?') {
            if (esc_len < sizeof(esc_buf))
                esc_buf[esc_len++] = (char)ch;
        } else {
            csi((char)ch);
            esc_state = ES_NONE;
        }
        return true;
    }
    return false;
}

/* A byte on the alternate screen: no tabs, and \b stops at the left. */
static void alt_byte(uint8_t ch)
{
    if (ch == 0x1b)
        esc_state = ES_ESC;
    else if (ch == '\n')
        alt_newline();
    else if (ch == '\r')
        alt_x = 0;
    else if (ch == '\b' && alt_x)
        alt_x--;
    else if (ch >= 0x20 && ch <= 0x7e)
        put_char(ch);
}

void out_char(uint8_t ch)
{
    if (ch >= 0x80 && esc_state == ES_NONE) {
        utf8_byte(ch);
        return;
    }
    utf8_flush();
    if (escape_byte(ch))
        return;
    if (alt_on) {
        alt_byte(ch);
        return;
    }
    switch (ch) {
    case 0x1b:
        esc_state = ES_ESC;
        return;
    case '\n':
        new_line();
        return;
    case '\r':
        cur_x = 0;
        return;
    case '\b':
        if (cur_x)
            cur_x--;
        return;
    case '\t':
        do
            out_char(' ');
        while (cur_x % 8);
        return;
    }
    if (ch < 0x20)
        return;
    put_char(ch);
}

/* A printable character or a block glyph at the cursor. */
static void put_char(uint16_t ch)
{
    if (alt_on) {
        if (alt_x >= cols)
            alt_newline();
        alt[alt_y * cols + alt_x++] = (struct cell){ ch, out_attr, out_style };
        return;
    }
    if (cur_x >= cols)
        new_line();
    cur[cur_x++] = (struct cell){ ch, out_attr, out_style };
}
