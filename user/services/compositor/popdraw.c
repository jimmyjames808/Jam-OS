/* The popovers and the notifications drawn (desk.h): popover.c's and
 * notify.c's boxes on glass (frost.c's FROST_POP and FROST_NOTES
 * backdrops), the prototype's look. A popover's sections are split by
 * dividers inset to its padding:
 *   volume   speaker, slider (raspberry up to the volume, a raspberry
 *            knob), the percentage | "Output" and its name | "Playing" and
 *            what is;
 *   network  a green dot and "Connected" (grey, "Not connected") | the
 *            address and the link (card, speed) | the rates down and up;
 *   clock    the time, large, and the full date | the month, Monday first,
 *            today an apricot square.
 * A notification: a letter tile, its title and body, its buttons (the
 * first apricot), the whole card faded by its alpha while it comes and
 * goes. */
#include <fun.h>
#include "desk.h"

#define LABEL_H 16

static const char *const day_name[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                         "Friday", "Saturday" };
static const char *const month_name[12] = { "January", "February", "March", "April", "May",
                                            "June", "July", "August", "September", "October",
                                            "November", "December" };

/* A line of the popover at y: x1 to x2 inside its padding. */
static struct comp_box pline(int32_t y, int32_t h)
{
    return (struct comp_box){ pop.box.x1 + LOOK_POP_PAD, y, pop.box.x2 - LOOK_POP_PAD, y + h };
}

/* A divider after y: its row; the next line's y returned. */
static int32_t divide(const struct tile_buf *t, int32_t y)
{
    ui_divider(t, pop.box.x1 + LOOK_POP_PAD, pop.box.x2 - LOOK_POP_PAD, y + LOOK_POP_DIV);
    return y + 2 * LOOK_POP_DIV + 1;
}

/* A label and a value under it; the next y. */
static int32_t labelled(const struct tile_buf *t, int32_t y, const char *label, const char *value)
{
    ui_text(t, pline(y, LABEL_H), desk_font.r11, LOOK_MUTED, false, label);
    ui_text(t, pline(y + LABEL_H, LOOK_POP_LINE), desk_font.r12, LOOK_INK, false, value);
    return y + LABEL_H + LOOK_POP_LINE;
}

/* A label on the left, its value on the right; the next y. */
static int32_t key_value(const struct tile_buf *t, int32_t y, const char *key, const char *value)
{
    struct comp_box l = pline(y, LOOK_POP_LINE);
    ui_text(t, l, desk_font.r11, LOOK_MUTED, false, key);
    int32_t w = desk_text_w(desk_font.r12, value);
    ui_text(t, (struct comp_box){ l.x2 - w, l.y1, l.x2, l.y2 }, desk_font.r12, LOOK_INK, false,
            value);
    return y + LOOK_POP_LINE;
}

static void volume(const struct tile_buf *t)
{
    int32_t y = pop.box.y1 + LOOK_POP_PAD;
    struct comp_box l = pline(y, LOOK_POP_LINE), s = pop_slider_box();
    ui_icon(t, (struct comp_box){ l.x1, y + 2, l.x1 + 16, y + 18 }, UI_VOLUME, LOOK_INK);
    int32_t mid = (s.y1 + s.y2) / 2, fill = s.x1 + (s.x2 - s.x1) * (int32_t)pop.volume / 100;
    ui_round(t, (struct comp_box){ s.x1, mid - 2, s.x2, mid + 2 }, 2, LOOK_SLIDER_OFF, 255);
    ui_round(t, (struct comp_box){ s.x1, mid - 2, fill, mid + 2 }, 2, LOOK_SLIDER_ON, 255);
    ui_round(t, (struct comp_box){ fill - 7, mid - 7, fill + 7, mid + 7 }, 7, LOOK_SLIDER_ON, 255);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", pop.volume);
    int32_t w = desk_text_w(desk_font.r12, pct);
    ui_text(t, (struct comp_box){ l.x2 - w, y, l.x2, y + LOOK_POP_LINE }, desk_font.r12, LOOK_INK,
            false, pct);
    y = divide(t, y + LOOK_POP_LINE);
    y = labelled(t, y, "Output", pop.output);
    y = divide(t, y);
    labelled(t, y, "Playing", pop.playing);
}

/* Bytes a second as "1.2 MB/s". */
static void rate(char *buf, size_t n, uint64_t bps)
{
    uint64_t tenths = bps / 100000;
    snprintf(buf, n, "%lu.%lu MB/s", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

static void network(const struct tile_buf *t)
{
    const struct desk_net *n = &pop.net;
    int32_t y = pop.box.y1 + LOOK_POP_PAD;
    struct comp_box l = pline(y, LOOK_POP_LINE);
    int32_t dy = (l.y1 + l.y2 - 8) / 2;
    ui_round(t, (struct comp_box){ l.x1, dy, l.x1 + 8, dy + 8 }, 4, n->up ? LOOK_LIVE : LOOK_DOT,
             255);
    ui_text(t, (struct comp_box){ l.x1 + 16, l.y1, l.x2, l.y2 }, desk_font.r12, LOOK_INK, false,
            n->up ? "Connected" : "Not connected");
    y = divide(t, y + LOOK_POP_LINE);
    char link[64], down[24], up[24];
    if (n->nic[0] && n->mbps)
        snprintf(link, sizeof(link), "%s, %u.%u Gb/s", n->nic, n->mbps / 1000,
                 n->mbps % 1000 / 100);
    else
        snprintf(link, sizeof(link), "%s", n->nic[0] ? n->nic : "—");
    y = key_value(t, y, "Address", n->address[0] ? n->address : "—");
    y = key_value(t, y, "Link", link);
    y = divide(t, y);
    rate(down, sizeof(down), n->rx_bps);
    rate(up, sizeof(up), n->tx_bps);
    y = key_value(t, y, "Down", down);
    key_value(t, y, "Up", up);
}

/* The month's calendar from y: the weekdays' letters, then the days. */
static void calendar(const struct tile_buf *t, int32_t y)
{
    static const char *const head[7] = { "M", "T", "W", "T", "F", "S", "S" };
    struct comp_box l = pline(y, LOOK_POP_LINE);
    int32_t cw = (l.x2 - l.x1) / 7;
    for (int c = 0; c < 7; c++)
        ui_text(t, (struct comp_box){ l.x1 + c * cw, y, l.x1 + (c + 1) * cw, y + LOOK_POP_LINE },
                desk_font.r11, LOOK_MUTED, true, head[c]);
    uint8_t cells[42];
    unsigned n = pop_calendar(&pop.now, cells);
    for (unsigned i = 0; i < n; i++) {
        if (!cells[i])
            continue;
        int32_t x = l.x1 + (int32_t)(i % 7) * cw, cy = y + LOOK_POP_LINE * (1 + (int32_t)(i / 7));
        struct comp_box cell = { x + 1, cy + 1, x + cw - 1, cy + LOOK_POP_LINE - 1 };
        bool today = cells[i] == pop.now.day;
        if (today)
            ui_round(t, cell, 5, LOOK_TODAY, 255);
        char d[4];
        snprintf(d, sizeof(d), "%u", cells[i]);
        ui_text(t, cell, today ? desk_font.m11 : desk_font.r11, today ? LOOK_TODAY_INK : LOOK_INK,
                true, d);
    }
}

static void clock(const struct tile_buf *t)
{
    const struct civil *c = &pop.now;
    int32_t y = pop.box.y1 + LOOK_POP_PAD;
    char text[64];
    snprintf(text, sizeof(text), "%02u:%02u", c->hour, c->minute);
    ui_text(t, pline(y, 24), desk_font.m17, LOOK_INK, false, text);
    snprintf(text, sizeof(text), "%s %u %s %ld", day_name[c->wday % 7], c->day,
             month_name[(c->month + 11) % 12], (long)c->year);
    ui_text(t, pline(y + 24, LABEL_H), desk_font.r11, LOOK_MUTED, false, text);
    calendar(t, divide(t, y + 24 + LABEL_H));
}

void pop_draw(const struct tile_buf *t)
{
    if (pop.kind == POP_NONE)
        return;
    ui_glass(t, pop.box, FROST_POP, 255);
    if (box_empty(box_intersect(pop.box, t->b)))
        return;
    if (pop.kind == POP_VOLUME)
        volume(t);
    else if (pop.kind == POP_NETWORK)
        network(t);
    else
        clock(t);
}

/* ---- notifications ------------------------------------------------------------------------ */

/* ink faded towards the glass by alpha a (0..255). */
static uint32_t fade(uint32_t ink, uint32_t a)
{
    return paint_mix(LOOK_GLASS_TINT, ink, a);
}

static void note_card(const struct tile_buf *t, const struct notify_card *c)
{
    struct comp_box b = c->box;
    ui_glass(t, b, FROST_NOTES, c->alpha);
    if (box_empty(box_intersect(b, t->b)) || !c->alpha)
        return;
    int32_t x = b.x1 + LOOK_NOTE_PAD_X, y = b.y1 + LOOK_NOTE_PAD_Y;
    struct comp_box tile = { x, y, x + LOOK_NOTE_TILE, y + LOOK_NOTE_TILE };
    ui_tile(t, tile, LOOK_NOTE_TILE_R, fade(c->colour, c->alpha), c->letter, desk_font.m13);
    x = tile.x2 + 10;
    ui_text(t, (struct comp_box){ x, y, b.x2 - LOOK_NOTE_PAD_X, y + 18 }, desk_font.m13,
            fade(LOOK_INK, c->alpha), false, c->title);
    if (c->body[0])
        ui_text(t, (struct comp_box){ x, y + 18, b.x2 - LOOK_NOTE_PAD_X, y + 35 }, desk_font.r12,
                fade(LOOK_MUTED, c->alpha), false, c->body);
    for (unsigned i = 0; i < c->nbuttons; i++) {
        struct comp_box bb = notify_button_box(c, i);
        bool pri = i == 0;
        ui_round(t, bb, 6, pri ? LOOK_NOTE_PRI : 0xffffff,
                 (pri ? LOOK_NOTE_PRI_A : LOOK_NOTE_BTN_A) * c->alpha / 255);
        ui_text(t, bb, desk_font.r12, fade(pri ? LOOK_NOTE_PRI_INK : LOOK_INK, c->alpha), true,
                c->buttons[i]);
    }
}

void notify_draw(const struct tile_buf *t)
{
    for (unsigned i = notes.n; i-- > 0;)
        note_card(t, &notes.cards[i]);
}
