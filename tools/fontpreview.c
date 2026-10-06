/* fontpreview: libfun's smooth text (<fun.h>, user/apps/fun/font.c and
 * fontdraw.c) on the Mac, built from the same files as Jam OS's.
 *
 *     fontpreview <out.png>    a picture of the floating windows' title
 *                              bars (style B1, docs/G1-PLAN.md "The look")
 *                              at 1x and 2x, focused and not, and sample
 *                              text at several sizes in both weights
 *     fontpreview --check      the drawing code against hostile input
 *                              (`make check` runs it built with ASan and
 *                              UBSan as build/host/fontcheck)
 *     fontpreview --panic <out.c>  the kernel's panic screen's glyphs
 *                              (tools/panicglyphs.c), which `make` links
 *                              into the kernel
 *
 * The check: random strings (malformed UTF-8 among them) drawn at random
 * places, partly or wholly off every edge, into a surface inside a
 * bigger buffer whose border must stay untouched, through random clip
 * rectangles, every pixel outside which must stay too; font_ellipsize's
 * result always fits its width and its buffer, is a start of the string
 * plus "…" when cut, and is the longest such start. Exit 0 when all of it
 * holds, 1 if not, 2 on a usage or file error.
 *
 * The font's block comes from mmap and is made read-only with mprotect,
 * as big_alloc and big_seal do on Jam OS (in the ASan build from malloc,
 * so ASan sees its exact size; not sealed then). Built as x86-64 (Rosetta
 * on an Apple-silicon Mac): <os.h> has x86 instructions in its inline
 * functions, and the floats round as on the PC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fun.h>
#include "fontpreview.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FONT_ASAN 1
#endif
#endif

/* ---- libfun's memory calls, for the host ------------------------------------------ */

void *big_alloc(uint64_t bytes)
{
#ifdef FONT_ASAN
    return calloc(1, bytes);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void big_free(void *p, uint64_t bytes)
{
#ifdef FONT_ASAN
    (void)bytes;
    free(p);
#else
    if (p)
        munmap(p, bytes);
#endif
}

status_t big_seal(void *p, uint64_t bytes)
{
#ifdef FONT_ASAN
    (void)p;
    (void)bytes;
    return OK;
#else
    return mprotect(p, bytes, PROT_READ) ? ERR_ACCESS_DENIED : OK;
#endif
}

/* ---- the picture ------------------------------------------------------------------- */

#define BACKDROP   0x15181cu
#define BODY       0x1c2025u
#define BAR_FOCUS  0x30363eu
#define BAR        0x262b31u
#define INK_FOCUS  0xe6e9ecu
#define INK        0x7f8892u
#define GREY       0x4a5058u
#define SAMPLE_INK 0xd8dce0u

static const uint32_t jam[3] = { 0xd4537e, 0xef9f27, 0x7f77dd };   /* close, minimise, full */

/* A rectangle of colour c (inside s): libfun's fill is in gfx.c, with the
 * screen, which the host doesn't build. */
static void box(const struct surf *s, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h && j < s->h; j++)
        for (int i = x; i < x + w && i < s->w; i++)
            s->px[j * s->stride + i] = c;
}

/* One floating window's title bar at scale k (1 or 2) with its top-left
 * corner at x, y, w pixels wide (at scale 1), and a strip of its body. */
static void title_bar(const struct surf *s, int x, int y, int w, int k, const struct font *f,
                      bool focused, const char *title)
{
    int bar_h = 28 * k;
    box(s, x, y, w * k, bar_h, focused ? BAR_FOCUS : BAR);
    box(s, x, y + bar_h, w * k, 36 * k, BODY);
    for (int i = 0; i < 3; i++) {
        float cx = (float)x + (float)k * (16.0f + 20.0f * (float)i);
        float cy = (float)y + (float)bar_h / 2.0f;
        disc_aa(s, cx, cy, 6.0f * (float)k, focused ? jam[i] : GREY, 255);
    }
    /* The title centred in the bar, kept clear of the circles on both sides. */
    struct rect r = { x + 72 * k, y, (w - 144) * k, bar_h };
    font_draw_in(s, &r, f, focused ? INK_FOCUS : INK, FONT_CENTRE, title);
}

struct faces {
    struct font *reg[2], *med[2];   /* 13 and 26 pixels to the em */
};

/* A line of sample text at each size in each weight from y down, its
 * label on the left; cut with "…" where the picture ends. The y after. */
static int sample_lines(const struct surf *s, int y)
{
    static const int sizes[] = { 11, 13, 16, 20, 26, 32 };
    static const char *text = "Jam OS: the quick brown fox jumps over the lazy dog. "
                              "Größe, café, ½ — “AVATAR” 0123456789";
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int w = FONT_REGULAR; w <= FONT_MEDIUM; w++) {
            struct font *f;
            if (font_open((enum font_weight)w, sizes[i], &f) != OK)
                return -1;
            int line = font_metrics(f)->line_h;
            char label[16];
            snprintf(label, sizeof(label), "%d px %s", sizes[i], w ? "Medium" : "Regular");
            font_draw_in(s, &(struct rect){ 24, y, 220, line }, f, INK, FONT_LEFT, label);
            font_draw_in(s, &(struct rect){ 254, y, s->w - 278, line }, f, SAMPLE_INK, FONT_LEFT,
                         text);
            y += line;
            font_close(f);
        }
        y += 6;
    }
    return y;
}

static int preview(const char *out)
{
    struct faces fc;
    for (int k = 0; k < 2; k++) {
        if (font_open(FONT_REGULAR, 13 << k, &fc.reg[k]) != OK ||
            font_open(FONT_MEDIUM, 13 << k, &fc.med[k]) != OK) {
            fprintf(stderr, "fontpreview: font_open failed\n");
            return 2;
        }
    }
    struct surf s = { calloc(1240 * 900, 4), 1240, 900, 1240 };
    box(&s, 0, 0, s.w, s.h, BACKDROP);
    const char *t1 = "jamjar — Jam OS’s music player", *t2 = "Terminal";
    const char *t3 = "sysmon: 8 CPUs, 2.0 GiB of memory, a title far too long to fit";
    title_bar(&s, 24, 24, 520, 1, fc.med[0], true, t1);
    title_bar(&s, 24, 112, 520, 1, fc.reg[0], false, t2);
    title_bar(&s, 568, 24, 360, 1, fc.med[0], true, t3);
    title_bar(&s, 568, 112, 360, 1, fc.reg[0], false, t3);
    title_bar(&s, 24, 200, 520, 2, fc.med[1], true, t1);
    title_bar(&s, 24, 348, 520, 2, fc.reg[1], false, t3);
    int y = sample_lines(&s, 500);
    printf("fontpreview: widths at 13 px: \"%s\" %d (Medium), \"%s\" %d (Regular); text to y %d\n",
           t1, font_width(fc.med[0], t1), t2, font_width(fc.reg[0], t2), y);
    int st = png_write(out, s.px, s.w, s.h, s.stride) ? 0 : 2;
    for (int k = 0; k < 2; k++) {
        font_close(fc.reg[k]);
        font_close(fc.med[k]);
    }
    free(s.px);
    return st;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--check"))
        return font_check();
    if (argc == 3 && !strcmp(argv[1], "--panic"))
        return panic_glyphs(argv[2]);
    if (argc == 2 && argv[1][0] != '-')
        return preview(argv[1]);
    fprintf(stderr, "usage: fontpreview <out.png> | --check | --panic <out.c>\n");
    return 2;
}
