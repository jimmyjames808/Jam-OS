/* fractal: the view and its progressive render (fractal.h): the tiles,
 * the passes, the colours, and reusing what was computed. */
#include "fractal.h"

#define LV_NONE   32             /* a tile with nothing computed */
#define PAL       1024

/* The picture: in the header (PW, PH, NT, nu, aa_used), and here. */
int PW, PH;
static int TW, TH;               /* tiles across and down */
int NT;
float *nu;
static float *nu2;               /* the other buffer (reproject, shift) */
static uint8_t *tlev;            /* per tile: the finest level done (16..1; 0: + anti-aliased) */
static uint8_t *tapprox;         /* per tile: holds a stretched old picture (not blocks) */
static uint8_t *tdirty;          /* per tile: to colour again */
static uint32_t *order;          /* the tiles, middle first */
static uint32_t *aa_idx, *aa_idx2;   /* per pixel: 1 + its block of extra samples, or 0 */
static float *aa_pool;
static uint32_t aa_cap;          /* blocks in aa_pool */
uint32_t aa_used;
static bool aa_full;

struct view view;
struct kview kv;
struct ref ref;
uint64_t iters_by[FUN_MAX_THREADS];

/* ---- colours ------------------------------------------------------------------------------ */

const char *const pal_names[NPALS] = { "classic", "twilight", "fire", "ocean", "electric",
                                       "forest", "rainbow", "mono" };
int pal_kind, dens_i = 1;
const float dens_v[4] = { 48, 96, 192, 384 };
double pal_shift;
static uint32_t pal[PAL], inside_c;
float nu_min, nu_min_shown;

void make_palette(int kind)
{
    static const struct stop { double at; uint32_t c; } stops[][8] = {
        { { 0.0, 0x000764 }, { 0.16, 0x206bcb }, { 0.42, 0xedffff }, { 0.6425, 0xffaa00 },
          { 0.8575, 0x000200 }, { 1.0, 0x000764 } },
        { { 0.0, 0x0d0221 }, { 0.22, 0x3d1a78 }, { 0.45, 0xc23b8c }, { 0.66, 0xf6a55c },
          { 0.82, 0xfff0c8 }, { 1.0, 0x0d0221 } },
        { { 0.0, 0x140000 }, { 0.25, 0xc81e00 }, { 0.5, 0xffa000 }, { 0.7, 0xffff78 },
          { 0.85, 0xff7814 }, { 1.0, 0x140000 } },
        { { 0.0, 0x000a28 }, { 0.3, 0x0050a0 }, { 0.55, 0x28c8dc }, { 0.75, 0xdcffff },
          { 0.9, 0x1478a0 }, { 1.0, 0x000a28 } },
        { { 0.0, 0x000000 }, { 0.2, 0x0b1a6b }, { 0.42, 0x1ad1ff }, { 0.55, 0xffffff },
          { 0.7, 0xff2fb5 }, { 0.88, 0x3a0050 }, { 1.0, 0x000000 } },
        { { 0.0, 0x03140c }, { 0.3, 0x0f5132 }, { 0.55, 0x46a36b }, { 0.75, 0xe9d985 },
          { 0.9, 0x7a4b1e }, { 1.0, 0x03140c } },
        { { 0, 0 } },
        { { 0.0, 0x0a0a14 }, { 0.5, 0xf0f0f0 }, { 1.0, 0x0a0a14 } },
    };
    for (int i = 0; i < PAL; i++) {
        double t = (double)i / PAL;
        if (kind == 6) {   /* rainbow: three sines a third apart */
            double a = t * 6.283185307179586;
            pal[i] = rgb((uint32_t)(127.5 + 127 * sind(a)),
                         (uint32_t)(127.5 + 127 * sind(a + 2.0944)),
                         (uint32_t)(127.5 + 127 * sind(a + 4.1888)));
            continue;
        }
        const struct stop *s = stops[kind];
        int k = 0;
        while (t > s[k + 1].at)
            k++;
        double f = (t - s[k].at) / (s[k + 1].at - s[k].at);
        f = f * f * (3 - 2 * f);
        pal[i] = mixc(s[k].c, s[k + 1].c, (uint32_t)(f * 256));
    }
    inside_c = 0x000000;
}

static inline uint32_t colour_of(float n)
{
    if (n < 0)
        return inside_c;
    float t = n - nu_min_shown;
    if (t < 0)
        t = 0;
    int idx = (int)(sqrtf_(t + 1.0f) * dens_v[dens_i] + (float)pal_shift);
    return pal[idx & (PAL - 1)];
}

/* The tiles to colour this frame (all, or the dirty ones). */
static uint32_t *clist, clen;

static void colour_item(uint32_t i, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    uint32_t t = clist[i];
    int x0 = (int)(t % (uint32_t)TW) * TS, y0 = (int)(t / (uint32_t)TW) * TS;
    int x1 = x0 + TS < PW ? x0 + TS : PW, y1 = y0 + TS < PH ? y0 + TS : PH;
    int n = view.ss * view.ss;
    for (int y = y0; y < y1; y++) {
        uint32_t *out = scr.s.px + (uint64_t)y * scr.s.stride;
        const float *src = nu + (uint64_t)y * PW;
        const uint32_t *aai = aa_idx + (uint64_t)y * PW;
        for (int x = x0; x < x1; x++) {
            uint32_t a = aai[x];
            if (!a) {
                out[x] = colour_of(src[x]);
                continue;
            }
            /* Anti-aliased: the samples' average (of squares: closer to how
             * light adds up than averaging the 0..255 values). */
            const float *s = aa_pool + (uint64_t)(a - 1) * (uint32_t)n;
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < n; k++) {
                uint32_t c = colour_of(s[k]);
                r += (c >> 16 & 0xff) * (c >> 16 & 0xff);
                g += (c >> 8 & 0xff) * (c >> 8 & 0xff);
                b += (c & 0xff) * (c & 0xff);
            }
            out[x] = rgb((uint32_t)sqrtf_((float)r / n), (uint32_t)sqrtf_((float)g / n),
                         (uint32_t)sqrtf_((float)b / n));
        }
    }
}

void colour_tiles(bool all)
{
    clen = 0;
    for (int t = 0; t < NT; t++)
        if (all || tdirty[t]) {
            clist[clen++] = (uint32_t)t;
            tdirty[t] = 0;
        }
    if (clen)
        pool_run(colour_item, NULL, clen);
}

void dirty_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int tx0 = x < 0 ? 0 : x / TS, ty0 = y < 0 ? 0 : y / TS;
    int tx1 = (x + w - 1) / TS, ty1 = (y + h - 1) / TS;
    for (int ty = ty0; ty <= ty1 && ty < TH; ty++)
        for (int tx = tx0; tx <= tx1 && tx < TW; tx++)
            tdirty[ty * TW + tx] = 1;
}

/* ---- the view ----------------------------------------------------------------------------- */

static double pixel_size(void) { return (view.julia ? 3.2 : 2.6) / PH / view.zoom; }

static int auto_iterations(void)
{
    if (view.maxit)
        return view.maxit;
    double z = view.zoom > 1 ? view.zoom : 1;
    int n = view.julia ? 400 + (int)(60 * log2d(z)) : 200 + (int)(90 * log2d(z));
    return n > 50000 ? 50000 : n;
}

bool deep(void) { return view.zoom > DEEP_ZOOM; }

/* The kernels' view of `view` (and the reference orbit, deep). */
void apply_view(void)
{
    kv.julia = view.julia;
    kv.cx = view.cx;
    kv.cy = view.cy;
    kv.px = pixel_size();
    kv.jr = view.jr;
    kv.ji = view.ji;
    kv.maxit = auto_iterations();
    kv.mode = !deep() ? M_DOUBLE : view.julia ? M_DD : M_PERTURB;
    if (kv.mode == M_PERTURB && !ref_build(&ref, &kv))
        kv.mode = M_DD;
}

/* ---- the passes --------------------------------------------------------------------------- */

int pass_target;          /* the level being computed: 16..1, 0 anti-aliasing, -1 done */
static uint32_t *plist, plen, pnext, pbase;
uint64_t view_t0, view_ns, view_iters;
static double tile_ns;           /* measured: ns a tile takes one thread, this pass */
static bool serial;              /* the self-test: one thread does it all */

static void build_list(void)
{
    plen = 0;
    for (int i = 0; i < NT; i++) {
        uint32_t t = order[i];
        if (pass_target > 0 ? tlev[t] > pass_target : tlev[t] == 1)
            plist[plen++] = t;
    }
    pnext = 0;
    tile_ns = 0;
}

static void restart_passes(void)
{
    pass_target = 16;
    view_t0 = now();
    view_ns = 0;
    view_iters = 0;
    build_list();
}

/* A level-L pass over tile t (x0, y0; w x h pixels): one pixel in L x L
 * that no earlier pass computed, drawn as an L x L block until a finer pass
 * (or as the one pixel, over a stretched old picture). The iterations. */
static uint64_t tile_level(uint32_t t, int x0, int y0, int w, int h, int L)
{
    double ox[TS * TS], oy[TS * TS];
    float out[TS * TS];
    uint64_t it = 0;
    int cur = tlev[t], n = 0;
    uint8_t xs[TS * TS], ys[TS * TS];
    for (int y = 0; y < h; y += L)
        for (int x = 0; x < w; x += L) {
            if (cur <= 16 && x % cur == 0 && y % cur == 0)
                continue;   /* done by an earlier pass */
            xs[n] = (uint8_t)x;
            ys[n] = (uint8_t)y;
            ox[n] = x0 + x + 0.5 - PW / 2.0;
            oy[n] = y0 + y + 0.5 - PH / 2.0;
            n++;
        }
    if (n)
        eval_points(&kv, &ref, n, ox, oy, out, &it);
    bool apx = tapprox[t];
    for (int k = 0; k < n; k++) {
        int x = xs[k], y = ys[k];
        if (apx || L == 1) {
            nu[(uint64_t)(y0 + y) * PW + x0 + x] = out[k];
            continue;
        }
        for (int yy = y; yy < y + L && yy < h; yy++) {   /* a block, until finer passes */
            float *row = nu + (uint64_t)(y0 + yy) * PW + x0;
            for (int xx = x; xx < x + L && xx < w; xx++)
                row[xx] = out[k];
        }
    }
    tlev[t] = (uint8_t)L;
    if (L == 1)
        tapprox[t] = 0;
    return it;
}

/* Pixel (px, py) is on an edge: in the set and a neighbour not (or the
 * other way round), or a big step in colour to a neighbour. */
static bool is_edge(int px, int py)
{
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    float step = dens_v[dens_i];
    float c = nu[(uint64_t)py * PW + px];
    for (int k = 0; k < 4; k++) {
        int qx = px + d[k][0], qy = py + d[k][1];
        if (qx < 0 || qy < 0 || qx >= PW || qy >= PH)
            continue;
        float o = nu[(uint64_t)qy * PW + qx];
        if ((c < 0) != (o < 0))
            return true;
        if (c >= 0) {
            float a = sqrtf_((c > nu_min ? c - nu_min : 0) + 1.0f);
            float b = sqrtf_((o > nu_min ? o - nu_min : 0) + 1.0f);
            if ((a - b) * step > 10 || (b - a) * step > 10)
                return true;
        }
    }
    return false;
}

/* Pixel (px, py)'s ss x ss sample points, as offsets from the centre in
 * pixels, into ox and oy. */
static void aa_offsets(int px, int py, int ss, double *ox, double *oy)
{
    for (int sy = 0; sy < ss; sy++)
        for (int sx = 0; sx < ss; sx++) {
            ox[sy * ss + sx] = px + (sx + 0.5) / ss - PW / 2.0;
            oy[sy * ss + sx] = py + (sy + 0.5) / ss - PH / 2.0;
        }
}

/* The anti-aliasing pass over tile t: ss x ss samples for each pixel on
 * an edge (until aa_pool is full). The iterations. */
static uint64_t tile_aa(uint32_t t, int x0, int y0, int w, int h)
{
    double ox[TS * TS], oy[TS * TS];
    uint64_t it = 0;
    int ss = view.ss, n = ss * ss;
    for (int y = 0; y < h && !aa_full; y++)
        for (int x = 0; x < w; x++) {
            int px = x0 + x, py = y0 + y;
            if (!is_edge(px, py))
                continue;
            uint32_t blk = __atomic_fetch_add(&aa_used, 1, __ATOMIC_RELAXED);
            if (blk >= aa_cap) {
                aa_full = true;
                break;
            }
            aa_offsets(px, py, ss, ox, oy);
            eval_points(&kv, &ref, n, ox, oy, aa_pool + (uint64_t)blk * (uint32_t)n, &it);
            aa_idx[(uint64_t)py * PW + px] = blk + 1;
        }
    tlev[t] = 0;
    return it;
}

static void tile_item(uint32_t i, uint32_t me, void *arg)
{
    (void)arg;
    uint32_t t = plist[pbase + i];
    int x0 = (int)(t % (uint32_t)TW) * TS, y0 = (int)(t / (uint32_t)TW) * TS;
    int w = PW - x0 < TS ? PW - x0 : TS, h = PH - y0 < TS ? PH - y0 : TS;
    uint64_t it = pass_target > 0 ? tile_level(t, x0, y0, w, h, pass_target)
                                   : tile_aa(t, x0, y0, w, h);
    tdirty[t] = 1;
    iters_by[me] += it;
}

/* The smallest count in view: at every tile's corner (all computed by the
 * first pass). */
static void find_min(void)
{
    float mn = 1e30f;
    for (int t = 0; t < NT; t++) {
        float c = nu[(uint64_t)(t / TW) * TS * PW + (uint64_t)(t % TW) * TS];
        if (c >= 0 && c < mn)
            mn = c;
    }
    if (mn < 1e30f)
        nu_min = mn;
}

/* Work on the passes until `until` (ns; at least one slice) or until
 * everything is done. */
void work(uint64_t until)
{
    uint32_t threads = serial ? 1 : pool_threads();
    bool did = false;
    while (pass_target >= 0) {
        if (pnext >= plen) {   /* the next pass */
            if (pass_target == 16)
                find_min();
            pass_target = pass_target > 1 ? pass_target / 2
                        : pass_target == 1 && view.ss > 1 && !aa_full ? 0 : -1;
            if (pass_target < 0) {
                view_ns = now() - view_t0;
                break;
            }
            build_list();
            continue;
        }
        uint64_t t0 = now();
        if (did && t0 >= until)
            break;
        uint32_t left = plen - pnext, n = threads;
        if (tile_ns > 0) {
            double fit = (double)(until > t0 ? until - t0 : 0) * threads / tile_ns;
            n = fit < threads ? threads : fit > left ? left : (uint32_t)fit;
        }
        if (n > left)
            n = left;
        for (uint32_t i = 0; i < threads; i++)
            iters_by[i] = 0;
        pbase = pnext;
        if (serial)
            for (uint32_t i = 0; i < n; i++)
                tile_item(i, 0, NULL);
        else
            pool_run(tile_item, NULL, n);
        pnext += n;
        did = true;
        double per = (double)(now() - t0) * threads / n;
        tile_ns = tile_ns > 0 ? (tile_ns + per) / 2 : per;
        for (uint32_t i = 0; i < threads; i++)
            view_iters += iters_by[i];
    }
}

/* How far the passes are, 0..1. */
double progress(void)
{
    if (pass_target < 0)
        return 1;
    static const int lv[] = { 16, 8, 4, 2, 1, 0 };
    int k = 0;
    while (k < 5 && lv[k] != pass_target)
        k++;
    double per = 1.0 / (view.ss > 1 ? 6 : 5);
    return per * k + per * (plen ? (double)pnext / plen : 1);
}

static void aa_clear(void)
{
    memset(aa_idx, 0, (uint64_t)PW * PH * 4);
    aa_used = 0;
    aa_full = false;
}

void aa_next(void)
{
    view.ss = view.ss % MAXSS + 1;
    aa_clear();
    for (int t = 0; t < NT; t++)
        if (tlev[t] == 0)
            tlev[t] = 1;
    if (pass_target <= 0) {   /* (else it comes after the passes still going) */
        pass_target = view.ss > 1 ? 0 : -1;
        if (pass_target == 0) {
            view_t0 = now();
            build_list();
        }
    }
}

/* Start the view again (a new place, a new kind of view); keep_picture:
 * show the old picture until the passes replace it (same place). */
void reset_tiles(bool keep_picture)
{
    for (int t = 0; t < NT; t++) {
        tlev[t] = LV_NONE;
        tapprox[t] = keep_picture;
        tdirty[t] = 1;
    }
    aa_clear();
    apply_view();
    restart_passes();
}

/* ---- reusing what was computed ------------------------------------------------------------- */

static double rp_f;              /* reproject: the zoom factor */
static int rp_l;                 /* ... and the old picture's finest complete level */

/* Where new pixel x (of n) was in the old picture: the nearest point of
 * the old level-rp_l grid (points computed exactly, not stretched ones). */
static inline int old_px(int x, int n)
{
    double q = ((double)x + 0.5 - n / 2.0) / rp_f + n / 2.0;
    int i = (int)floord((q - 0.5) / rp_l + 0.5) * rp_l;
    if (i >= n)
        i -= rp_l;
    return i < 0 && q >= 0 ? 0 : i;
}

static void reproject_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int yo = old_px((int)y, PH);
    float *out = nu2 + (uint64_t)y * PW;
    if (yo < 0 || yo >= PH) {
        for (int x = 0; x < PW; x++)
            out[x] = -2;
        return;
    }
    const float *src = nu + (uint64_t)yo * PW;
    for (int x = 0; x < PW; x++) {
        int xo = old_px(x, PW);
        out[x] = xo >= 0 && xo < PW ? src[xo] : -2;
    }
}

/* The view was zoomed by f about its centre: stretch the old picture as a
 * first guess (tiles it doesn't cover start empty). */
void reproject(double f)
{
    rp_f = f;
    rp_l = pass_target < 0 || pass_target == 0 ? 1 : pass_target == 16 ? 16 : pass_target * 2;
    pool_run(reproject_row, NULL, (uint32_t)PH);
    float *t = nu;
    nu = nu2;
    nu2 = t;
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            int x0 = tx * TS, y0 = ty * TS, x1 = x0 + TS - 1 < PW ? x0 + TS - 1 : PW - 1;
            int y1 = y0 + TS - 1 < PH ? y0 + TS - 1 : PH - 1;
            /* stretched only if the old picture was good (every 4th pixel
             * computed); a stretch of a stretch is worse than blocks */
            bool cover = rp_l <= 4 && nu[(uint64_t)y0 * PW + x0] > -2 &&
                         nu[(uint64_t)y0 * PW + x1] > -2 && nu[(uint64_t)y1 * PW + x0] > -2 &&
                         nu[(uint64_t)y1 * PW + x1] > -2;
            int k = ty * TW + tx;
            tlev[k] = LV_NONE;
            tapprox[k] = cover;
            tdirty[k] = 1;
        }
    aa_clear();
    apply_view();
    restart_passes();
}

static int sh_dx, sh_dy;

static void shift_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    int ys = (int)y + sh_dy;
    float *out = nu2 + (uint64_t)y * PW;
    uint32_t *ai = aa_idx2 + (uint64_t)y * PW;
    for (int x = 0; x < PW; x++) {
        int xs = x + sh_dx;
        bool in = ys >= 0 && ys < PH && xs >= 0 && xs < PW;
        out[x] = in ? nu[(uint64_t)ys * PW + xs] : -2;
        ai[x] = in ? aa_idx[(uint64_t)ys * PW + xs] : 0;
    }
}

/* The view moved by (dx, dy) pixels (whole tiles): keep what is still on screen. */
void shift(int dx, int dy)
{
    sh_dx = dx;
    sh_dy = dy;
    pool_run(shift_row, NULL, (uint32_t)PH);
    float *t = nu;
    nu = nu2;
    nu2 = t;
    uint32_t *a = aa_idx;
    aa_idx = aa_idx2;
    aa_idx2 = a;
    int tdx = dx / TS, tdy = dy / TS;
    uint8_t *lv = (uint8_t *)nu2, *ap = lv + NT;   /* scratch: the old nu2 is free now */
    memcpy(lv, tlev, (size_t)NT);
    memcpy(ap, tapprox, (size_t)NT);
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            int sx = tx + tdx, sy = ty + tdy, k = ty * TW + tx;
            /* the last row / column of tiles may be partial: redo those */
            bool in = sx >= 0 && sy >= 0 && sx < TW && sy < TH && (sx + 1) * TS <= PW &&
                      (sy + 1) * TS <= PH && (tx + 1) * TS <= PW && (ty + 1) * TS <= PH;
            tlev[k] = in ? lv[sy * TW + sx] : LV_NONE;
            tapprox[k] = in ? ap[sy * TW + sx] : 0;
            tdirty[k] = 1;
        }
    apply_view();
    restart_passes();
}

/* The middle first: the tiles into order[] by distance from the centre
 * (a counting sort). How many were placed. */
static int order_tiles(void)
{
    int maxd = TW + TH, n = 0;
    for (int d = 0; d <= maxd; d++)
        for (int ty = 0; ty < TH; ty++)
            for (int tx = 0; tx < TW; tx++) {
                int dx = 2 * tx + 1 - TW, dy = 2 * ty + 1 - TH;
                if ((int)sqrtd((double)dx * dx + (double)dy * dy) / 2 == d)
                    order[n++] = (uint32_t)(ty * TW + tx);
            }
    return n;
}

bool view_alloc(int w, int h)
{
    PW = w;
    PH = h;
    TW = (w + TS - 1) / TS;
    TH = (h + TS - 1) / TS;
    NT = TW * TH;
    uint64_t px = (uint64_t)w * h;
    nu = big_alloc(px * 4);
    nu2 = big_alloc(px * 4);
    aa_idx = big_alloc(px * 4);
    aa_idx2 = big_alloc(px * 4);
    aa_cap = (uint32_t)((64ull << 20) / (MAXSS * MAXSS * 4));
    if (aa_cap > px)
        aa_cap = (uint32_t)px;
    aa_pool = big_alloc((uint64_t)aa_cap * MAXSS * MAXSS * 4);
    tlev = big_alloc((uint64_t)NT);
    tapprox = big_alloc((uint64_t)NT);
    tdirty = big_alloc((uint64_t)NT);
    order = big_alloc((uint64_t)NT * 4);
    plist = big_alloc((uint64_t)NT * 4);
    clist = big_alloc((uint64_t)NT * 4);
    if (!nu || !nu2 || !aa_idx || !aa_idx2 || !aa_pool || !tlev || !tapprox || !tdirty ||
        !order || !plist || !clist)
        return false;
    return order_tiles() == NT;
}

/* Render the whole view, all passes, on the pool (or one thread). */
uint64_t render_all(bool parallel)
{
    reset_tiles(false);
    serial = !parallel;
    work(~0ull);
    serial = false;
    return view_iters;
}
