/* The search box and Alt+Tab's list drawn (desk.h): menus.c's boxes, on
 * glass (ui.c, frost.c's FROST_MENU backdrop), the prototype's look:
 *   search  a magnifier and the line typed (a hint while it is empty, an
 *           apricot caret after the text) | the rows: a letter tile, the
 *           name, what it is on the right and "Enter" on the selected row,
 *           which is lit blackcurrant; "Run “...” in a terminal" last, after
 *           a divider;
 *   Alt+Tab a row per window, tile and title; a divider and a small label
 *           ("Screen 2", "Full screen") before each other screen's group, a
 *           divider before the minimised ones, which are dimmed and say so.
 * Dividers are inset to the card's padding. */
#include <fun.h>
#include "desk.h"

#define RUN_TILE 0x3a414bu   /* the "Run" row's tile */

/* ---- the search box ------------------------------------------------------------------------ */

static void search_line(const struct tile_buf *t, struct comp_box b)
{
    int32_t x = b.x1 + LOOK_SEARCH_PAD + 8, top = b.y1 + LOOK_SEARCH_PAD;
    int32_t y = top + (LOOK_SEARCH_IN_H - 17) / 2;
    ui_icon(t, (struct comp_box){ x, y, x + 17, y + 17 }, UI_SEARCH, LOOK_MUTED);
    x += 17 + 10;
    struct comp_box line = { x, top, b.x2 - LOOK_SEARCH_PAD - 8, top + LOOK_SEARCH_IN_H };
    if (!search.text[0]) {
        ui_text(t, line, desk_font.r16, LOOK_DIM, false, "Search apps, or type a command");
        x -= 1;
    } else {
        ui_text(t, line, desk_font.r16, LOOK_INK, false, search.text);
        x += desk_text_w(desk_font.r16, search.text) + 1;
    }
    if (x < line.x2)   /* the caret */
        ui_round(t, (struct comp_box){ x, top + 8, x + 2, top + LOOK_SEARCH_IN_H - 8 }, 0,
                 LOOK_JAM_APRICOT, 255);
    int32_t dy = top + LOOK_SEARCH_IN_H + 4;
    ui_divider(t, b.x1 + LOOK_SEARCH_PAD, b.x2 - LOOK_SEARCH_PAD, dy);
}

/* A row's letter tile: LOOK_TILE_D square, LOOK_ROW_PAD in from its left. */
static struct comp_box row_tile(struct comp_box r)
{
    int32_t y = (r.y1 + r.y2 - LOOK_TILE_D) / 2;
    return (struct comp_box){ r.x1 + LOOK_ROW_PAD, y, r.x1 + LOOK_ROW_PAD + LOOK_TILE_D,
                              y + LOOK_TILE_D };
}

/* The rest of a row after its tile: text from there. */
static struct comp_box row_text(struct comp_box r)
{
    return (struct comp_box){ r.x1 + LOOK_ROW_PAD + LOOK_TILE_D + 10, r.y1, r.x2 - LOOK_ROW_PAD,
                              r.y2 };
}

static void search_row(const struct tile_buf *t, unsigned i)
{
    struct comp_box r = search_row_box(i), text = row_text(r);
    if (box_empty(box_intersect(r, t->b)) && search.row[i] != DESK_APPS)
        return;
    bool sel = i == search.sel;
    if (sel)
        ui_round(t, r, LOOK_ROW_R, LOOK_JAM_BLACKCURRANT, LOOK_ROW_SEL_A);
    if (search.row[i] == DESK_APPS) {
        if (i > 0)
            ui_divider(t, r.x1 + LOOK_ROW_PAD, r.x2 - LOOK_ROW_PAD, r.y1 - 5);
        struct comp_box tb = row_tile(r);
        ui_round(t, tb, LOOK_TILE_R, RUN_TILE, 255);
        ui_icon(t, (struct comp_box){ tb.x1 + 5, tb.y1 + 5, tb.x2 - 5, tb.y2 - 5 }, UI_TERMINAL,
                0xffffff);
        char line[SEARCH_TEXT_MAX + 32];
        snprintf(line, sizeof(line), "Run “%s” in a terminal", search.text);
        ui_text(t, text, desk_font.r13, LOOK_INK, false, line);
        return;
    }
    const struct desk_app *a = &desk_apps[search.row[i]];
    ui_tile(t, row_tile(r), LOOK_TILE_R, a->colour, a->letter, desk_font.m12);
    int32_t right = text.x2;
    if (sel) {
        int32_t w = desk_text_w(desk_font.r11, "Enter");
        ui_text(t, (struct comp_box){ right - w, r.y1, right, r.y2 }, desk_font.r11, LOOK_MUTED,
                false, "Enter");
        right -= w + 10;
    }
    int32_t dw = desk_text_w(desk_font.r11, a->desc);
    ui_text(t, (struct comp_box){ right - dw, r.y1, right, r.y2 }, desk_font.r11, LOOK_MUTED,
            false, a->desc);
    ui_text(t, (struct comp_box){ text.x1, r.y1, right - dw - 10, r.y2 }, desk_font.r13, LOOK_INK,
            false, a->name);
}

void search_draw(const struct tile_buf *t)
{
    struct comp_box b = search_box();
    if (box_empty(b))
        return;
    ui_glass(t, b, FROST_MENU, 255);
    if (box_empty(box_intersect(b, t->b)))
        return;
    search_line(t, b);
    for (unsigned i = 0; i < search.nrows; i++)
        search_row(t, i);
}

/* ---- Alt+Tab ------------------------------------------------------------------------------- */

/* A window's tile: the app's letter and colour when it is one of the
 * table's, else its title's first letter in a jam colour picked by it. */
static void window_tile(const struct wm_window *ww, char *letter, uint32_t *colour)
{
    static const uint32_t jam[3] = { LOOK_JAM_RASPBERRY, LOOK_JAM_APRICOT, LOOK_JAM_BLACKCURRANT };
    for (unsigned i = 0; i < DESK_APPS; i++)
        if (!strncmp(ww->title, desk_apps[i].name, strlen(desk_apps[i].name))) {
            *letter = desk_apps[i].letter;
            *colour = desk_apps[i].colour;
            return;
        }
    char c = ww->title[0];
    *letter = c >= 'a' && c <= 'z' ? (char)(c - 32) : c >= ' ' && c < 0x7f ? c : '?';
    *colour = jam[(unsigned char)*letter % 3];
}

/* Above row i, the first of its group: a divider, and a label for a screen. */
static void group_head(const struct tile_buf *t, struct comp_box b, unsigned i, struct comp_box r)
{
    uint32_t g = alttab.rows[i].group;
    bool label = g != DESK_SCREENS_MAX + 1;
    int32_t y = r.y1 - (label ? LOOK_GROUP_H : 0) - 6;
    if (y < b.y1 + LOOK_ALTTAB_PAD)
        return;   /* scrolled out */
    ui_divider(t, r.x1 + LOOK_ROW_PAD, r.x2 - LOOK_ROW_PAD, y);
    if (!label)
        return;
    const struct desk_screen *s = screens_nth(g - 1);
    char text[24];
    if (s && s->kind == SCREEN_FULL)
        snprintf(text, sizeof(text), "Full screen");
    else
        snprintf(text, sizeof(text), "Screen %u", g);
    ui_text(t, (struct comp_box){ r.x1 + LOOK_ROW_PAD, r.y1 - LOOK_GROUP_H + 4, r.x2, r.y1 - 2 },
            desk_font.r11, LOOK_MUTED, false, text);
}

static void alttab_row(const struct tile_buf *t, struct comp_box b, unsigned i)
{
    struct comp_box r = alttab_row_box(i);
    if (box_empty(r))
        return;
    const struct alttab_row *row = &alttab.rows[i];
    if (row->first)
        group_head(t, b, i, r);
    if (box_empty(box_intersect(r, t->b)))
        return;
    if (i == alttab.sel)
        ui_round(t, r, LOOK_ROW_R, LOOK_JAM_BLACKCURRANT, LOOK_ROW_SEL_A);
    char letter;
    uint32_t colour;
    window_tile(row->ww, &letter, &colour);
    bool min = row->ww->minimised;
    ui_tile(t, row_tile(r), LOOK_TILE_R, min ? paint_mix(colour, LOOK_GLASS_TINT, 102) : colour,
            letter, desk_font.m12);
    struct comp_box text = row_text(r);
    if (min) {
        int32_t w = desk_text_w(desk_font.r11, "Minimised");
        ui_text(t, (struct comp_box){ text.x2 - w, r.y1, text.x2, r.y2 }, desk_font.r11,
                LOOK_MUTED, false, "Minimised");
        text.x2 -= w + 10;
    }
    ui_text(t, text, desk_font.r13, min ? LOOK_MUTED : LOOK_INK, false, row->ww->title);
}

void alttab_draw(const struct tile_buf *t)
{
    if (!alttab.shown)
        return;
    struct comp_box b = alttab_box();
    ui_glass(t, b, FROST_MENU, 255);
    if (box_empty(box_intersect(b, t->b)))
        return;
    for (unsigned i = 0; i < alttab.n; i++)
        alttab_row(t, b, i);
}
