/* panicglyphs: fontpreview's --panic (tools/fontpreview.c): the kernel's
 * panic screen's glyphs, baked at build time into a C table the kernel
 * links (build/gen/panicglyphs.c; <jam/panictext.h> says what is in it and
 * why a crashed kernel needs it baked).
 *
 * Each size is opened with libfun's font_open, from the same Inter file
 * as the desktop's text, and the baked font's own tables are copied for
 * the characters the screen's words use: every glyph at its FONT_PHASES
 * positions, its advance, and the kerning between them. Then a few lines
 * are drawn with libfun's font_draw and hashed, for the kernel's test that
 * its own drawing of them gives the same pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jam/panictext.h>
#include "fontpreview.h"
#include "internal.h"

_Static_assert(PANIC_PHASES == FONT_PHASES, "the kernel lays text out as libfun does");

/* The code point a character of the screen's words is baked as. */
static uint32_t cp_of(char c)
{
    return c == '\'' ? 0x2019u : (uint32_t)(unsigned char)c;
}

/* The characters of `words` (ASCII; '%' is a format's, not a word's), in
 * ASCII order. */
static int chars_of(const char *words, char *out)
{
    bool have[128] = { false };
    for (const char *p = words; *p; p++)
        if (*p >= 0x20 && *p < 0x7f && *p != '%')
            have[(unsigned char)*p] = true;
    int n = 0;
    for (int c = 0x20; c < 0x7f; c++)
        if (have[c])
            out[n++] = (char)c;
    out[n] = 0;
    return n;
}

/* libfun's kerning between slots l and r (its pairs are few: a scan). */
static int kern_of(const struct font *f, int l, int r)
{
    uint32_t key = (uint32_t)(l * FONT_TEXT_SLOTS + r);
    for (uint32_t i = 0; i < f->nkern; i++)
        if (f->kern[i].pair == key)
            return f->kern[i].d;
    return 0;
}

static bool fits_i8(int v)
{
    return v >= -128 && v <= 127;
}

/* One size, as `name` (title or small): its tables and its struct. */
static int write_font(FILE *o, const char *name, int px, const char *words)
{
    struct font *f;
    if (font_open(FONT_REGULAR, px, &f) != OK) {
        fprintf(stderr, "panicglyphs: font_open %d failed\n", px);
        return 1;
    }
    char ch[128];
    int n = chars_of(words, ch);
    fprintf(o, "/* %s: Inter Regular at %d px, \"%s\" */\n", name, px, ch);
    fprintf(o, "static const int8_t %s_slot_of[128] = {", name);
    for (int c = 0; c < 128; c++) {
        const char *at = c ? strchr(ch, c) : NULL;
        fprintf(o, "%s%d,", c % 16 ? " " : "\n    ", at ? (int)(at - ch) : -1);
    }
    fprintf(o, "\n};\nstatic const int32_t %s_adv[%d] = {", name, n);
    for (int i = 0; i < n; i++)
        fprintf(o, "%s%d,", i % 8 ? " " : "\n    ", f->adv[font_slot(cp_of(ch[i]))]);
    fprintf(o, "\n};\nstatic const struct panic_glyph %s_g[%d][PANIC_PHASES] = {\n", name, n);
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        fprintf(o, "    {");
        for (int q = 0; q < FONT_PHASES; q++) {
            const struct font_glyph *g = &f->g[font_slot(cp_of(ch[i]))][q];
            if (!fits_i8(g->x) || !fits_i8(g->y) || g->w > 255 || g->h > 255) {
                fprintf(stderr, "panicglyphs: '%c' at %d px doesn't fit the table\n", ch[i], px);
                return 1;
            }
            fprintf(o, " { %d, %d, %u, %u, %u },", g->x, g->y, g->w, g->h, off);
            off += (uint32_t)g->w * g->h;
        }
        fprintf(o, " },   /* '%c' */\n", ch[i]);
    }
    fprintf(o, "};\nstatic const struct panic_kern %s_kern[] = {\n", name);
    int nk = 0;
    for (int l = 0; l < n; l++)
        for (int r = 0; r < n; r++) {
            int d = kern_of(f, font_slot(cp_of(ch[l])), font_slot(cp_of(ch[r])));
            if (d) {
                fprintf(o, "    { %d, %d, %d },   /* \"%c%c\" */\n", l, r, d, ch[l], ch[r]);
                nk++;
            }
        }
    if (!nk)
        fprintf(o, "    { 0, 0, 0 },   /* none: an entry for C, not counted */\n");
    fprintf(o, "};\nstatic const uint8_t %s_cov[%u] = {", name, off ? off : 1);
    uint32_t k = 0;
    for (int i = 0; i < n; i++)
        for (int q = 0; q < FONT_PHASES; q++) {
            const struct font_glyph *g = &f->g[font_slot(cp_of(ch[i]))][q];
            for (uint32_t b = 0; b < (uint32_t)g->w * g->h; b++, k++)
                fprintf(o, "%s%u,", k % 20 ? " " : "\n    ", f->cov[g->off + b]);
        }
    const struct font_metrics *m = font_metrics(f);
    fprintf(o, "\n};\nconst struct panic_font panic_font_%s = {\n"
               "    %d, %d, %d, %d, %d, %s_slot_of, %s_adv, %s_g, %d, %s_kern, %s_cov,\n};\n\n",
            name, m->px, m->ascent, m->descent, m->cap_h, n, name, name, name, nk, name, name);
    font_close(f);
    return 0;
}

/* text with each ASCII apostrophe as U+2019, in UTF-8: what the kernel
 * draws for it. */
static void typographic(const char *text, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = text; *p && k + 4 < n; p++) {
        if (*p == '\'') {
            memcpy(out + k, "\xe2\x80\x99", 3);
            k += 3;
        } else {
            out[k++] = *p;
        }
    }
    out[k] = 0;
}

/* text drawn by libfun on a picture of its own, as the kernel's test draws it. */
static int write_ref(FILE *o, const char *text, bool title)
{
    struct font *f;
    if (font_open(FONT_REGULAR, title ? PANIC_TITLE_PX : PANIC_SMALL_PX, &f) != OK)
        return 1;
    char utf8[256];
    typographic(text, utf8, sizeof(utf8));
    const struct font_metrics *m = font_metrics(f);
    int x = 3, y = m->ascent + 2, w = font_width(f, utf8) + 6, h = m->ascent + m->descent + 4;
    uint32_t rgb = title ? PANIC_INK_TITLE : PANIC_INK_SMALL;
    struct surf s = { calloc((size_t)w * h, 4), w, h, w };
    for (int i = 0; i < w * h; i++)
        s.px[i] = PANIC_BG;
    font_draw(&s, NULL, f, x, y, rgb, utf8);
    uint32_t hash = 2166136261u;
    for (int i = 0; i < w * h; i++)
        hash = (hash ^ s.px[i]) * 16777619u;
    fprintf(o, "    { \"%s\", %d, 0x%06x, 0x%06x, %d, %d, %d, %d, 0x%08x },\n", text, title, rgb,
            PANIC_BG, w, h, x, y, hash);
    free(s.px);
    font_close(f);
    return 0;
}

int panic_glyphs(const char *path)
{
    FILE *o = fopen(path, "w");
    if (!o) {
        perror(path);
        return 2;
    }
    fprintf(o, "/* The panic screen's glyphs (<jam/panictext.h>): generated by\n"
               " * tools/panicglyphs.c from third_party/inter with libfun's font code.\n"
               " * Do not edit; `make` makes it again. */\n"
               "#include <jam/panictext.h>\n\n");
    int st = write_font(o, "title", PANIC_TITLE_PX, PANIC_TITLE_CHARS);
    if (!st)
        st = write_font(o, "small", PANIC_SMALL_PX, PANIC_SMALL_CHARS);
    fprintf(o, "const struct panic_text_ref panic_text_refs[] = {\n");
    static const char *const titles[] = { PANIC_TITLE_RESTARTING, PANIC_TITLE_STUCK,
                                          PANIC_TITLE_NORESET };
    static const char *const smalls[] = { "JAM-PF-7F3A", "JAM-OOM-0042", "Restarting the PC in 15 s",
                                          PANIC_SMALL_POWER };
    for (unsigned i = 0; !st && i < sizeof(titles) / sizeof(titles[0]); i++)
        st = write_ref(o, titles[i], true);
    for (unsigned i = 0; !st && i < sizeof(smalls) / sizeof(smalls[0]); i++)
        st = write_ref(o, smalls[i], false);
    fprintf(o, "};\nconst unsigned panic_text_nrefs = %u;\n",
            (unsigned)(sizeof(titles) / sizeof(titles[0]) + sizeof(smalls) / sizeof(smalls[0])));
    if (fclose(o) || st) {
        fprintf(stderr, "panicglyphs: %s not written\n", path);
        remove(path);
        return 2;
    }
    return 0;
}
