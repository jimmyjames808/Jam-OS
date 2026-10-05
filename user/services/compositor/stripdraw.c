/* The top bar drawn (desk.h): strip.c's layout, item by item, into a tile.
 * The strip's own picture (frost.c) first, a copy; then the islands, white
 * at LOOK_ISLAND_A over it; then each item: "Jam OS" in apricot ink (lit
 * while the search box is open), the screens' dots, "+", the chips (the
 * focused one tinted raspberry, a minimised one's title dimmed after an
 * apricot dot), the icons (lit blackcurrant while their popover is open)
 * and the clock. Every colour and size is look.h's. */
#include <fun.h>
#include "desk.h"

/* The strip's background where it meets t. */
static void background(const struct tile_buf *t, struct comp_box in)
{
    int32_t n = in.x2 - in.x1;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *dst = tile_row(t, y) + (in.x1 - t->b.x1);
        const uint32_t *src = frost_strip_row(y);
        if (src) {
            memcpy(dst, src + in.x1, (size_t)n * 4);
            continue;
        }
        for (int32_t i = 0; i < n; i++)
            dst[i] = LOOK_STRIP_TINT;
    }
}

/* A screen's dot: a disc, the current one's pill, a full-screen one's square. */
static void dot(const struct tile_buf *t, const struct strip_item *it)
{
    int32_t y = LOOK_ISLAND_TOP + (LOOK_ISLAND_H - LOOK_DOT_D) / 2;
    struct comp_box b = { it->box.x1, y, it->box.x2, y + LOOK_DOT_D };
    int32_t r = it->on ? LOOK_DOT_CUR_R : it->full ? LOOK_DOT_FULL_R : LOOK_DOT_D / 2 + 1;
    ui_round(t, b, r, it->on ? LOOK_JAM_APRICOT : LOOK_DOT, 255);
}

/* The chip's text box: inside its padding, after a minimised one's dot. */
static void chip(const struct tile_buf *t, const struct strip_item *it)
{
    struct comp_box b = it->box;
    if (it->on)
        ui_round(t, b, LOOK_BTN_R, LOOK_JAM_RASPBERRY, LOOK_CHIP_FOCUS_A);
    int32_t x = b.x1 + LOOK_CHIP_PAD;
    if (it->minimised) {
        int32_t y = (b.y1 + b.y2 - LOOK_CHIP_MIN_DOT) / 2;
        ui_round(t, (struct comp_box){ x, y, x + LOOK_CHIP_MIN_DOT, y + LOOK_CHIP_MIN_DOT },
                 LOOK_CHIP_MIN_DOT / 2, LOOK_JAM_APRICOT, 255);
        x += LOOK_CHIP_MIN_DOT + LOOK_DOT_GAP;
    }
    uint32_t ink = it->on ? LOOK_CHIP_FOCUS_INK : it->minimised ? LOOK_DOT : LOOK_INK;
    ui_text(t, (struct comp_box){ x, b.y1, b.x2 - LOOK_CHIP_PAD, b.y2 }, desk_font.r12, ink,
            false, it->text);
}

/* An icon button: lit while its popover is open; the icon centred. */
static void icon(const struct tile_buf *t, const struct strip_item *it, enum ui_icon ic)
{
    struct comp_box b = it->box;
    if (it->on)
        ui_round(t, b, LOOK_ICON_R, LOOK_JAM_BLACKCURRANT, LOOK_ICON_ON_A);
    int32_t d = 15, x = (b.x1 + b.x2 - d) / 2, y = (b.y1 + b.y2 - d) / 2;
    ui_icon(t, (struct comp_box){ x, y, x + d, y + d }, ic, it->on ? 0xffffff : LOOK_ICON_INK);
}

static void item(const struct tile_buf *t, const struct strip_item *it)
{
    switch (it->part) {
    case STRIP_JAM:
        if (it->on)
            ui_round(t, it->box, LOOK_BTN_R, LOOK_JAM_OPEN, LOOK_JAM_OPEN_A);
        ui_text(t, it->box, desk_font.m12, LOOK_JAM_INK, true, "Jam OS");
        break;
    case STRIP_DOT:
        dot(t, it);
        break;
    case STRIP_PLUS: {
        int32_t x = (it->box.x1 + it->box.x2 - LOOK_PLUS) / 2;
        int32_t y = (it->box.y1 + it->box.y2 - LOOK_PLUS) / 2;
        ui_icon(t, (struct comp_box){ x, y, x + LOOK_PLUS, y + LOOK_PLUS }, UI_PLUS, LOOK_DOT);
        break;
    }
    case STRIP_CHIP:
        chip(t, it);
        break;
    case STRIP_NOCHIP:
        ui_text(t, it->box, desk_font.r12, LOOK_DIM, true, "No windows");
        break;
    case STRIP_MODE:
        icon(t, it, scene.layout == COMP_TILING ? UI_TILING : UI_FLOATING);
        break;
    case STRIP_NET:
        icon(t, it, UI_NETWORK);
        break;
    case STRIP_VOL:
        icon(t, it, UI_VOLUME);
        break;
    case STRIP_CLOCK:
        if (it->on)
            ui_round(t, it->box, LOOK_BTN_R, LOOK_JAM_BLACKCURRANT, LOOK_ICON_ON_A);
        ui_text(t, it->box, desk_font.r12, it->on ? 0xffffff : LOOK_INK, true, it->text);
        break;
    default:
        break;
    }
}

void strip_draw(const struct tile_buf *t)
{
    struct comp_box in = box_intersect(strip_box(), t->b);
    if (box_empty(in))
        return;
    background(t, in);
    for (int i = 0; i < 3; i++)
        ui_round(t, strip.islands[i], LOOK_ISLAND_R, 0xffffff, LOOK_ISLAND_A);
    for (unsigned i = 0; i < strip.n; i++)
        if (!box_empty(box_intersect(strip.items[i].box, t->b)))
            item(t, &strip.items[i]);
}
