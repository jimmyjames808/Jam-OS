/* The test scene (paint.h): windows of known pixels with no client behind
 * them, painted on the output as the commands say, so the painting can be
 * checked pixel by pixel (utest headless, tools/comp-test.sh on QEMU's
 * framebuffer) and timed (`bench`) before any client can make a window.
 *
 * `compositor [headless size=WxH] testscene <command>...` runs the
 * commands in order, then exits (0, or 2 for a command it can't take,
 * which it names). Only the starter can ask for it (an argument): no
 * client ever reaches this code. The commands:
 *   win=X,Y,W,H,AARRGGBB[,flags]
 *                 a window (numbered 1, 2, ... in order), on top, its
 *                 buffer W x H of testscene.h's pixels in colour RRGGBB:
 *                 xrgb8888 if AA is ff, else argb8888 at alpha AA. Flags:
 *                 t decorations (a title bar "Window <n>", 2-pixel borders),
 *                 f focused, o its opaque region is all of it, u not
 *                 responding, s solid (no pattern)
 *   fullscreen=AARRGGBB
 *                 the same at (0, 0), as big as the output
 *   move=K,X,Y    raise=K    unmap=K    map=K
 *   damage=X,Y,W,H
 *   cursor=X,Y    the cursor (the arrow at first), there;  nocursor
 *   cursorsurf=W,H,AARRGGBB,HX,HY
 *                 a surface of ours (no window; it takes the next number)
 *                 as the cursor, hot spot HX, HY
 *   cursorsize=W,H  that surface shrinks, as a smaller buffer's commit would
 *   cursorarrow   the arrow again
 *   blank         unblank
 *   paint         paint the damage now; a line in the log and a struct
 *                 testscene_report on SR_USER + TESTSCENE_REPORT_ROLE (if given)
 *   hold=MS       wait (a screenshot's time), saying so in the log
 *   bench         time whole frames (below): `compositor: bench:` lines
 * Every command is echoed to the log as it runs, so a test that only sees
 * the log (comp-test.sh's checker) knows the scene too. */
#include <fun.h>
#include <jwl/wayland.h>
#include "paint.h"
#include "testscene.h"

#define WINS_MAX      8
#define BENCH_SAMPLES 31
#define BORDER_W      2

/* A window with no client: its surface, buffer and pool are ours. */
struct twin {
    struct comp_surface s;
    struct comp_buffer  b;
    struct comp_pool    p;
    char title[24];
};

static struct twin wins[WINS_MAX];
/* The client every test surface belongs to, for the seat (which looks at a
 * window's client): it has no connection, so nothing is ever sent to it
 * and the seat never gives it the focus. */
static struct comp_client scene_client;
static unsigned nwins;
static handle_t report = HANDLE_INVALID;
static uint32_t npaints;

/* ---- parsing ---------------------------------------------------------------------------- */

/* Up to n comma-separated numbers (decimal, with an optional '-'; hex for
 * the field numbered `hex`, -1 for none) from s; the rest (after a comma)
 * in *rest, or NULL. How many were read; -1 if malformed. */
static int numbers(const char *s, int64_t *v, int n, int hex, const char **rest)
{
    int k = 0;
    *rest = NULL;
    while (k < n && *s) {
        bool neg = *s == '-' && k != hex;
        s += neg;
        int64_t x = 0;
        const char *start = s;
        for (; *s && *s != ','; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0'
                    : k == hex && (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f' ? (*s | 0x20) - 'a' + 10
                    : -1;
            if (d < 0 || x > (1ll << 40))
                return -1;
            x = x * (k == hex ? 16 : 10) + d;
        }
        if (s == start)
            return -1;
        v[k++] = neg ? -x : x;
        if (*s == ',')
            s++;
    }
    if (*s)
        *rest = s;
    return k;
}

/* "key=..." with key: what follows the '=', else NULL. */
static const char *after(const char *arg, const char *key)
{
    size_t k = strlen(key);
    return !strncmp(arg, key, k) && arg[k] == '=' ? arg + k + 1 : NULL;
}

/* ---- windows ------------------------------------------------------------------------------ */

/* The buffer's pixels (testscene.h). */
static void fill_pixels(uint32_t *px, int32_t w, int32_t h, uint32_t argb, bool solid)
{
    uint32_t a = argb >> 24;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint32_t rgb = testscene_rgb(argb, x, y, solid);
            px[(uint64_t)y * (uint64_t)w + (uint64_t)x] =
                a == 0xff ? rgb | TESTSCENE_GARBAGE : testscene_pm(rgb, a);
        }
}

/* w x h pixels of testscene.h's picture in memory of their own, or NULL. */
static uint32_t *pixels_of(int32_t w, int32_t h, uint32_t argb, bool solid)
{
    uint32_t *px = big_alloc((uint64_t)w * (uint64_t)h * 4);
    if (px)
        fill_pixels(px, w, h, argb, solid);
    return px;
}

/* A surface of ours, w x h, over pixels px (stride pixels a row), with no
 * window yet. */
static struct twin *add_surface(int32_t w, int32_t h, const uint32_t *px, uint32_t stride,
                                bool argb)
{
    if (nwins == WINS_MAX)
        return NULL;
    struct twin *t = &wins[nwins++];
    memset(t, 0, sizeof(*t));
    t->p = (struct comp_pool){ .addr = (uint64_t)(uintptr_t)px, .refs = 1 };
    t->b = (struct comp_buffer){ .pool = &t->p, .refs = 1, .busy = 1, .width = w, .height = h,
                                 .stride = stride * 4,
                                 .format = argb ? JWL_WL_SHM_FORMAT_ARGB8888
                                                : JWL_WL_SHM_FORMAT_XRGB8888 };
    t->s.buffer = &t->b;
    t->s.width = w;
    t->s.height = h;
    t->s.input_all = true;
    t->s.client = &scene_client;
    region_init(&t->s.opaque, NULL);
    region_init(&t->s.input, NULL);
    return t;
}

/* A window of ours over pixels px (stride pixels a row), on top, mapped. */
static struct twin *add_window(struct comp_box at, const uint32_t *px, uint32_t stride, bool argb)
{
    struct twin *t = add_surface(at.x2 - at.x1, at.y2 - at.y1, px, stride, argb);
    if (!t)
        return NULL;
    struct comp_window *w;
    if (window_create(&t->s, at.x1, at.y1, &w) != OK) {
        nwins--;
        return NULL;
    }
    snprintf(t->title, sizeof(t->title), "Window %u", nwins);
    w->title = t->title;
    window_map(w, true);
    return t;
}

/* Decorations and states from the flags letters. */
static void apply_flags(struct twin *t, const char *flags)
{
    struct comp_window *w = t->s.window;
    window_damage(w);
    for (const char *f = flags; f && *f; f++) {
        if (*f == 't') {
            w->deco_top = COMP_TITLE_H;
            w->deco_left = w->deco_right = w->deco_bottom = BORDER_W;
        }
        /* Focus as the seat marks it (it gives none to a client with no
         * connection, so it leaves these windows to us). */
        w->flags |= *f == 'f' ? COMP_WIN_FOCUSED : *f == 'u' ? COMP_WIN_UNRESPONSIVE : 0;
        if (*f == 'o')
            (void)region_add(&t->s.opaque, box_make(0, 0, t->s.width, t->s.height));
    }
    window_damage(w);
}

/* win=X,Y,W,H,AARRGGBB[,flags] */
static bool cmd_win(const char *s)
{
    int64_t v[5];
    const char *flags;
    if (numbers(s, v, 5, 4, &flags) != 5 || v[2] < 1 || v[3] < 1 || v[2] > 4096 || v[3] > 4096 ||
        v[0] < -8192 || v[0] > 8192 || v[1] < -8192 || v[1] > 8192 || v[4] > 0xffffffffll)
        return false;
    uint32_t *px = pixels_of((int32_t)v[2], (int32_t)v[3], (uint32_t)v[4],
                             flags && strchr(flags, 's'));
    if (!px)
        return false;
    struct comp_box at = box_make((int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]);
    struct twin *t = add_window(at, px, (uint32_t)v[2], (uint32_t)v[4] >> 24 != 0xff);
    if (!t)
        return false;
    apply_flags(t, flags);
    return true;
}

/* fullscreen=AARRGGBB: a window as big as the output, at (0, 0). */
static bool cmd_fullscreen(const char *s)
{
    int64_t v[1];
    const char *rest;
    if (numbers(s, v, 1, 0, &rest) != 1 || rest || v[0] > 0xffffffffll)
        return false;
    char win[64];
    snprintf(win, sizeof(win), "0,0,%d,%d,%08x", scene.width, scene.height, (uint32_t)v[0]);
    return cmd_win(win);
}

static struct twin *cursor_twin;   /* the cursor's surface (cursorsurf), or NULL */

/* The pointer to (x, y), as the seat moves it. */
static void move_cursor(int32_t x, int32_t y)
{
    int32_t ox = cursor.x, oy = cursor.y;
    cursor.x = x;
    cursor.y = y;
    cursor_moved(ox, oy);
}

/* The cursor's picture, as the seat sets it: s with its hot spot at
 * (hx, hy), or the arrow (s NULL). */
static void set_cursor_picture(struct twin *t, int32_t hx, int32_t hy)
{
    cursor_twin = t;
    cursor.surface = t ? &t->s : NULL;
    cursor.hot_x = hx;
    cursor.hot_y = hy;
    cursor.hidden = false;
    cursor_moved(cursor.x, cursor.y);
}

/* cursorsurf=W,H,AARRGGBB,HX,HY: a surface of ours, no window, as the
 * cursor with hot spot HX, HY. cursorsize=W,H: it shrinks to W x H, as a
 * commit of a smaller buffer would make it. cursorarrow: the arrow again. */
static bool cmd_cursor(const char *cmd, const char *s)
{
    int64_t v[5];
    const char *rest;
    if (!strcmp(cmd, "cursorarrow")) {
        set_cursor_picture(NULL, 0, 0);
        return true;
    }
    if (!strcmp(cmd, "cursorsize")) {
        if (!cursor_twin || numbers(s, v, 2, -1, &rest) != 2 || rest || v[0] < 1 || v[1] < 1 ||
            v[0] > cursor_twin->b.width || v[1] > cursor_twin->b.height)
            return false;
        cursor_twin->b.width = cursor_twin->s.width = (int32_t)v[0];
        cursor_twin->b.height = cursor_twin->s.height = (int32_t)v[1];
        cursor_moved(cursor.x, cursor.y);   /* as its commit would */
        return true;
    }
    if (numbers(s, v, 5, 2, &rest) != 5 || rest || v[0] < 1 || v[1] < 1 || v[0] > 256 ||
        v[1] > 256 || v[2] > 0xffffffffll)
        return false;
    uint32_t *px = pixels_of((int32_t)v[0], (int32_t)v[1], (uint32_t)v[2], false);
    struct twin *t = px ? add_surface((int32_t)v[0], (int32_t)v[1], px, (uint32_t)v[0],
                                      (uint32_t)v[2] >> 24 != 0xff)
                        : NULL;
    if (!t)
        return false;
    set_cursor_picture(t, (int32_t)v[3], (int32_t)v[4]);
    return true;
}

/* The window numbered k (1-based), or NULL. */
static struct comp_window *window_no(int64_t k)
{
    return k >= 1 && k <= (int64_t)nwins ? wins[k - 1].s.window : NULL;
}

/* move=K,X,Y raise=K map=K unmap=K */
static bool cmd_window(const char *cmd, const char *s)
{
    int64_t v[3];
    const char *rest;
    int n = numbers(s, v, 3, -1, &rest);
    struct comp_window *w = n >= 1 && !rest ? window_no(v[0]) : NULL;
    if (!w)
        return false;
    if (!strcmp(cmd, "move") && n == 3)
        window_move(w, (int32_t)v[1], (int32_t)v[2]);
    else if (!strcmp(cmd, "raise") && n == 1)
        window_raise(w);
    else if ((!strcmp(cmd, "map") || !strcmp(cmd, "unmap")) && n == 1)
        window_map(w, cmd[0] == 'm');
    else
        return false;
    return true;
}

/* blank, unblank: as compctl.blank does it. */
static void set_blank(bool on)
{
    comp.blanked = on;
    scene_damage((struct comp_box){ 0, 0, scene.width, scene.height });
}

/* ---- painting and reporting ----------------------------------------------------------------- */

static void do_paint(void)
{
    (void)paint_frame();
    npaints++;
    printf("compositor: testscene: paint %u: %lu px, %lu px from windows, %u tiles (%u direct), "
           "%lu us\n", npaints, (unsigned long)paint_last.px, (unsigned long)paint_last.layer_px,
           paint_last.tiles, paint_last.direct, (unsigned long)(paint_last.ns / 1000));
    if (report == HANDLE_INVALID)
        return;
    struct testscene_report r = { .paint = npaints, .tiles = paint_last.tiles,
                                  .direct = paint_last.direct, .px = paint_last.px,
                                  .layer_px = paint_last.layer_px, .ns = paint_last.ns };
    if (jam_channel_write(report, &r, sizeof(r), NULL, 0) != OK)
        printf("compositor: testscene: the report channel took no report\n");
}

/* ---- bench ------------------------------------------------------------------------------------ */

/* Median and worst of n samples (sorted in place). */
static void bench_line(const char *what, uint64_t *ns, unsigned n, uint64_t px)
{
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && ns[j - 1] > ns[j]; j--) {
            uint64_t x = ns[j];
            ns[j] = ns[j - 1];
            ns[j - 1] = x;
        }
    /* Into the RESULTS box too: the PC's comptest boot has no log on a stick. */
    char line[128];
    int len = snprintf(line, sizeof(line),
                       "compositor: bench: %-30s median %5lu us worst %5lu us %7lu px", what,
                       (unsigned long)(ns[n / 2] / 1000), (unsigned long)(ns[n - 1] / 1000),
                       (unsigned long)px);
    (void)jam_debug_report(line, (uint64_t)(len < (int)sizeof(line) ? len : (int)sizeof(line) - 1));
}

/* One kind of frame, BENCH_SAMPLES times: damage() before each paint. */
static void bench_frames(const char *what, void (*damage)(void))
{
    uint64_t ns[BENCH_SAMPLES], px = 0;
    for (unsigned i = 0; i < BENCH_SAMPLES; i++) {
        damage();
        px = paint_frame();
        ns[i] = paint_last.ns;
    }
    bench_line(what, ns, BENCH_SAMPLES, px);
}

static struct comp_box bench_box;   /* what the next bench frame damages */
static void damage_box(void) { scene_damage(bench_box); }
static void damage_pointer(void)
{
    struct comp_box c = cursor_box();
    move_cursor(c.x1 + (c.x1 > scene.width / 2 ? -8 : 8), c.y1 + 3);
}

/* Every test window gone, then the bench's own: a full-screen opaque one,
 * then a decorated argb window over it. */
static bool bench_run(void)
{
    int32_t w = scene.width, h = scene.height;
    set_cursor_picture(NULL, 0, 0);
    while (nwins)
        if (wins[--nwins].s.window)
            window_destroy(wins[nwins].s.window);
    uint32_t *opaque = big_alloc((uint64_t)w * (uint64_t)h * 4);
    uint32_t *argb = big_alloc((uint64_t)w * (uint64_t)h * 4);
    if (!opaque || !argb)
        return false;
    fill_pixels(opaque, w, h, 0xff2a6f97, false);
    fill_pixels(argb, w, h, 0xb0e0a040, false);
    cursor_show(false);
    if (!add_window((struct comp_box){ 0, 0, w, h }, opaque, (uint32_t)w, false))
        return false;
    bench_box = (struct comp_box){ 0, 0, w, h };
    bench_frames("full screen, opaque (direct)", damage_box);
    int32_t ww = w < 1280 ? w * 3 / 4 : 1280, wh = h < 800 ? h * 3 / 4 : 800;
    struct comp_box at = { (w - ww) / 2, (h - wh) / 2, (w + ww) / 2, (h + wh) / 2 };
    struct twin *t = add_window(at, argb, (uint32_t)w, true);
    if (!t)
        return false;
    apply_flags(t, "tf");
    cursor_show(true);
    bench_frames("full screen, argb window on it", damage_box);
    bench_box = window_frame(t->s.window);
    char what[64];
    snprintf(what, sizeof(what), "%dx%d argb over opaque", ww, wh);
    bench_frames(what, damage_box);
    bench_box = (struct comp_box){ 0, h / 2, w, h / 2 + 16 };
    bench_frames("a text line (full width x 16)", damage_box);
    bench_frames("the pointer moved", damage_pointer);
    return true;
}

/* ---- the commands ---------------------------------------------------------------------------- */

static bool run_one(const char *c)
{
    const char *s;
    int64_t v[4];
    const char *rest;
    if ((s = after(c, "win")))
        return cmd_win(s);
    if ((s = after(c, "fullscreen")))
        return cmd_fullscreen(s);
    if ((s = after(c, "cursorsurf")))
        return cmd_cursor("cursorsurf", s);
    if ((s = after(c, "cursorsize")))
        return cmd_cursor("cursorsize", s);
    if (!strcmp(c, "cursorarrow"))
        return cmd_cursor(c, "");
    if ((s = after(c, "damage"))) {
        if (numbers(s, v, 4, -1, &rest) != 4 || rest)
            return false;
        scene_damage(box_make((int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]));
        return true;
    }
    if ((s = after(c, "cursor"))) {
        if (numbers(s, v, 2, -1, &rest) != 2 || rest)
            return false;
        move_cursor((int32_t)v[0], (int32_t)v[1]);
        cursor_show(true);
        return true;
    }
    if ((s = after(c, "hold"))) {
        if (numbers(s, v, 1, -1, &rest) != 1 || rest || v[0] < 0 || v[0] > 600000)
            return false;
        printf("compositor: testscene: holding %ld ms after paint %u\n", (long)v[0], npaints);
        jam_nanosleep(now() + (uint64_t)v[0] * NS_PER_MS);
        return true;
    }
    static const char *const win_cmds[] = { "move", "raise", "map", "unmap" };
    for (unsigned i = 0; i < sizeof(win_cmds) / sizeof(win_cmds[0]); i++)
        if ((s = after(c, win_cmds[i])))
            return cmd_window(win_cmds[i], s);
    if (!strcmp(c, "nocursor") || !strcmp(c, "blank") || !strcmp(c, "unblank")) {
        if (c[0] == 'n')
            cursor_show(false);
        else
            set_blank(c[0] == 'b');
        return true;
    }
    if (!strcmp(c, "paint")) {
        do_paint();
        return true;
    }
    return !strcmp(c, "bench") && bench_run();
}

int testscene_run(int argc, char **argv, int first)
{
    report = startup_handle(SR_USER + TESTSCENE_REPORT_ROLE);
    printf("compositor: testscene: output %dx%d, %s\n", scene.width, scene.height,
           output.screen ? "the screen" : "headless");
    for (int i = first; i < argc; i++) {
        printf("compositor: testscene: %s\n", argv[i]);
        if (!run_one(argv[i])) {
            printf("compositor: testscene: can't do \"%s\"\n", argv[i]);
            return 2;
        }
    }
    printf("compositor: testscene: done\n");
    return 0;
}
