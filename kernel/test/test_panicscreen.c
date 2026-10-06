/* The calm panic screen's parts (<jam/panicscreen.h>), each on a buffer of
 * the test's own: the code's table, the baked text against libfun's own
 * drawing of it, the ring's pixels, the 8x16 panel text and the layout.
 * Nothing here panics or touches the framebuffer. */
#include <stdint.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/panicscreen.h>
#include <jam/string.h>

#define GUARD 0x5a5a5au

extern const uint8_t font_8x16[128][16];   /* kernel/dev/font_8x16.c */   /* a buffer's colour where nothing may be drawn */

static bool code_is(const struct panic_cause *c, const char *want)
{
    if (strcmp(c->code, want)) {
        kprintf("panicscreen: code %s, want %s\n", c->code, want);
        return false;
    }
    return true;
}

KTEST(panicscreen_code_traps)
{
    struct panic_cause c;
    panic_cause_trap(&c, 14, 0xffffffff80101234ull, 0x7f3a);
    KT_ASSERT(code_is(&c, "JAM-PF-7F3A"));   /* a page fault: the faulting address */
    panic_cause_trap(&c, 14, 0xffffffff80101234ull, 0x8);
    KT_ASSERT(code_is(&c, "JAM-PF-0008"));
    panic_cause_trap(&c, 13, 0xffffffff8012abcdull, 0x7f3a);
    KT_ASSERT(code_is(&c, "JAM-GP-ABCD"));   /* the others: RIP */
    panic_cause_trap(&c, 6, 0xffffffff80100001ull, 0);
    KT_ASSERT(code_is(&c, "JAM-UD-0001"));
    panic_cause_trap(&c, 8, 0, 0xffff800000123000ull);
    KT_ASSERT(code_is(&c, "JAM-DF-0000"));
    panic_cause_trap(&c, 18, 0xffffffff8010beefull, 0);
    KT_ASSERT(code_is(&c, "JAM-MC-BEEF"));
    panic_cause_trap(&c, 2, 0x1234, 0);
    KT_ASSERT(code_is(&c, "JAM-NMI-1234"));
    panic_cause_trap(&c, 15, 0x10, 0);       /* reserved */
    KT_ASSERT(code_is(&c, "JAM-EX-0010"));
    panic_cause_trap(&c, 9, 0x10, 0);        /* no mnemonic */
    KT_ASSERT(code_is(&c, "JAM-EX-0010"));
    panic_cause_trap(&c, 40, 0x10, 0);       /* not an exception at all */
    KT_ASSERT(code_is(&c, "JAM-EX-0010"));
    panic_cause_watchdog(&c, 0xffffffff80100f00ull);
    KT_ASSERT(code_is(&c, "JAM-WD-0F00"));
    KT_ASSERT(!strcmp(c.hex_of, "RIP") && c.addr == 0xffffffff80100f00ull);
}

KTEST(panicscreen_code_messages)
{
    static const struct {
        const char *msg, *code;
    } rows[] = {
        { "assertion failed: x (kernel/a.c:1)", "JAM-AS-C0DE" },
        { "spinlock \"pmm\" stuck for 5 s on cpu 1, held by cpu 2", "JAM-WD-C0DE" },
        { "lockdep: lock order inversion: taking \"a\" while holding \"b\"", "JAM-LK-C0DE" },
        { "mutex \"m\": recursive lock by \"t\"", "JAM-LK-C0DE" },
        { "sched: out of memory for thread \"x\"", "JAM-OOM-C0DE" },
        { "test panic requested (crash test)", "JAM-KP-C0DE" },
        { "", "JAM-KP-C0DE" },
    };
    struct panic_cause c;
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        panic_cause_message(&c, rows[i].msg, 0xffffffff8010c0deull);
        KT_ASSERT(code_is(&c, rows[i].code));
        KT_ASSERT(c.addr == 0xffffffff8010c0deull && c.what && c.hex_of);
    }
}

/* Every character of the screen's words is baked in its size (an
 * apostrophe too, as U+2019), and every kind's letters in the small one. */
static bool baked(const struct panic_font *f, const char *s)
{
    for (; *s; s++)
        if (*s != '%' && f->slot_of[(unsigned char)*s] < 0) {
            kprintf("panicscreen: '%c' isn't baked at %d px\n", *s, f->px);
            return false;
        }
    return true;
}

KTEST(panicscreen_words_baked)
{
    KT_ASSERT(baked(&panic_font_title, PANIC_TITLE_RESTARTING));
    KT_ASSERT(baked(&panic_font_title, PANIC_TITLE_STUCK));
    KT_ASSERT(baked(&panic_font_title, PANIC_TITLE_NORESET));
    KT_ASSERT(baked(&panic_font_small, PANIC_SMALL_COUNTDOWN));
    KT_ASSERT(baked(&panic_font_small, PANIC_SMALL_POWER));
    KT_ASSERT(baked(&panic_font_small, PANIC_CODE_CHARS "0123456789"));
    KT_EQ(panic_font_title.px, PANIC_TITLE_PX);
    KT_EQ(panic_font_small.px, PANIC_SMALL_PX);
    KT_ASSERT(panic_font_title.slot_of['\''] >= 0);
}

static struct panic_canvas canvas_of(uint32_t *px, uint32_t w, uint32_t h)
{
    return (struct panic_canvas){ px, w, h, w, 16, 8, 0 };
}

static uint32_t hash_of(const uint32_t *px, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ px[i]) * 16777619u;
    return h;
}

/* The lines tools/panicglyphs.c drew with libfun's font_draw, drawn here
 * the kernel's way: the same pixels, and the same width. */
KTEST(panicscreen_text_matches_libfun)
{
    KT_ASSERT(panic_text_nrefs >= 4);
    for (unsigned i = 0; i < panic_text_nrefs; i++) {
        const struct panic_text_ref *r = &panic_text_refs[i];
        const struct panic_font *f = r->title ? &panic_font_title : &panic_font_small;
        uint32_t n = (uint32_t)r->w * (uint32_t)r->h;
        uint32_t *px = kmalloc(n * 4);
        KT_ASSERT(px);
        struct panic_canvas c = canvas_of(px, (uint32_t)r->w, (uint32_t)r->h);
        panic_fill(&c, 0, 0, r->w, r->h, r->bg);
        panic_text(&c, f, r->x, r->y, r->rgb, r->text);
        uint32_t h = hash_of(px, n);
        kfree(px);
        if (h != r->hash)
            kprintf("panicscreen: \"%s\" hashes %08x, libfun's %08x\n", r->text, h, r->hash);
        KT_EQ(h, r->hash);
        KT_EQ(panic_text_width(f, r->text), r->w - 2 * r->x);
    }
}

KTEST(panicscreen_sin)
{
    KT_EQ(panic_sin(0), 0);
    KT_EQ(panic_sin(0x4000), 16384);
    KT_EQ(panic_sin(0x8000), 0);
    KT_EQ(panic_sin(0xc000), -16384);
    KT_EQ(panic_sin(0x2000), 11585);      /* 45 degrees */
    KT_EQ(panic_sin(0x10000 + 0x2000), 11585);
    for (uint32_t t = 0; t < 0x10000; t += 97) {
        KT_EQ(panic_sin(t + 0x8000), -panic_sin(t));
        KT_EQ(panic_sin(0x8000 - t), panic_sin(t));
    }
}

#define RW  48   /* the ring's test picture: the ring's box with a border */
#define RCX 24
#define RCY 24

static uint32_t ring_at(const uint32_t *px, int dx, int dy)
{
    return px[(RCY + dy) * RW + RCX + dx];
}

/* Exact pixels at a few turns (worked out by hand from cursors.svg's
 * measures at 5/3 pixel a unit), nothing written outside the box, and a
 * quarter turn on is the same picture turned a quarter. */
KTEST(panicscreen_ring)
{
    static uint32_t a[RW * RW], b[RW * RW];
    struct panic_canvas ca = canvas_of(a, RW, RW), cb = canvas_of(b, RW, RW);
    panic_fill(&ca, 0, 0, RW, RW, GUARD);
    panic_ring(&ca, RCX, RCY, 0);
    for (int y = 0; y < RW; y++)
        for (int x = 0; x < RW; x++) {
            bool box = x >= RCX - 20 && x < RCX + 20 && y >= RCY - 20 && y < RCY + 20;
            KT_ASSERT(box == (a[y * RW + x] != GUARD));
        }
    KT_EQ(ring_at(a, 8, 10), 0xd4537e);    /* the arc's middle (50 degrees): raspberry */
    KT_EQ(ring_at(a, -11, 8), 0xf6f3f8);   /* 141 degrees: past the arc, the white */
    KT_EQ(ring_at(a, -14, 0), 0xf6f3f8);   /* on the ring, the far side */
    KT_EQ(ring_at(a, -17, 0), 0x262a35);   /* the outline outside the white */
    KT_EQ(ring_at(a, -1, -1), PANIC_BG);   /* the middle */
    KT_EQ(ring_at(a, -20, 0), PANIC_BG);   /* the box's edge */
    uint32_t edge = ring_at(a, -18, 0);    /* the outline's outer edge: blended */
    KT_ASSERT(edge != PANIC_BG && edge != 0x262a35);

    panic_ring(&ca, RCX, RCY, 0x4000);   /* a quarter turn: the arc at 90..190 degrees */
    KT_EQ(ring_at(a, -11, 8), 0xd4537e);
    KT_EQ(ring_at(a, 8, 10), 0xf6f3f8);

    panic_ring(&ca, RCX, RCY, 1000);
    panic_ring(&cb, RCX, RCY, 1000 + 0x4000);
    for (int j = -20; j < 20; j++)
        for (int i = -20; i < 20; i++)
            KT_EQ(ring_at(b, -j - 1, i), ring_at(a, i, j));
}

KTEST(panicscreen_mono)
{
    static uint32_t px[24 * 18];
    struct panic_canvas c = canvas_of(px, 24, 18);
    panic_fill(&c, 0, 0, 24, 18, GUARD);
    panic_mono(&c, 1, 1, 0xffffff, 0x000000, "A-Z", 2);   /* two characters only */
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 24; x++) {
            uint32_t want = GUARD;
            if (x >= 1 && x < 17 && y >= 1 && y < 17) {
                char ch = x < 9 ? 'A' : '-';
                int i = (x - 1) % 8;
                want = font_8x16[(int)ch][y - 1] & (0x80 >> i) ? 0xffffff : 0x000000;
            }
            KT_EQ(px[y * 24 + x], want);
        }
}

/* Everything fits on the screen and in order down it, at the PC's size,
 * OVMF's and a small one. */
KTEST(panicscreen_layout)
{
    static const uint32_t sizes[][2] = { { 2560, 1440 }, { 1280, 800 }, { 800, 600 } };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t w = sizes[i][0], h = sizes[i][1];
        struct panic_layout l;
        panic_layout(w, h, &l);
        KT_EQ(l.ring_x, (int)w / 2);
        KT_ASSERT(l.ring_y - PANIC_RING_PX / 2 > 0);
        KT_ASSERT(l.ring_y + PANIC_RING_PX / 2 < l.title_y - panic_font_title.cap_h);
        KT_ASSERT(l.title_y + panic_font_title.descent < l.small_y - panic_font_small.cap_h);
        KT_ASSERT(l.small_y + panic_font_small.descent < l.code_y - panic_font_small.cap_h);
        KT_ASSERT(l.code_y + panic_font_small.descent < l.panel_y);
        KT_ASSERT(l.panel_x >= 16 && l.panel_x + l.panel_w <= (int)w - 16);
        KT_ASSERT(l.panel_y + l.panel_h <= (int)h);
        KT_ASSERT(16 + 8 * l.panel_cols <= l.panel_w - 16);
        KT_ASSERT(l.panel_h >= PANIC_PANEL_LINES * 16 + 24);
    }
    struct panic_layout l;
    panic_layout(1280, 800, &l);
    KT_EQ(l.panel_w, PANIC_PANEL_W);
    KT_EQ(l.panel_cols, 88);
}
