/* console: the mouse's selection in a terminal window, its geometry and
 * its text (console.h, "select.c").
 *
 * A selection is two points of the text, the anchor (where the press
 * was) and the head (where the pointer is now), each a line, numbered as
 * the scrollback numbers them (the current line is `committed`), and a
 * column. A selection of whole words or lines (a double or triple click)
 * grows each end to its word's or line's edge. It is a range of the text,
 * not of the screen, so it stays on its lines while they scroll, reaches
 * back into the scrollback, and a resize keeps it (a column past the new
 * width is cut).
 *
 * Words: a run of cells of one kind, the kind of the cell clicked: word
 * characters (letters, digits and the characters of paths and addresses,
 * WORD_PUNCT), blanks, or other punctuation.
 *
 * The text copied: each line's cells in the range, as UTF-8, its trailing
 * blanks dropped, the lines joined with '\n' (a line the console wrapped
 * is two lines here: the console keeps no mark of a wrap).
 *
 * No state of the console's and no system calls: utest checks them
 * (user/tests/utest/consel.c). */
#include "console.h"

/* Punctuation that is part of a word: paths, file names, addresses, options. */
#define WORD_PUNCT "-_./~:@%+=#?&"
#define CLICK_MS   500   /* a second click within this, on the same cell, counts on */

enum { K_BLANK, K_WORD, K_OTHER };

static int kind_of(struct cell c)
{
    uint32_t cp = cell_cp(c);
    if (cp == ' ' || cp == 0)
        return K_BLANK;
    if ((cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') ||
        (cp >= 0xc0 && cp < 0x2500 && cp != 0xd7 && cp != 0xf7) ||
        (cp < 0x80 && strchr(WORD_PUNCT, (int)cp)))
        return K_WORD;
    return K_OTHER;
}

void sel_cell_at(const struct cell_look *l, uint32_t cols, uint32_t rows, int32_t x, int32_t y,
                 uint32_t *col, uint32_t *row)
{
    int32_t cx = x < WIN_PAD ? 0 : (x - WIN_PAD) / l->w;
    int32_t cy = y < WIN_PAD ? 0 : (y - WIN_PAD) / l->h;
    *col = (uint32_t)cx < cols ? (uint32_t)cx : cols - 1;
    *row = (uint32_t)cy < rows ? (uint32_t)cy : rows - 1;
}

/* p before or at q, in reading order. */
static bool pt_le(struct sel_pt p, struct sel_pt q)
{
    return p.line < q.line || (p.line == q.line && p.col <= q.col);
}

/* p moved to its word's first (start) or last cell. */
static struct sel_pt word_edge(const struct sel_text *t, struct sel_pt p, bool start)
{
    const struct cell *l = t->line(t->ctx, p.line);
    if (!l || p.col >= t->cols)
        return p;
    int kind = kind_of(l[p.col]);
    if (start) {
        while (p.col > 0 && kind_of(l[p.col - 1]) == kind)
            p.col--;
    } else {
        while (p.col + 1 < t->cols && kind_of(l[p.col + 1]) == kind)
            p.col++;
    }
    return p;
}

bool sel_range(const struct selection *s, const struct sel_text *t, struct sel_pt *from,
               struct sel_pt *to)
{
    if (!s->on || !t->cols)
        return false;
    bool fwd = pt_le(s->anchor, s->head);
    struct sel_pt a = fwd ? s->anchor : s->head, b = fwd ? s->head : s->anchor;
    if (s->unit == SEL_WORD) {
        a = word_edge(t, a, true);
        b = word_edge(t, b, false);
    } else if (s->unit == SEL_LINE) {
        a.col = 0;
        b.col = t->cols - 1;
    }
    *from = a;
    *to = b;
    return true;
}

bool sel_has(const struct sel_pt *from, const struct sel_pt *to, int64_t line, uint32_t col)
{
    struct sel_pt p = { line, col };
    return pt_le(*from, p) && pt_le(p, *to);
}

/* cp as UTF-8 at out[n..cap): the new length, or n if it doesn't fit. */
static size_t put_utf8(char *out, size_t n, size_t cap, uint32_t cp)
{
    char u[3];
    size_t k;
    if (cp < 0x80) {
        u[0] = (char)cp, k = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3f)), k = 2;
    } else {
        u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f)), k = 3;
    }
    if (cap - n < k)
        return n;
    memcpy(out + n, u, k);
    return n + k;
}

/* Line l's cells [c0, c1] as UTF-8 at out[n..cap), blanks at the end dropped. */
static size_t put_line(const struct cell *l, uint32_t c0, uint32_t c1, char *out, size_t n,
                       size_t cap)
{
    while (c1 > c0 && kind_of(l[c1]) == K_BLANK)
        c1--;
    if (kind_of(l[c1]) == K_BLANK)
        return n;   /* all blank */
    for (uint32_t c = c0; c <= c1 && n < cap; c++)
        n = put_utf8(out, n, cap, cell_cp(l[c]));
    return n;
}

size_t sel_copy(const struct selection *s, const struct sel_text *t, char *out, size_t cap)
{
    struct sel_pt a, b;
    if (!sel_range(s, t, &a, &b))
        return 0;
    size_t n = 0;
    /* bounded: a range spans at most the scrollback and the current line */
    for (int64_t i = a.line; i <= b.line && n < cap; i++) {
        const struct cell *l = t->line(t->ctx, i);
        if (i > a.line)
            n = put_utf8(out, n, cap, '\n');
        if (!l)
            continue;   /* scrolled out of what the scrollback keeps */
        uint32_t c0 = i == a.line ? a.col : 0, c1 = i == b.line ? b.col : t->cols - 1;
        c0 = c0 < t->cols ? c0 : t->cols - 1;
        c1 = c1 < t->cols ? c1 : t->cols - 1;
        n = put_line(l, c0, c1 < c0 ? c0 : c1, out, n, cap);
    }
    return n;
}

/* ---- presses and drags ------------------------------------------------------------- */

unsigned sel_click(struct click_track *k, uint32_t time_ms, struct sel_pt at)
{
    bool again = k->count && time_ms - k->time_ms <= CLICK_MS && at.line == k->at.line &&
                 at.col == k->at.col;
    k->count = again ? k->count % 3 + 1 : 1;
    k->time_ms = time_ms;
    k->at = at;
    return k->count;
}

void sel_press(struct selection *s, struct sel_pt at, unsigned clicks)
{
    s->anchor = s->head = at;
    s->unit = clicks >= 3 ? SEL_LINE : clicks == 2 ? SEL_WORD : SEL_CHAR;
    /* One click selects nothing until a drag reaches another cell (and
     * clears what was selected); a double or triple click selects at once. */
    s->on = clicks >= 2;
}

bool sel_drag(struct selection *s, struct sel_pt at)
{
    if (at.line == s->head.line && at.col == s->head.col)
        return false;
    s->head = at;
    s->on = true;
    return true;
}
