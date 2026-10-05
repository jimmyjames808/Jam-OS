/* Frosted glass (desk.h, look.h): the blurs behind the strip and the
 * cards.
 *
 * The strip: a blur of the wallpaper under it, a little more saturated,
 * under a dark tint, with a faint line along its bottom, made once for the
 * output's size (frost_init): windows never go under the strip, so what is
 * behind it is always the wallpaper, and painting it is a copy.
 *
 * The cards (the search box or Alt+Tab, a popover, the notifications: a
 * slot each): what is behind a card, windows included, changes, so its
 * backdrop is made while it is open, of its box only: before a paint,
 * frost_prepare finds each card whose backdrop is stale (just opened,
 * moved or resized, or the scene's `under` damage meets it: something
 * behind it changed; the cursor's and the cards' own damage don't count),
 * and damages its box; after the paint is planned, frost_build composes
 * what is under the card into the slot's buffer (paint_under, on the
 * painting workers, in bands of TILE_H rows) and blurs it there. A card
 * still open over nothing that changes costs nothing.
 *
 * The blur is three box blurs (each LOOK_GLASS_BLUR or LOOK_STRIP_BLUR
 * pixels wide, a close match for a Gaussian, as shape.c's shadows), across
 * each row and then down each column, with the edge pixels repeated past
 * the box: only the box is ever read. A row or column at a time, through a
 * line buffer of each worker's own.
 *
 * Memory: the strip's picture (the output's width x LOOK_STRIP_H) and
 * each slot's buffer (big_alloc's, its own VMO), grown when a card is
 * bigger than any before it in that slot and never given back: at most a
 * search box, a popover and a column of notifications. */
#include <fun.h>
#include "desk.h"

static struct {
    uint32_t *strip;               /* w x LOOK_STRIP_H, or NULL: a flat tint */
    int32_t w;
    struct slot {
        uint32_t *px;              /* the backdrop: have's width a row */
        uint64_t cap;              /* pixels it has room for */
        struct comp_box have;      /* what it holds (empty: nothing) */
        bool build;                /* make it again at this paint */
    } slot[FROST_SLOTS];
    uint32_t *line[FUN_MAX_THREADS];   /* each worker's two lines */
    uint32_t line_px;              /* each line's length */
    struct slot *doing;            /* the slot frost_build is making */
    int32_t blur;                  /* the box blur's width for it */
} fr;

/* ---- the blur ------------------------------------------------------------------------------ */

/* One box blur of width k (odd) over n pixels of src into dst, the ends
 * repeated: dst[i] is the mean of src[i - k/2 .. i + k/2], rounded. */
static void box_pass(const uint32_t *src, uint32_t *dst, int32_t n, int32_t k)
{
    int32_t h = k / 2;
    uint32_t s[3] = { 0, 0, 0 };
    for (int32_t j = -h - 1; j < h; j++) {   /* the window before pixel 0 */
        uint32_t p = src[j < 0 ? 0 : j >= n ? n - 1 : j];
        for (int c = 0; c < 3; c++)
            s[c] += p >> (8 * c) & 0xff;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t in = src[i + h < n ? i + h : n - 1];
        uint32_t out = src[i - h - 1 < 0 ? 0 : i - h - 1];
        uint32_t px = 0;
        for (int c = 0; c < 3; c++) {
            s[c] += (in >> (8 * c) & 0xff) - (out >> (8 * c) & 0xff);
            px |= ((s[c] + (uint32_t)h) / (uint32_t)k) << (8 * c);
        }
        dst[i] = px;
    }
}

/* n pixels from p, step apart, blurred three times in place (two lines of
 * room in tmp). */
static void blur_line(uint32_t *p, int32_t n, int32_t step, int32_t k, uint32_t *tmp)
{
    uint32_t *a = tmp, *b = tmp + n;
    for (int32_t i = 0; i < n; i++)
        a[i] = p[(int64_t)i * step];
    box_pass(a, b, n, k);
    box_pass(b, a, n, k);
    box_pass(a, b, n, k);
    for (int32_t i = 0; i < n; i++)
        p[(int64_t)i * step] = b[i];
}

/* ---- the strip ----------------------------------------------------------------------------- */

/* p a little more saturated (LOOK_STRIP_SAT), under the strip's tint. */
static uint32_t strip_px(uint32_t p)
{
    int32_t r = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
    int32_t l = (r * 77 + g * 150 + b * 29) >> 8, c[3] = { r, g, b };
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        int32_t v = l + (c[i] - l) * LOOK_STRIP_SAT / 256;
        out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - 8 * i);
    }
    return paint_mix(out, LOOK_STRIP_TINT, LOOK_STRIP_TINT_A);
}

/* The strip's picture: the wallpaper's top rows (and the blur's reach
 * below them) blurred, saturated, tinted, its bottom line lit. */
static status_t make_strip(int32_t w, int32_t h)
{
    int32_t rows = LOOK_STRIP_H + 3 * (LOOK_STRIP_BLUR / 2);
    rows = rows < h ? rows : h;
    uint64_t bytes = (uint64_t)w * (uint64_t)rows * 4;
    uint32_t *tmp = big_alloc(bytes), *line = big_alloc((uint64_t)(w > rows ? w : rows) * 8);
    fr.strip = big_alloc((uint64_t)w * LOOK_STRIP_H * 4);
    if (!tmp || !line || !fr.strip || !wallpaper_row(0)) {
        big_free(tmp, bytes);
        big_free(line, (uint64_t)(w > rows ? w : rows) * 8);
        big_free(fr.strip, (uint64_t)w * LOOK_STRIP_H * 4);
        fr.strip = NULL;
        return ERR_NO_MEMORY;
    }
    for (int32_t y = 0; y < rows; y++) {
        memcpy(tmp + (uint64_t)y * (uint32_t)w, wallpaper_row(y), (size_t)w * 4);
        blur_line(tmp + (uint64_t)y * (uint32_t)w, w, 1, LOOK_STRIP_BLUR, line);
    }
    for (int32_t x = 0; x < w; x++)
        blur_line(tmp + x, rows, w, LOOK_STRIP_BLUR, line);
    for (int32_t y = 0; y < LOOK_STRIP_H; y++)
        for (int32_t x = 0; x < w; x++) {
            uint32_t p = strip_px(tmp[(uint64_t)y * (uint32_t)w + (uint32_t)x]);
            if (y == LOOK_STRIP_H - 1)
                p = paint_mix(p, 0xffffff, LOOK_STRIP_LINE_A);
            fr.strip[(uint64_t)y * (uint32_t)w + (uint32_t)x] = p;
        }
    big_free(tmp, bytes);
    big_free(line, (uint64_t)(w > rows ? w : rows) * 8);
    return OK;
}

status_t frost_init(int32_t w, int32_t h)
{
    fr.w = w;
    fr.line_px = (uint32_t)(w > h ? w : h);
    for (uint32_t i = 0; i < pool_threads(); i++)
        if (!(fr.line[i] = big_alloc((uint64_t)fr.line_px * 8)))
            return ERR_NO_MEMORY;
    return make_strip(w, h);
}

const uint32_t *frost_strip_row(int32_t y)
{
    if (!fr.strip || y < 0 || y >= LOOK_STRIP_H)
        return NULL;
    return fr.strip + (uint64_t)y * (uint32_t)fr.w;
}

/* ---- the cards' backdrops ------------------------------------------------------------------- */

struct comp_box frost_card_box(enum frost_slot slot)
{
    switch (slot) {
    case FROST_MENU:
        return search.open ? search_box() : alttab.shown ? alttab_box()
                                                         : (struct comp_box){ 0, 0, 0, 0 };
    case FROST_POP:
        return pop.box;
    default:
        return notify_box();
    }
}

static bool same_box(struct comp_box a, struct comp_box b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

/* Does the scene's `under` damage meet b? */
static bool under_meets(struct comp_box b)
{
    for (uint32_t i = 0; i < scene.under.n; i++)
        if (!box_empty(box_intersect(scene.under.b[i], b)))
            return true;
    return false;
}

void frost_prepare(void)
{
    struct comp_box out = { 0, 0, scene.width, scene.height };
    for (int k = 0; k < FROST_SLOTS; k++) {
        struct slot *s = &fr.slot[k];
        struct comp_box want = box_intersect(frost_card_box((enum frost_slot)k), out);
        s->build = false;
        if (box_empty(want)) {
            s->have = (struct comp_box){ 0, 0, 0, 0 };
            continue;
        }
        if (same_box(want, s->have) && !under_meets(want))
            continue;
        s->have = want;
        s->build = true;
        desk_damage(want);
    }
}

/* Band `item` (TILE_H rows) of the slot being made: what is under it. */
static void compose_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)arg;
    struct slot *s = fr.doing;
    struct comp_box b = s->have;
    int32_t w = b.x2 - b.x1, y = b.y1 + (int32_t)item * TILE_H;
    struct tile_buf t = { { b.x1, y, b.x2, y + TILE_H < b.y2 ? y + TILE_H : b.y2 },
                          s->px + (uint64_t)(y - b.y1) * (uint32_t)w };
    paint_under(&t, worker);
}

static void blur_row(uint32_t item, uint32_t worker, void *arg)
{
    (void)arg;
    struct comp_box b = fr.doing->have;
    int32_t w = b.x2 - b.x1;
    blur_line(fr.doing->px + (uint64_t)item * (uint32_t)w, w, 1, fr.blur, fr.line[worker]);
}

static void blur_column(uint32_t item, uint32_t worker, void *arg)
{
    (void)arg;
    struct comp_box b = fr.doing->have;
    blur_line(fr.doing->px + item, b.y2 - b.y1, b.x2 - b.x1, fr.blur, fr.line[worker]);
}

/* Room in s for n pixels: false if there is none (the card is then drawn
 * over what is under it, unblurred). */
static bool room(struct slot *s, uint64_t n)
{
    if (n <= s->cap)
        return true;
    uint64_t cap = (n + 0xffff) & ~(uint64_t)0xffff;   /* in 64 Ki-pixel steps */
    uint32_t *px = big_alloc(cap * 4);
    if (!px)
        return false;
    big_free(s->px, s->cap * 4);
    s->px = px;
    s->cap = cap;
    return true;
}

void frost_build(void)
{
    for (int k = 0; k < FROST_SLOTS; k++) {
        struct slot *s = &fr.slot[k];
        if (!s->build)
            continue;
        s->build = false;
        struct comp_box b = s->have;
        int32_t w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (!room(s, (uint64_t)w * (uint64_t)h) || !fr.line[0] ||
            (uint32_t)(w > h ? w : h) > fr.line_px) {
            s->have = (struct comp_box){ 0, 0, 0, 0 };
            continue;
        }
        fr.doing = s;
        fr.blur = LOOK_GLASS_BLUR;
        pool_run(compose_band, NULL, (uint32_t)((h + TILE_H - 1) / TILE_H));
        pool_run(blur_row, NULL, (uint32_t)h);
        pool_run(blur_column, NULL, (uint32_t)w);
        fr.doing = NULL;
    }
}

const uint32_t *frost_row(enum frost_slot slot, int32_t x, int32_t y)
{
    const struct slot *s = &fr.slot[slot];
    if (!s->px || !box_contains(s->have, x, y))
        return NULL;
    return s->px + (uint64_t)(y - s->have.y1) * (uint32_t)(s->have.x2 - s->have.x1) +
           (uint32_t)(x - s->have.x1);
}
