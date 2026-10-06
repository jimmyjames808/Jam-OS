/* utest: the compositor painting tests' runner and reference painter
 * (comppaint.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "comppaint.h"
#include "comptest.h"
#include "utest.h"

#define CP_WAIT (30 * NS_PER_S)

/* ---- running the test scene ---------------------------------------------------------- */

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

bool cp_run(const char *const *cmds, unsigned n, struct cp_run *r)
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
    const char *argv[24] = { "bin/compositor", "headless", size, "threads=3", "layout=floating",
                             "testscene" };
    CHECK(n <= 24 - 6);
    for (unsigned i = 0; i < n; i++)
        argv[6 + i] = cmds[i];
    struct spawn_handle x[] = { { SR_USER + 0, svc_theirs }, { SR_USER + 1, image },
                                { SR_USER + TESTSCENE_REPORT_ROLE, rep_theirs } };
    struct spawn_args a = { .path = "bin/compositor", .argc = (int)(6 + n), .argv = argv,
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

void cp_done(struct cp_run *r)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)r->image,
                         r->image_size);
}

void cp_win_cmd(char *buf, size_t n, const struct cp_win *w)
{
    snprintf(buf, n, "win=%d,%d,%d,%d,%08x,%s%s%s%s", w->x, w->y, w->w, w->h, w->argb,
             w->solid ? "s" : "", w->opaque_region ? "o" : "",
             w->look == 't' ? "t" : w->look == 'g' ? "g" : "", w->focused ? "f" : "");
}

/* ---- blends ----------------------------------------------------------------------------- */

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

/* below * (255 - cov) + edge * ring + p * (cov - ring), / 255 (look.h's corners). */
static uint32_t corner_mix(uint32_t below, uint32_t edge, uint32_t p, uint32_t cov, uint32_t ring)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8)
        out |= div255((below >> sh & 0xff) * (255 - cov) + (edge >> sh & 0xff) * ring +
                      (p >> sh & 0xff) * (cov - ring)) << sh;
    return out;
}

/* ---- the wallpaper ------------------------------------------------------------------------ */

/* The 8x8 ordered-dither matrix, made the textbook way: from [[0, 2], [3, 1]],
 * each size's entry four times the half size's (at x and y modulo the half
 * size) plus the 2x2's for the quadrant: 16 * B2[bit 0] + 4 * B2[bit 1] +
 * B2[bit 2] of x and y. */
static uint32_t bayer(int32_t x, int32_t y)
{
    static const uint32_t b2[2][2] = { { 0, 2 }, { 3, 1 } };
    uint32_t v = 0;
    for (int s = 0; s <= 2; s++)
        v = v * 4 + b2[(y >> s) & 1][(x >> s) & 1];
    return v;
}

uint32_t ref_wallpaper(int32_t x, int32_t y)
{
    return ref_wallpaper_of(x, y, CP_W, CP_H);
}

uint32_t ref_wallpaper_of(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int64_t side = w > h ? w : h, acc[3];
    for (int c = 0; c < 3; c++)
        acc[c] = (int64_t)(LOOK_WALL_BASE >> (16 - 8 * c) & 0xff) * 256;
    for (int g = 0; g < LOOK_GLOWS; g++) {
        int64_t r = side * look_glows[g].r / 1000, r2 = r * r;
        int64_t dx = x - (int64_t)w * look_glows[g].cx / 1000;
        int64_t dy = y - (int64_t)h * look_glows[g].cy / 1000, d2 = dx * dx + dy * dy;
        if (d2 >= r2)
            continue;
        int64_t q = 65536 - ((d2 * ((1ll << 48) / r2)) >> 32), f = (q * q) >> 16;
        for (int c = 0; c < 3; c++) {
            int64_t delta = (int64_t)(look_glows[g].rgb >> (16 - 8 * c) & 0xff) -
                            (int64_t)(LOOK_WALL_BASE >> (16 - 8 * c) & 0xff);
            acc[c] += (f * delta) >> 8;
        }
    }
    uint32_t px = 0;
    for (int c = 0; c < 3; c++) {
        int64_t v = acc[c] < 0 ? 0 : acc[c] > 255 * 256 ? 255 * 256 : acc[c];
        uint64_t o = ((uint64_t)v + bayer(x & 7, y & 7) * 4 + 2) >> 8;
        px |= (uint32_t)(o > 255 ? 255 : o) << (16 - 8 * c);
    }
    return px;
}

/* ---- shapes: corners and shadows --------------------------------------------------------- */

/* How much of corner pixel (i, j) of a radius r corner is within radius rr
 * of the circle's centre (r, r): 256 points, 0..255. */
static uint32_t ref_cover(int32_t i, int32_t j, int32_t r, int32_t rr)
{
    uint32_t n = 0;
    for (int32_t t = 0; t < 16; t++)
        for (int32_t s = 0; s < 16; s++) {
            int64_t dx = 32 * (int64_t)(r - i) - (2 * s + 1);
            int64_t dy = 32 * (int64_t)(r - j) - (2 * t + 1);
            n += dx * dx + dy * dy <= 1024 * (int64_t)rr * rr;
        }
    return (n * 255 + 128) / 256;
}

struct ref_shadow {
    int32_t box, reach, dy;
    uint32_t alpha;
    uint32_t edge[2 * LOOK_SHADOW_REACH_MAX];   /* T(d), d in [-reach, reach) */
};

/* Ways three whole numbers in [0, box) add up to k: the blur's kernel. */
static uint64_t ways(int32_t k, int32_t box)
{
    uint64_t n = 0;
    for (int32_t a = 0; a < box; a++)
        for (int32_t b = 0; b < box; b++)
            n += k - a - b >= 0 && k - a - b < box;
    return n;
}

static void ref_shadow_make(struct ref_shadow *sh, int32_t box, int32_t reach, int32_t dy,
                            uint32_t alpha)
{
    *sh = (struct ref_shadow){ box, reach, dy, alpha, { 0 } };
    uint64_t total = (uint64_t)box * box * box, half = (uint64_t)(3 * box - 2) / 2;
    for (int32_t d = -reach; d < reach; d++) {
        uint64_t sum = 0;
        for (int64_t k = 0; k <= d + (int64_t)half; k++)
            sum += ways((int32_t)k, box);
        sh->edge[d + reach] = (uint32_t)((sum * 65536 + total / 2) / total);
    }
}

static uint64_t ref_edge(const struct ref_shadow *sh, int32_t d)
{
    return d < -sh->reach ? 0 : d >= sh->reach ? 65536 : sh->edge[d + sh->reach];
}

/* The shadow's alpha at (x, y) for frame f. */
static uint32_t shadow_alpha(const struct ref_shadow *sh, struct comp_box f, int32_t x, int32_t y)
{
    uint64_t fy = ref_edge(sh, y - f.y1 - sh->dy) - ref_edge(sh, y - f.y2 - sh->dy);
    uint64_t fx = ref_edge(sh, x - f.x1) - ref_edge(sh, x - f.x2);
    return (uint32_t)((sh->alpha * fy * fx + (1ull << 31)) >> 32);
}

static const struct ref_shadow *ref_shadow(bool focused)
{
    static struct ref_shadow sh[2];
    static bool made;
    if (!made) {
        ref_shadow_make(&sh[0], LOOK_SHADOW_BOX, LOOK_SHADOW_REACH, LOOK_SHADOW_DY,
                        LOOK_SHADOW_ALPHA);
        ref_shadow_make(&sh[1], LOOK_SHADOW_F_BOX, LOOK_SHADOW_F_REACH, LOOK_SHADOW_F_DY,
                        LOOK_SHADOW_F_ALPHA);
        made = true;
    }
    return &sh[focused];
}

/* ---- the reference painter ------------------------------------------------------------------ */

struct comp_box ref_frame(const struct cp_win *w)
{
    int32_t top = w->look == 't' ? COMP_TITLE_H : w->look == 'g' ? DECO_BORDER : 0;
    int32_t side = w->look == 't' ? DECO_OUTLINE : w->look == 'g' ? DECO_BORDER : 0;
    return (struct comp_box){ w->x - side, w->y - top, w->x + w->w + side, w->y + w->h + side };
}

struct comp_box ref_extent(const struct cp_win *w)
{
    struct comp_box f = ref_frame(w);
    if (w->look != 't')
        return f;
    return (struct comp_box){ f.x1 - LOOK_SHADOW_F_REACH,
                              f.y1 - LOOK_SHADOW_F_REACH + LOOK_SHADOW_F_DY,
                              f.x2 + LOOK_SHADOW_F_REACH,
                              f.y2 + LOOK_SHADOW_F_REACH + LOOK_SHADOW_F_DY };
}

/* w's corner radius and ring (0: square). */
static int32_t radius_of(const struct cp_win *w, int32_t *ring)
{
    struct comp_box f = ref_frame(w);
    int32_t r = w->look == 't' ? LOOK_RADIUS : w->look == 'g' ? LOOK_TILE_RADIUS : 0;
    *ring = w->look == 't' ? DECO_OUTLINE : DECO_BORDER;
    return f.x2 - f.x1 < 2 * r || f.y2 - f.y1 < 2 * r ? 0 : r;
}

/* Is (x, y) in one of frame f's r by r corner squares? Its (i, j) in the
 * top-left one's terms. */
static bool in_corner(struct comp_box f, int32_t r, int32_t x, int32_t y, int32_t *i, int32_t *j)
{
    bool left = x < f.x1 + r, right = x >= f.x2 - r, top = y < f.y1 + r, bottom = y >= f.y2 - r;
    if (!r || !(left || right) || !(top || bottom))
        return false;
    *i = left ? x - f.x1 : f.x2 - 1 - x;
    *j = top ? y - f.y1 : f.y2 - 1 - y;
    return true;
}

static void ref_shadow_paint(uint32_t *img, const struct cp_win *w)
{
    struct comp_box f = ref_frame(w);
    int32_t ring, r = radius_of(w, &ring), i, j;
    const struct ref_shadow *sh = ref_shadow(w->focused);
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 0; x < CP_W; x++) {
            bool in_frame = box_contains(f, x, y);
            if (in_frame && !in_corner(f, r, x, y, &i, &j))
                continue;   /* the window covers it */
            uint32_t a = shadow_alpha(sh, f, x, y), p = img[y * CP_W + x], out = 0;
            for (int s = 0; s < 24; s += 8)
                out |= div255((p >> s & 0xff) * (255 - a)) << s;
            img[y * CP_W + x] = out;
        }
}

/* w's own pixel at (x, y) in its frame over what img has; *unknown set if
 * the reference can't say (its title's text and circles). */
static uint32_t ref_window_px(const struct cp_win *w, const uint32_t *img, int32_t x, int32_t y,
                              bool *unknown)
{
    struct comp_box f = ref_frame(w);
    *unknown = false;
    if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
        uint32_t a = w->argb >> 24, rgb = testscene_rgb(w->argb, x - w->x, y - w->y, w->solid);
        if (a == 0xff)
            return rgb;
        if (w->opaque_region)
            return testscene_pm(rgb, a) & 0xffffff;   /* taken as opaque: copied */
        return ref_over(img[y * CP_W + x], testscene_pm(rgb, a));
    }
    if (w->look == 'g')
        return w->focused ? LOOK_TILE_FOCUSED : LOOK_TILE;
    if (x == f.x1 || x == f.x2 - 1 || y == f.y1 || y == f.y2 - 1)
        return w->focused ? LOOK_OUTLINE_FOCUSED : LOOK_OUTLINE;
    int32_t ring, r = radius_of(w, &ring), i, j;
    *unknown = !in_corner(f, r, x, y, &i, &j);   /* text and circles are past the corners */
    return w->focused ? LOOK_BAR_FOCUSED : LOOK_BAR;
}

static void ref_window(uint32_t *img, uint8_t *unknown, const struct cp_win *w)
{
    static uint32_t below[CP_W * CP_H];
    struct comp_box f = box_intersect(ref_frame(w), (struct comp_box){ 0, 0, CP_W, CP_H });
    int32_t ring, r = radius_of(w, &ring), i, j;
    uint32_t edge = w->look == 'g' ? (w->focused ? LOOK_TILE_FOCUSED : LOOK_TILE)
                                   : (w->focused ? LOOK_OUTLINE_FOCUSED : LOOK_OUTLINE);
    memcpy(below, img, sizeof(below));
    for (int32_t y = f.y1; y < f.y2; y++)
        for (int32_t x = f.x1; x < f.x2; x++) {
            bool unk;
            uint32_t p = ref_window_px(w, img, x, y, &unk), k = (uint32_t)(y * CP_W + x);
            bool blended = (w->argb >> 24) != 0xff && !w->opaque_region &&
                           x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
            if (in_corner(ref_frame(w), r, x, y, &i, &j)) {
                uint32_t cov = ref_cover(i, j, r, r), inner = ref_cover(i, j, r, r - ring);
                p = corner_mix(below[k], edge, p, cov, cov - inner);
                unk = unk || (cov < 255 && unknown[k]);
            }
            img[k] = p;
            unknown[k] = unk || (blended && unknown[k]);
        }
}

void ref_paint(uint32_t *img, uint8_t *unknown, const struct cp_win *v, unsigned n)
{
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 0; x < CP_W; x++)
            img[y * CP_W + x] = ref_wallpaper(x, y);
    memset(unknown, 0, CP_W * CP_H);
    for (unsigned k = 0; k < n; k++) {
        if (v[k].look == 't')
            ref_shadow_paint(img, &v[k]);
        ref_window(img, unknown, &v[k]);
    }
}

/* ---- comparing ------------------------------------------------------------------------------- */

bool cp_same_image(const struct cp_run *r, const uint32_t *want, const uint8_t *unknown,
                   struct comp_box skip)
{
    unsigned bad = 0;
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 0; x < CP_W; x++) {
            if (box_contains(skip, x, y) || (unknown && unknown[y * CP_W + x]))
                continue;
            uint32_t got = r->image[y * CP_W + x], w = want[y * CP_W + x];
            if (got != w && bad++ < 4)
                printf("utest: %s: pixel (%d, %d) is %06x, want %06x\n", utest_cur, x, y, got, w);
        }
    if (bad)
        FAIL("%u pixels wrong", bad);
    return true;
}

bool cp_run_windows(const struct cp_win *v, unsigned n, const char *const *more, unsigned nmore,
                    struct comp_box skip, struct cp_run *r)
{
    static char cmd[8][64];
    static uint32_t want[CP_W * CP_H];
    static uint8_t unknown[CP_W * CP_H];
    const char *cmds[16];
    CHECK(n <= 8 && n + nmore <= 16);
    for (unsigned i = 0; i < n; i++) {
        cp_win_cmd(cmd[i], sizeof(cmd[i]), &v[i]);
        cmds[i] = cmd[i];
    }
    for (unsigned i = 0; i < nmore; i++)
        cmds[n + i] = more[i];
    if (!cp_run(cmds, n + nmore, r))
        return false;
    ref_paint(want, unknown, v, n);
    return cp_same_image(r, want, unknown, skip);
}

unsigned cp_count_colour(const struct cp_run *r, struct comp_box b, uint32_t c)
{
    unsigned n = 0;
    for (int32_t y = b.y1; y < b.y2; y++)
        for (int32_t x = b.x1; x < b.x2; x++)
            n += r->image[y * CP_W + x] == c;
    return n;
}
