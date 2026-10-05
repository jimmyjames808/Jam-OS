/* libfun: the smooth text's fonts, baked (fun.h "smooth text"; drawing
 * is fontdraw.c).
 *
 * font_open turns a built-in face (fontdata.c) at one size into a struct
 * font that drawing only reads: every slot's advance, its coverage at
 * each of FONT_PHASES horizontal positions, and the kerning between
 * slots, all in one block that is then made read-only. stb_truetype
 * (ttf.c) does the font's arithmetic and renders the coverage (each
 * pixel's exact share of the outline); everything it allocates is freed
 * before font_open returns.
 *
 * Sizes in the font's own units become 1/256 pixels with integers
 * (rounded to the nearest), so a width doesn't depend on how a compiler
 * rounds floats; only the outlines go through stb_truetype's floats.
 *
 * The kerning: the faces carry a 'kern' table of glyph pairs
 * (tools/subsetfont.py flattened Inter's GPOS into it). It becomes a list
 * of slot pairs sorted by pair, built through a dense table of every slot
 * pair (FONT_SLOTS squared, freed at once), so slots that share a glyph
 * (U+0020 and U+00A0) both get its pairs and no sort is needed. */
#include <stb_truetype.h>
#include "internal.h"

/* The punctuation slots after Latin-1, in order (tools/subsetfont.py's
 * EXTRA): en and em dash, quotes, bullet, ellipsis, euro. */
static const uint32_t font_extra[FONT_EXTRA] = {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x20ac,
};
_Static_assert(FONT_ELLIPSIS == FONT_ASCII + FONT_LATIN + 7, "U+2026 is font_extra[7]");
_Static_assert(FONT_SLOTS * FONT_SLOTS <= 65536, "a kerning pair fits 16 bits");

int font_slot(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7f)
        return (int)(cp - 0x20);
    if (cp >= 0xa0 && cp <= 0xff)
        return FONT_ASCII + (int)(cp - 0xa0);
    for (int i = 0; i < FONT_EXTRA; i++) {
        if (font_extra[i] == cp)
            return FONT_ASCII + FONT_LATIN + i;
    }
    return FONT_NOTDEF;
}

uint32_t font_slot_cp(int slot)
{
    if (slot < FONT_ASCII)
        return 0x20 + (uint32_t)slot;
    if (slot < FONT_ASCII + FONT_LATIN)
        return 0xa0 + (uint32_t)(slot - FONT_ASCII);
    if (slot < FONT_NOTDEF)
        return font_extra[slot - FONT_ASCII - FONT_LATIN];
    return 0;
}

/* What baking needs besides the font it fills. */
struct bake {
    stbtt_fontinfo info;
    int      px;                  /* pixels to the em */
    int      upm;                 /* the face's units to the em */
    float    scale;               /* pixels per unit, for stb_truetype */
    int      glyph[FONT_SLOTS];   /* each slot's glyph in the face (0: .notdef) */
    int16_t *kern;                /* FONT_SLOTS * FONT_SLOTS pairs, 1/256 pixels (malloc) */
    uint32_t nkern;               /* the non-zero ones */
    uint64_t cov_bytes;           /* every glyph's coverage */
};

/* v units of the face in 1/256 pixels, rounded to the nearest (half away
 * from zero). */
static int32_t units_256(const struct bake *b, int v)
{
    int64_t num = (int64_t)v * b->px * 256, den = b->upm;
    return (int32_t)(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den));
}

/* The face's units to the em: the 'head' table's unitsPerEm (offset 18),
 * big-endian; stb_truetype keeps only a scale made from it. */
static int units_per_em(const stbtt_fontinfo *info)
{
    const uint8_t *head = info->data + info->head;
    return head[18] << 8 | head[19];
}

static status_t bake_begin(struct bake *b, enum font_weight w, int px)
{
    size_t n = 0;
    const uint8_t *data = font_face(w, &n);
    if (!data)
        return ERR_OUT_OF_RANGE;
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (n > INT32_MAX || off < 0 || !stbtt_InitFont(&b->info, data, off))
        return ERR_NOT_SUPPORTED;
    b->px = px;
    b->upm = units_per_em(&b->info);
    if (b->upm < 16)
        return ERR_NOT_SUPPORTED;
    b->scale = stbtt_ScaleForMappingEmToPixels(&b->info, (float)px);
    for (int s = 0; s < FONT_SLOTS; s++) {
        uint32_t cp = font_slot_cp(s);
        b->glyph[s] = cp ? stbtt_FindGlyphIndex(&b->info, (int)cp) : 0;
    }
    return OK;
}

/* d as the kerning of every slot pair whose glyphs are g1 and g2. */
static void kern_put(struct bake *b, int g1, int g2, int16_t d)
{
    for (int l = 0; l < FONT_SLOTS; l++) {
        if (b->glyph[l] != g1)
            continue;
        for (int r = 0; r < FONT_SLOTS; r++) {
            if (b->glyph[r] == g2)
                b->kern[l * FONT_SLOTS + r] = d;
        }
    }
}

/* The face's kerning as a dense table of slot pairs. */
static status_t bake_kerning(struct bake *b)
{
    b->kern = calloc(FONT_SLOTS * FONT_SLOTS, sizeof(*b->kern));
    if (!b->kern)
        return ERR_NO_MEMORY;
    int n = stbtt_GetKerningTableLength(&b->info);
    stbtt_kerningentry *table = n > 0 ? malloc((size_t)n * sizeof(*table)) : NULL;
    if (n > 0 && !table)
        return ERR_NO_MEMORY;
    n = n > 0 ? stbtt_GetKerningTable(&b->info, table, n) : 0;
    for (int i = 0; i < n; i++) {
        int32_t d = units_256(b, table[i].advance);
        /* No real pair is near the limits: Inter's biggest (340 units) at 128
         * pixels to the em is 5,440. */
        if (d >= INT16_MIN && d <= INT16_MAX)
            kern_put(b, table[i].glyph1, table[i].glyph2, (int16_t)d);
    }
    free(table);
    b->nkern = 0;
    for (int i = 0; i < FONT_SLOTS * FONT_SLOTS; i++)
        b->nkern += b->kern[i] != 0;
    return OK;
}

/* Each glyph's box at each position (its coverage comes later), the
 * advances, and how many coverage bytes they need. */
static void bake_boxes(struct bake *b, struct font *f)
{
    b->cov_bytes = 0;
    for (int s = 0; s < FONT_SLOTS; s++) {
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&b->info, b->glyph[s], &adv, &lsb);
        f->adv[s] = units_256(b, adv);
        for (int p = 0; p < FONT_PHASES; p++) {
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&b->info, b->glyph[s], b->scale, b->scale,
                                            (float)p / FONT_PHASES, 0.0f, &x0, &y0, &x1, &y1);
            struct font_glyph *g = &f->g[s][p];
            *g = (struct font_glyph){ (int16_t)x0, (int16_t)y0, (uint16_t)(x1 - x0),
                                      (uint16_t)(y1 - y0), (uint32_t)b->cov_bytes };
            b->cov_bytes += (uint64_t)g->w * g->h;
        }
    }
}

/* The face's heights at this size. */
static void bake_metrics(struct bake *b, struct font *f)
{
    int asc = 0, desc = 0, gap = 0, x0 = 0, y0 = 0, x1 = 0, cap = 0;
    stbtt_GetFontVMetrics(&b->info, &asc, &desc, &gap);
    int h = stbtt_FindGlyphIndex(&b->info, 'H');
    if (!h || !stbtt_GetGlyphBox(&b->info, h, &x0, &y0, &x1, &cap))
        cap = asc;
    f->m = (struct font_metrics){
        .px = b->px,
        .ascent = (units_256(b, asc) + 255) >> 8,
        .descent = (units_256(b, -desc) + 255) >> 8,
        .line_h = (units_256(b, asc - desc + gap) + 128) >> 8,
        .cap_h = (units_256(b, cap) + 128) >> 8,
    };
}

/* The block: the font, its kerning pairs (in pair order), its coverage. */
static struct font *bake_block(const struct bake *b, const struct font *proto)
{
    uint64_t head = (sizeof(struct font) + 15) & ~15ull;
    uint64_t kern = (uint64_t)b->nkern * sizeof(struct font_kern);
    uint64_t bytes = head + kern + b->cov_bytes;
    struct font *f = big_alloc(bytes);
    if (!f)
        return NULL;
    *f = *proto;
    f->bytes = bytes;
    struct font_kern *k = (struct font_kern *)((uint8_t *)f + head);
    for (int i = 0, n = 0; i < FONT_SLOTS * FONT_SLOTS; i++) {
        if (b->kern[i])
            k[n++] = (struct font_kern){ (uint16_t)i, b->kern[i] };
    }
    f->kern = k;
    f->nkern = b->nkern;
    f->cov = (const uint8_t *)k + kern;
    return f;
}

/* Every glyph's coverage at every position, into f's block. */
static void bake_coverage(struct bake *b, struct font *f)
{
    uint8_t *cov = (uint8_t *)f->cov;
    for (int s = 0; s < FONT_SLOTS; s++) {
        for (int p = 0; p < FONT_PHASES; p++) {
            const struct font_glyph *g = &f->g[s][p];
            if (g->w && g->h)
                stbtt_MakeGlyphBitmapSubpixel(&b->info, cov + g->off, g->w, g->h, g->w, b->scale,
                                              b->scale, (float)p / FONT_PHASES, 0.0f,
                                              b->glyph[s]);
        }
    }
}

status_t font_open(enum font_weight w, int px, struct font **out)
{
    if (px < FONT_PX_MIN || px > FONT_PX_MAX)
        return ERR_OUT_OF_RANGE;
    struct bake *b = calloc(1, sizeof(*b));
    struct font *proto = calloc(1, sizeof(*proto));
    status_t st = b && proto ? OK : ERR_NO_MEMORY;
    if (st == OK)
        st = bake_begin(b, w, px);
    if (st == OK)
        st = bake_kerning(b);
    struct font *f = NULL;
    if (st == OK) {
        bake_boxes(b, proto);
        bake_metrics(b, proto);
        f = bake_block(b, proto);
        st = f ? OK : ERR_NO_MEMORY;
    }
    if (st == OK) {
        bake_coverage(b, f);
        st = big_seal(f, f->bytes);
        if (st != OK)
            big_free(f, f->bytes);
    }
    if (b)
        free(b->kern);
    free(b);
    free(proto);
    if (st == OK)
        *out = f;
    return st;
}

void font_close(struct font *f)
{
    if (f)
        big_free(f, f->bytes);
}

const struct font_metrics *font_metrics(const struct font *f)
{
    return &f->m;
}
