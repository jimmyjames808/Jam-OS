/* libfun: the screen borrowed from the console, and drawing on surfaces (fun.h). */
#include <idl/console.h>
#include "fun.h"

/* ---- the screen -------------------------------------------------------------------- */

struct screen scr;

static status_t map_vmo(handle_t vmo, uint64_t len, void **out)
{
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ | VMAR_WRITE,
                               &addr);
    *out = (void *)(uintptr_t)addr;
    return st;
}

status_t gfx_open(void)
{
    memset(&scr, 0, sizeof(scr));
    scr.con = startup_handle(SR_CONSOLE);
    if (!scr.con)
        return ERR_NOT_FOUND;
    /* Keys first: then the screen (the console lends it to anyone who asks
     * on a PROGRAM channel; the focus makes the keys ours while we run). */
    status_t st = console_open_keys(scr.con, &scr.keys);
    if (st != OK)
        return st;
    uint32_t w, h, pitch;
    uint8_t rs, gs, bs;
    uint64_t size;
    handle_t vmo;
    st = console_lend_screen(scr.con, &w, &h, &pitch, &rs, &gs, &bs, &size, &vmo, &scr.lease);
    if (st != OK) {
        jam_handle_close(scr.keys);
        return st;
    }
    void *p = NULL;
    if (w < 320 || h < 200 || w > 8192 || h > 8192 || pitch < w * 4 || size < (uint64_t)pitch * h)
        st = ERR_NOT_SUPPORTED;
    else
        st = map_vmo(vmo, (size + 4095) & ~4095ull, &p);
    jam_handle_close(vmo);   /* the mapping keeps it */
    uint64_t px = (uint64_t)w * h * 4;
    if (st == OK) {
        scr.s.px = big_alloc(px);
        scr.shown = big_alloc(px);
        if (!scr.s.px || !scr.shown)
            st = ERR_NO_MEMORY;
    }
    if (st != OK) {
        jam_handle_close(scr.lease);
        jam_handle_close(scr.keys);
        return st;
    }
    scr.fb = p;
    scr.w = scr.s.w = scr.s.stride = (int)w;
    scr.h = scr.s.h = (int)h;
    scr.pitch = pitch;
    scr.rs = rs;
    scr.gs = gs;
    scr.bs = bs;
    scr.native = rs == 16 && gs == 8 && bs == 0;
    scr.ui = h > 1100 ? 2 : 1;
    scr.open = true;
    /* The shown copy starts black; the screen starts as the console left
     * it, so the first present writes everything. */
    gfx_present_all();
    return OK;
}

void gfx_close(void)
{
    if (!scr.open)
        return;
    scr.open = false;
    jam_handle_close(scr.lease);   /* the console redraws its text screen */
    jam_handle_close(scr.keys);    /* and the keys go back to the shell */
    scr.lease = scr.keys = HANDLE_INVALID;
}

/* Present: rows in bands of PBAND, each row in pieces of PSEG pixels; a
 * piece that differs from what the screen shows is written to both. */
#define PBAND 16
#define PSEG  64

/* Not turned into rep movsb (slow under QEMU; a plain loop of wide stores
 * is what write-combining memory likes best anyway). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void copy_px(uint32_t *restrict d, const uint32_t *restrict s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static inline bool same_px(const uint32_t *a, const uint32_t *b, int n)
{
    const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
    uint64_t diff = 0;
    for (int i = 0; i < n / 2; i++)
        diff |= x[i] ^ y[i];
    if (n & 1)
        diff |= a[n - 1] ^ b[n - 1];
    return !diff;
}

static uint64_t present_bytes[FUN_MAX_THREADS];

/* n 0xRRGGBB pixels from b into a framebuffer with other channel
 * positions (scr.rs, gs, bs). */
static void convert_px(uint32_t *f, const uint32_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t c = b[i];
        f[i] = (c >> 16 & 0xff) << scr.rs | (c >> 8 & 0xff) << scr.gs | (c & 0xff) << scr.bs;
    }
}

static void present_band(uint32_t band, uint32_t me, void *arg)
{
    bool all = arg != NULL;
    int y0 = (int)band * PBAND, y1 = y0 + PBAND < scr.h ? y0 + PBAND : scr.h;
    uint64_t bytes = 0;
    for (int y = y0; y < y1; y++) {
        const uint32_t *b = scr.s.px + (uint64_t)y * scr.w;
        uint32_t *s = scr.shown + (uint64_t)y * scr.w;
        uint32_t *f = (uint32_t *)((uint8_t *)scr.fb + (uint64_t)y * scr.pitch);
        for (int x = 0; x < scr.w; x += PSEG) {
            int n = scr.w - x < PSEG ? scr.w - x : PSEG;
            if (!all && same_px(b + x, s + x, n))
                continue;
            copy_px(s + x, b + x, n);
            if (scr.native)
                copy_px(f + x, b + x, n);
            else
                convert_px(f + x, b + x, n);
            bytes += (uint64_t)n * 4;
        }
    }
    present_bytes[me] += bytes;
}

static void present(bool all)
{
    if (!scr.open)
        return;
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        present_bytes[i] = 0;
    pool_run(present_band, all ? (void *)1 : NULL, (uint32_t)(scr.h + PBAND - 1) / PBAND);
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        scr.bytes += present_bytes[i];
    scr.presents++;
}

void gfx_present(void) { present(false); }
void gfx_present_all(void) { present(true); }

/* ---- drawing ----------------------------------------------------------------------- */

static bool clip(const struct surf *s, int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > s->w)
        *w = s->w - *x;
    if (*y + *h > s->h)
        *h = s->h - *y;
    return *w > 0 && *h > 0;
}

void fill(const struct surf *s, int x, int y, int w, int h, uint32_t c)
{
    if (!clip(s, &x, &y, &w, &h))
        return;
    for (int j = y; j < y + h; j++) {
        uint32_t *p = s->px + (uint64_t)j * s->stride + x;
        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

void blend(const struct surf *s, int x, int y, int w, int h, uint32_t c, uint32_t a)
{
    if (!clip(s, &x, &y, &w, &h))
        return;
    for (int j = y; j < y + h; j++) {
        uint32_t *p = s->px + (uint64_t)j * s->stride + x;
        for (int i = 0; i < w; i++)
            p[i] = mixc(p[i], c, a);
    }
}

void panel(const struct surf *s, int x, int y, int w, int h, int r, uint32_t c, uint32_t a)
{
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (int j = 0; j < h; j++) {
        /* The corners: how far in the row starts (a quarter circle), with
         * the edge pixel at partial alpha. */
        int in = 0;
        uint32_t edge_a = a;
        int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
        if (dy) {
            /* where the row meets the circle: (r - x)^2 + dy^2 = r^2 */
            double rr = (double)r * r - (double)(dy - 0.5) * (dy - 0.5);
            double off = r - (rr > 0 ? sqrtd(rr) : 0);
            in = (int)off;
            edge_a = (uint32_t)(a * (1.0 - (off - in)));
        }
        if (in * 2 >= w)
            continue;
        blend(s, x + in, y + j, 1, 1, c, edge_a);
        blend(s, x + w - 1 - in, y + j, 1, 1, c, edge_a);
        blend(s, x + in + 1, y + j, w - 2 * in - 2, 1, c, a);
    }
}

void frame(const struct surf *s, int x, int y, int w, int h, int t, uint32_t c)
{
    fill(s, x, y, w, t, c);
    fill(s, x, y + h - t, w, t, c);
    fill(s, x, y + t, t, h - 2 * t, c);
    fill(s, x + w - t, y + t, t, h - 2 * t, c);
}

void vgrad(const struct surf *s, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
    for (int j = 0; j < h; j++)
        fill(s, x, y + j, w, 1, mixc(c0, c1, h > 1 ? (uint32_t)(j * 256 / (h - 1)) : 0));
}

void line(const struct surf *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < s->w && y0 < s->h)
            s->px[(uint64_t)y0 * s->stride + x0] = c;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void blit(const struct surf *dst, int x, int y, const struct surf *src, int sx, int sy, int w,
          int h)
{
    int x0 = x, y0 = y;
    if (!clip(dst, &x, &y, &w, &h))
        return;
    sx += x - x0;
    sy += y - y0;
    for (int j = 0; j < h; j++)
        copy_px(dst->px + (uint64_t)(y + j) * dst->stride + x,
                src->px + (uint64_t)(sy + j) * src->stride + sx, w);
}

void blit_key(const struct surf *dst, int x, int y, const struct surf *src, uint32_t key)
{
    for (int j = 0; j < src->h; j++) {
        if (y + j < 0 || y + j >= dst->h)
            continue;
        const uint32_t *sp = src->px + (uint64_t)j * src->stride;
        uint32_t *dp = dst->px + (uint64_t)(y + j) * dst->stride;
        for (int i = 0; i < src->w; i++)
            if (sp[i] != key && x + i >= 0 && x + i < dst->w)
                dp[x + i] = sp[i];
    }
}

struct surf surf_new(int w, int h)
{
    struct surf s = { big_alloc((uint64_t)w * h * 4), w, h, w };
    return s;
}
