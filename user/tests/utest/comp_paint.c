/* utest: the compositor's painting (user/services/compositor paint.c,
 * title.c, cursor.c), through bin/compositor's test scene run headless:
 * windows of known pixels with no client (testscene.c), composed into an
 * image VMO of ours, each paint reported on a channel of ours. Every
 * expectation is computed here from the scene's description, with a plain
 * painter's algorithm and our own px_over, never by the compositor's code.
 *
 * t_comp_paint_overlap: opaque and translucent windows overlapping each
 * other, the background and the output's edges: every pixel exact (the
 * blend's rounding included).
 * t_comp_paint_cull: windows hidden by opaque ones (xrgb, and argb with an
 * opaque region, whose pixels are then copied, alpha ignored) are skipped:
 * the pixels drawn from windows are exactly the visible layers', and the
 * image is still exact.
 * t_comp_paint_damage: only the damage is painted, once per pixel however
 * the boxes overlap: a window moved paints its old and new place, the
 * cursor appearing paints its box, nothing to paint paints nothing.
 * t_comp_paint_fullscreen: a full-screen opaque window is copied straight
 * from its buffer (every tile direct), except under the cursor; a window
 * over it turns that off.
 * t_comp_paint_title: title bars, close boxes and borders, focused and
 * not, drawn opaquely inside the frame and nowhere else.
 * t_comp_paint_cursor: a client's cursor surface at its hot spot, blended;
 * when it shrinks (a commit of a smaller buffer) its old box is painted
 * again, so nothing of it is left behind; a move paints both boxes.
 * t_comp_paint_blank: blank shows the background only, no cursor. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include <splash.h>
#include "paint.h"
#include "testscene.h"
#include "utest.h"

#define CP_W       320
#define CP_H       200
#define CP_REPORTS 16
#define CP_WAIT    (30 * NS_PER_S)

/* One run of the test scene: its image and its reports. */
struct cp_run {
    uint32_t *image;           /* CP_W * CP_H, 0x00RRGGBB, ours (mapped read-only) */
    uint64_t image_size;
    struct testscene_report rep[CP_REPORTS];
    unsigned nrep;
};

static bool cp_map_image(struct cp_run *r, handle_t *theirs)
{
    uint64_t size = ((uint64_t)CP_W * CP_H * 4 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    handle_t v;
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(size, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, size, VMAR_READ, &addr), OK);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, theirs), OK);
    jam_handle_close(v);
    r->image = (uint32_t *)(uintptr_t)addr;
    r->image_size = size;
    return true;
}

/* Every report the compositor sent. */
static bool cp_reports(struct cp_run *r, handle_t ch)
{
    for (;;) {
        struct testscene_report rep;
        uint32_t got[2] = { 0, 0 };
        struct channel_read_args a = { .h = ch, .bytes_cap = sizeof(rep),
                                       .bytes = (uint64_t)(uintptr_t)&rep,
                                       .actual_bytes = (uint64_t)(uintptr_t)&got[0],
                                       .actual_handles = (uint64_t)(uintptr_t)&got[1] };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED)
            return true;
        CHECK_ST(st, OK);
        CHECK_EQ(got[0], sizeof(rep));
        CHECK(r->nrep < CP_REPORTS);
        r->rep[r->nrep++] = rep;
    }
}

/* bin/compositor headless at CP_W x CP_H running the scene's commands; it
 * must exit 0, leaving its job empty. */
static bool cp_run(const char *const *cmds, unsigned n, struct cp_run *r)
{
    memset(r, 0, sizeof(*r));
    handle_t image, svc, svc_theirs, rep, rep_theirs, job, proc;
    if (!cp_map_image(r, &image))
        return false;
    CHECK_ST(jam_channel_create(&svc, &svc_theirs), OK);
    CHECK_ST(jam_channel_create(&rep, &rep_theirs), OK);
    CHECK_ST(new_job(&job), OK);
    char size[32];
    snprintf(size, sizeof(size), "size=%dx%d", CP_W, CP_H);
    const char *argv[24] = { "bin/compositor", "headless", size, "threads=3", "testscene" };
    CHECK(n <= 24 - 5);
    for (unsigned i = 0; i < n; i++)
        argv[5 + i] = cmds[i];
    struct spawn_handle x[] = { { SR_USER + 0, svc_theirs }, { SR_USER + 1, image },
                                { SR_USER + TESTSCENE_REPORT_ROLE, rep_theirs } };
    struct spawn_args a = { .path = "bin/compositor", .argc = (int)(5 + n), .argv = argv,
                            .job = job, .extra = x, .nextra = 3 };
    CHECK_ST(spawn(&a, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, CP_WAIT, &info), OK);
    CHECK_EQ(info.killed, 0);
    CHECK_EQ(info.exit_code, 0);
    jam_handle_close(proc);
    bool ok = cp_reports(r, rep);
    jam_handle_close(rep);
    jam_handle_close(svc);
    jam_handle_close(job);
    return ok;
}

static void cp_done(struct cp_run *r)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)r->image,
                         r->image_size);
}

/* ---- the reference painter ---------------------------------------------------------------- */

/* One window as a `win=` command says it. */
struct cp_win {
    int32_t x, y, w, h;
    uint32_t argb;             /* AARRGGBB */
    bool solid, opaque_region;
};

static uint32_t div255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* src (premultiplied) over the opaque dst, as fun.h documents px_over. */
static uint32_t ref_over(uint32_t dst, uint32_t src)
{
    uint32_t inv = 255 - (src >> 24), out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t c = (src >> sh & 0xff) + div255((dst >> sh & 0xff) * inv);
        out |= (c > 255 ? 255 : c) << sh;
    }
    return out;
}

/* The windows over the background, bottom to top, into img. */
static void ref_paint(uint32_t *img, const struct cp_win *v, unsigned n)
{
    for (unsigned i = 0; i < CP_W * CP_H; i++)
        img[i] = SPLASH_BG;
    for (unsigned k = 0; k < n; k++) {
        const struct cp_win *w = &v[k];
        uint32_t a = w->argb >> 24;
        for (int32_t y = w->y < 0 ? 0 : w->y; y < w->y + w->h && y < CP_H; y++)
            for (int32_t x = w->x < 0 ? 0 : w->x; x < w->x + w->w && x < CP_W; x++) {
                uint32_t rgb = testscene_rgb(w->argb, x - w->x, y - w->y, w->solid);
                uint32_t *p = &img[y * CP_W + x];
                if (a == 0xff)
                    *p = rgb;
                else if (w->opaque_region)
                    *p = testscene_pm(rgb, a) & 0xffffff;   /* taken as opaque: copied */
                else
                    *p = ref_over(*p, testscene_pm(rgb, a));
            }
    }
}

/* The `win=` command for w. */
static void win_cmd(char *buf, size_t n, const struct cp_win *w)
{
    snprintf(buf, n, "win=%d,%d,%d,%d,%08x,%s%s", w->x, w->y, w->w, w->h, w->argb,
             w->solid ? "s" : "", w->opaque_region ? "o" : "");
}

/* Every pixel of the image as the reference has it, except inside skip. */
static bool same_image(const struct cp_run *r, const uint32_t *want, struct comp_box skip)
{
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 0; x < CP_W; x++) {
            if (x >= skip.x1 && x < skip.x2 && y >= skip.y1 && y < skip.y2)
                continue;
            uint32_t got = r->image[y * CP_W + x], w = want[y * CP_W + x];
            if (got != w)
                FAIL("pixel (%d, %d) is %06x, want %06x", x, y, got, w);
        }
    return true;
}

static const struct comp_box no_skip = { 0, 0, 0, 0 };

/* Run windows v plus the commands after them; check the image exactly. */
static bool run_windows(const struct cp_win *v, unsigned n, const char *const *more, unsigned nmore,
                        struct cp_run *r)
{
    static char cmd[8][64];
    static uint32_t want[CP_W * CP_H];
    const char *cmds[16];
    CHECK(n <= 8 && n + nmore <= 16);
    for (unsigned i = 0; i < n; i++) {
        win_cmd(cmd[i], sizeof(cmd[i]), &v[i]);
        cmds[i] = cmd[i];
    }
    for (unsigned i = 0; i < nmore; i++)
        cmds[n + i] = more[i];
    if (!cp_run(cmds, n + nmore, r))
        return false;
    ref_paint(want, v, n);
    return same_image(r, want, no_skip);
}

/* ---- the tests ----------------------------------------------------------------------------- */

bool t_comp_paint_overlap(void)
{
    static const struct cp_win v[] = {
        { 20, 30, 160, 100, 0xff3060a0, false, false },    /* opaque */
        { 100, 70, 150, 90, 0xff70a030, false, false },    /* opaque, over the first */
        { 140, 20, 120, 120, 0x80602040, false, false },   /* half alpha over both and the bg */
        { -30, 150, 100, 80, 0x40404040, false, false },   /* off the left and bottom edges */
        { 280, -20, 80, 60, 0xff808080, true, false },     /* solid, off the top and right */
        { 60, 90, 40, 40, 0x01010101, false, false },      /* nearly transparent */
    };
    static const char *const paint[] = { "paint" };
    struct cp_run r;
    bool ok = run_windows(v, 6, paint, 1, &r);
    if (ok) {
        CHECK_EQ(r.nrep, 1);
        CHECK_EQ(r.rep[0].px, CP_W * CP_H);   /* the first paint: everything, once */
        CHECK_EQ(r.rep[0].direct, 0);
    }
    cp_done(&r);
    return ok;
}

/* Pixels drawn from windows, row by row: from the topmost window hiding
 * the whole row (full-width opaque ones, in these scenes) up. */
static uint64_t visible_layers(const struct cp_win *v, unsigned n)
{
    uint64_t sum = 0;
    for (int32_t y = 0; y < CP_H; y++) {
        unsigned from = 0;
        for (unsigned k = 0; k < n; k++)
            if (v[k].x <= 0 && v[k].x + v[k].w >= CP_W && y >= v[k].y && y < v[k].y + v[k].h &&
                ((v[k].argb >> 24) == 0xff || v[k].opaque_region))
                from = k;
        for (unsigned k = from; k < n; k++) {
            if (y < v[k].y || y >= v[k].y + v[k].h)
                continue;
            int32_t x1 = v[k].x < 0 ? 0 : v[k].x, x2 = v[k].x + v[k].w;
            sum += (uint64_t)((x2 < CP_W ? x2 : CP_W) - x1);
        }
    }
    return sum;
}

bool t_comp_paint_cull(void)
{
    /* Edges on multiples of 16 rows, so hiding a whole row hides whole tiles. */
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff102030, false, false },      /* everything, opaque */
        { 40, 16, 100, 64, 0xff405060, false, false },       /* opaque, then hidden */
        { 0, 48, CP_W, 64, 0xff506070, false, false },       /* full width: hides rows 48..112 */
        { 0, 128, CP_W, 48, 0xc0a0b0c0, false, true },       /* argb, opaque region: 128..176 */
        { 100, 8, 50, 140, 0x80ff4000, false, false },       /* translucent over all of them */
    };
    static const char *const paint[] = { "paint" };
    struct cp_run r;
    bool ok = run_windows(v, 5, paint, 1, &r);
    if (ok) {
        uint64_t all = 0, want = visible_layers(v, 5);
        for (unsigned k = 0; k < 5; k++)
            all += (uint64_t)v[k].w * (uint64_t)v[k].h;
        CHECK_EQ(r.nrep, 1);
        CHECK(want < all);
        CHECK_EQ(r.rep[0].layer_px, want);
    }
    cp_done(&r);
    return ok;
}

/* The arrow's box at (x, y) on a CP_H-line output (scale 1). */
static struct comp_box arrow_at(int32_t x, int32_t y)
{
    return (struct comp_box){ x, y, x + POINTER_ARROW_W, y + POINTER_ARROW_H };
}

bool t_comp_paint_damage(void)
{
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff203040, false, false },
        { 50, 50, 100, 60, 0xff506070, false, false },
    };
    static const char *const more[] = {
        "paint",
        "damage=10,10,40,30", "damage=30,20,40,30", "damage=12,12,5,5", "paint",
        "move=2,60,70", "paint",
        "cursor=200,100", "paint",
        "paint",
    };
    static const char *const cmds_v[2] = { "win=0,0,320,200,ff203040,", "win=50,50,100,60,ff506070," };
    const char *cmds[2 + sizeof(more) / sizeof(more[0])];
    for (unsigned i = 0; i < 2; i++)
        cmds[i] = cmds_v[i];
    for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++)
        cmds[2 + i] = more[i];
    struct cp_run r;
    static uint32_t want[CP_W * CP_H];
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        struct cp_win moved[2] = { v[0], v[1] };
        moved[1].x = 60;
        moved[1].y = 70;
        ref_paint(want, moved, 2);
        ok = same_image(&r, want, arrow_at(200, 100));
    }
    if (ok) {
        CHECK_EQ(r.nrep, 5);
        CHECK_EQ(r.rep[0].px, CP_W * CP_H);
        CHECK_EQ(r.rep[1].px, 40 * 30 + 40 * 30 - 20 * 20);   /* the union, the third inside */
        CHECK_EQ(r.rep[2].px, 100 * 60 * 2 - 90 * 40);        /* the old place and the new */
        CHECK_EQ(r.rep[3].px, POINTER_ARROW_W * POINTER_ARROW_H);
        CHECK_EQ(r.rep[4].px, 0);
        CHECK_EQ(r.image[100 * CP_W + 200], 0x000000);   /* the arrow's tip: outline */
        CHECK_EQ(r.image[103 * CP_W + 201], 0xffffff);   /* inside it: fill */
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_fullscreen(void)
{
    static const char *const cmds[] = {
        "win=0,0,320,200,ff336699,", "paint",
        "cursor=100,100", "paint",
        "damage=0,0,320,200", "paint",
        "win=10,10,50,50,80ffffff,", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        CHECK_EQ(r.nrep, 4);
        CHECK(r.rep[0].tiles > 0);
        CHECK_EQ(r.rep[0].direct, r.rep[0].tiles);   /* all of it straight from the buffer */
        CHECK_EQ(r.rep[1].direct, 0);                /* the cursor's box: composed */
        CHECK(r.rep[2].direct > 0 && r.rep[2].direct < r.rep[2].tiles);
        CHECK_EQ(r.rep[2].px, CP_W * CP_H);
        CHECK_EQ(r.rep[3].direct, 0);                /* a window over it: no longer full screen */
        static const struct cp_win v[] = {
            { 0, 0, CP_W, CP_H, 0xff336699, false, false },
            { 10, 10, 50, 50, 0x80ffffff, false, false },
        };
        static uint32_t want[CP_W * CP_H];
        ref_paint(want, v, 2);
        ok = same_image(&r, want, arrow_at(100, 100));
        CHECK_EQ(r.image[100 * CP_W + 100], 0x000000);
    }
    cp_done(&r);
    return ok;
}

/* Pixels of colour c in box b of the image. */
static unsigned count_colour(const struct cp_run *r, struct comp_box b, uint32_t c)
{
    unsigned n = 0;
    for (int32_t y = b.y1; y < b.y2; y++)
        for (int32_t x = b.x1; x < b.x2; x++)
            n += r->image[y * CP_W + x] == c;
    return n;
}

bool t_comp_paint_title(void)
{
    /* Window 1 focused at (40, 40) 200x100: frame (38, 16)..(242, 142),
     * its title bar rows 16..40, close box x 218..242. Window 2 over it,
     * not focused: frame (98, 96)..(252, 182), bar rows 96..120. */
    static const char *const cmds[] = {
        "win=40,40,200,100,ff305070,tf", "win=100,120,150,60,ff705030,t", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, 3, &r);
    if (ok) {
        const uint32_t *im = r.image;
        CHECK_EQ(im[18 * CP_W + 200], TITLE_BAR_FOCUSED);      /* bar, past the text */
        CHECK_EQ(im[18 * CP_W + 219], TITLE_BAR_FOCUSED);      /* close box, outside its X */
        CHECK_EQ(im[(16 + 7) * CP_W + 218 + 7], TITLE_TEXT_FOCUSED);   /* the X's corner */
        CHECK_EQ(im[(16 + 12) * CP_W + 218 + 12], TITLE_TEXT_FOCUSED); /* ... its middle */
        CHECK(count_colour(&r, (struct comp_box){ 46, 20, 120, 36 }, TITLE_TEXT_FOCUSED) > 20);
        CHECK_EQ(im[98 * CP_W + 200], TITLE_BAR);              /* window 2's bar, over window 1 */
        CHECK(count_colour(&r, (struct comp_box){ 106, 100, 180, 116 }, TITLE_TEXT) > 20);
        CHECK_EQ(im[60 * CP_W + 38], BORDER_FOCUSED);          /* window 1's left border */
        CHECK_EQ(im[60 * CP_W + 241], BORDER_FOCUSED);         /* its right */
        CHECK_EQ(im[141 * CP_W + 60], BORDER_FOCUSED);         /* its bottom */
        CHECK_EQ(im[150 * CP_W + 99], BORDER);                 /* window 2's left */
        CHECK_EQ(im[15 * CP_W + 100], SPLASH_BG);              /* nothing above the bar */
        CHECK_EQ(im[60 * CP_W + 37], SPLASH_BG);               /* nor left of the border */
        CHECK_EQ(im[40 * CP_W + 40], testscene_rgb(0x305070, 0, 0, false));   /* the surface */
        CHECK_EQ(im[121 * CP_W + 100], testscene_rgb(0x705030, 0, 1, false));
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_cursor(void)
{
    static const char *const cmds[] = {
        "win=0,0,320,200,ff203040,", "cursor=100,80", "cursorsurf=40,30,80c03060,10,5", "paint",
        "cursorsize=20,10", "paint", "cursor=200,150", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        CHECK_EQ(r.nrep, 3);
        CHECK_EQ(r.rep[1].px, 40 * 30);   /* shrunk: where it was, all of it */
        CHECK_EQ(r.rep[2].px, 2 * 20 * 10);
        /* The client's cursor, blended at its hot spot; nothing left behind. */
        static const struct cp_win v[] = {
            { 0, 0, CP_W, CP_H, 0xff203040, false, false },
            { 190, 145, 20, 10, 0x80c03060, false, false },
        };
        static uint32_t want[CP_W * CP_H];
        ref_paint(want, v, 2);
        ok = same_image(&r, want, no_skip);
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_blank(void)
{
    static const char *const cmds[] = {
        "win=10,10,100,100,ff123456,t", "cursor=50,50", "blank", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, 4, &r);
    if (ok) {
        CHECK_EQ(count_colour(&r, (struct comp_box){ 0, 0, CP_W, CP_H }, SPLASH_BG), CP_W * CP_H);
        CHECK_EQ(r.rep[0].layer_px, 0);
    }
    cp_done(&r);
    return ok;
}
