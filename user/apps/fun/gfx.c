/* libfun: the screen (fun.h): a window (wl.c) or, without one, the screen
 * borrowed from the console; and drawing on surfaces. */
#include <idl/console.h>
#include "internal.h"

/* ---- the screen -------------------------------------------------------------------- */

struct screen scr;
static handle_t console_with;   /* gfx_console_with's, HANDLE_INVALID: SR_CONSOLE */

void gfx_console_with(handle_t con)
{
    console_with = con;
}

static status_t map_vmo(handle_t vmo, uint64_t len, void **out)
{
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ | VMAR_WRITE,
                               &addr);
    *out = (void *)(uintptr_t)addr;
    return st;
}

/* Drop map_vmo's mapping (p NULL: none). A failure leaves it mapped,
 * which nothing writes again: the screen is closed. */
static void unmap_vmo(void *p, uint64_t len)
{
    if (p)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p, len);
}

status_t gfx_open(void)
{
    return gfx_open_on(0);
}

/* The borrowed screen, and with `keys` the keyboard focus. */
static status_t borrow_screen(uint32_t bg, bool keys)
{
    memset(&scr, 0, sizeof(scr));
    scr.con = console_with != HANDLE_INVALID ? console_with : startup_handle(SR_CONSOLE);
    if (!scr.con)
        return ERR_NOT_FOUND;
    /* Keys first: then the screen (the console lends it to anyone who asks
     * on a PROGRAM channel; the focus makes the keys ours while we run). */
    status_t st = keys ? console_open_keys(scr.con, &scr.keys) : OK;
    if (st != OK)
        return st;
    uint32_t w, h, pitch;
    uint8_t rs, gs, bs;
    uint64_t size;
    handle_t vmo;
    st = console_lend_screen(scr.con, &w, &h, &pitch, &rs, &gs, &bs, &size, &vmo, &scr.lease);
    if (st != OK) {
        if (scr.keys)
            jam_handle_close(scr.keys);
        return st;
    }
    void *p = NULL;
    uint64_t len = (size + 4095) & ~4095ull;
    if (w < 320 || h < 200 || w > 8192 || h > 8192 || pitch < w * 4 || size < (uint64_t)pitch * h)
        st = ERR_NOT_SUPPORTED;
    else
        st = map_vmo(vmo, len, &p);
    jam_handle_close(vmo);   /* the mapping keeps it */
    uint64_t px = (uint64_t)w * h * 4;
    if (st == OK) {
        scr.s.px = big_alloc(px);
        scr.shown = big_alloc(px);
        if (!scr.s.px || !scr.shown)
            st = ERR_NO_MEMORY;
    }
    if (st != OK) {
        big_free(scr.s.px, px);
        big_free(scr.shown, px);
        unmap_vmo(p, len);
        jam_handle_close(scr.lease);
        if (scr.keys)
            jam_handle_close(scr.keys);
        return st;
    }
    scr.fb = p;
    scr.fb_len = len;
    scr.w = scr.s.w = scr.s.stride = (int)w;
    scr.h = scr.s.h = (int)h;
    scr.pitch = pitch;
    scr.rs = rs;
    scr.gs = gs;
    scr.bs = bs;
    scr.native = rs == 16 && gs == 8 && bs == 0;
    scr.ui = h > 1100 ? 2 : 1;
    scr.bg = bg;
    scr.open = true;
    /* The shown copy starts black; the screen starts as the console left
     * it, so the first present writes everything. */
    if (bg)
        fill(&scr.s, 0, 0, scr.w, scr.h, bg);
    gfx_present_all();
    return OK;
}

/* A window if the compositor gives us one, else the borrowed screen. */
static status_t open_screen(uint32_t bg, bool keys, bool full)
{
    if (console_with != HANDLE_INVALID)
        return borrow_screen(bg, keys);
    status_t st = wl_open(bg, keys, full);
    if (st == OK) {
        gfx_present_all();   /* the window shows bg at once */
        return OK;
    }
    if (st != ERR_NOT_FOUND)   /* a compositor in our namespace, but no window from it */
        printf("libfun: no window (%s): borrowing the screen\n", status_str(st));
    return borrow_screen(bg, keys);
}

status_t gfx_open_on(uint32_t bg)
{
    return open_screen(bg, true, false);
}

status_t gfx_open_fullscreen(uint32_t bg)
{
    return open_screen(bg, true, true);
}

status_t gfx_open_screen(uint32_t bg)
{
    return open_screen(bg, false, true);
}

void gfx_close(void)
{
    if (!scr.open)
        return;
    scr.open = false;
    mouse_close();
    if (scr.windowed) {
        wl_close();   /* the compositor takes the window off the screen */
        return;
    }
    /* The screen's mapping goes before the lease, so nothing of ours can
     * draw over the console once it has its screen back; then the back
     * buffers (as a window's go in wl_close). */
    uint64_t px = (uint64_t)scr.w * (uint64_t)scr.h * 4;
    unmap_vmo(scr.fb, scr.fb_len);
    jam_handle_close(scr.lease);   /* the console redraws its text screen */
    if (scr.keys)
        jam_handle_close(scr.keys);   /* and the keys go back to the shell */
    big_free(scr.s.px, px);
    big_free(scr.shown, px);
    scr.s.px = scr.shown = scr.fb = NULL;
    scr.fb_len = 0;
    scr.lease = scr.keys = HANDLE_INVALID;
}

/* Present: rows in bands of PBAND, each row in pieces of PSEG pixels; a
 * piece that differs from what the screen shows is written to both. Not
 * turned into rep movsb (slow under QEMU; a plain loop of wide stores is
 * what write-combining memory likes best anyway). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void px_copy(uint32_t *restrict d, const uint32_t *restrict s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
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

/* One present: bands `first` .. of the screen, everything or what changed. */
struct present_job {
    bool     all;     /* write every piece, not only the changed ones */
    uint32_t first;   /* the first band */
};

static void present_band(uint32_t item, uint32_t me, void *arg)
{
    const struct present_job *job = arg;
    bool all = job->all;
    int y0 = (int)(job->first + item) * PBAND, y1 = y0 + PBAND < scr.h ? y0 + PBAND : scr.h;
    uint64_t bytes = 0;
    for (int y = y0; y < y1; y++) {
        const uint32_t *b = scr.s.px + (uint64_t)y * scr.w;
        uint32_t *s = scr.shown + (uint64_t)y * scr.w;
        uint32_t *f = (uint32_t *)((uint8_t *)scr.fb + (uint64_t)y * scr.pitch);
        for (int x = 0; x < scr.w; x += PSEG) {
            int n = scr.w - x < PSEG ? scr.w - x : PSEG;
            if (!all && px_same(b + x, s + x, n))
                continue;
            px_copy(s + x, b + x, n);
            if (scr.native)
                px_copy(f + x, b + x, n);
            else
                convert_px(f + x, b + x, n);
            bytes += (uint64_t)n * 4;
        }
    }
    present_bytes[me] += bytes;
}

/* Rows y0 .. y1 - 1 to the screen, with the mouse's arrow over them: it is
 * in the back buffer only for as long as the copy takes. */
static void present(bool all, int y0, int y1)
{
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > scr.h ? scr.h : y1;
    if (!scr.open || y0 >= y1)
        return;
    if (scr.windowed) {
        wl_present(all);   /* the whole picture: a window has no arrow to move */
        return;
    }
    struct present_job job = { all, (uint32_t)y0 / PBAND };
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        present_bytes[i] = 0;
    pointer_paint();
    pool_run(present_band, &job, (uint32_t)(y1 + PBAND - 1) / PBAND - job.first);
    pointer_unpaint();
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        scr.bytes += present_bytes[i];
    scr.presents++;
}

void gfx_present(void) { present(false, 0, scr.h); }
void gfx_present_all(void) { present(true, 0, scr.h); }
void gfx_present_rows(int y0, int y1) { present(false, y0, y1); }

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

void blend(const struct surf *s, const struct rect *r, uint32_t c, uint32_t a)
{
    int x = r->x, y = r->y, w = r->w, h = r->h;
    if (!clip(s, &x, &y, &w, &h))
        return;
    for (int j = y; j < y + h; j++) {
        uint32_t *p = s->px + (uint64_t)j * s->stride + x;
        for (int i = 0; i < w; i++)
            p[i] = mixc(p[i], c, a);
    }
}

void panel(const struct surf *s, const struct rect *at, int radius, uint32_t c, uint32_t a)
{
    int x = at->x, y = at->y, w = at->w, h = at->h, r = radius;
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
        blend(s, &(struct rect){ x + in, y + j, 1, 1 }, c, edge_a);
        blend(s, &(struct rect){ x + w - 1 - in, y + j, 1, 1 }, c, edge_a);
        blend(s, &(struct rect){ x + in + 1, y + j, w - 2 * in - 2, 1 }, c, a);
    }
}

void frame(const struct surf *s, const struct rect *r, int t, uint32_t c)
{
    int x = r->x, y = r->y, w = r->w, h = r->h;
    fill(s, x, y, w, t, c);
    fill(s, x, y + h - t, w, t, c);
    fill(s, x, y + t, t, h - 2 * t, c);
    fill(s, x + w - t, y + t, t, h - 2 * t, c);
}

void vgrad(const struct surf *s, const struct rect *r, uint32_t c0, uint32_t c1)
{
    int h = r->h;
    for (int j = 0; j < h; j++)
        fill(s, r->x, r->y + j, r->w, 1, mixc(c0, c1, h > 1 ? (uint32_t)(j * 256 / (h - 1)) : 0));
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

void blit(const struct surf *dst, int x, int y, const struct surf *src, const struct rect *from)
{
    int x0 = x, y0 = y, sx = from->x, sy = from->y, w = from->w, h = from->h;
    if (!clip(dst, &x, &y, &w, &h))
        return;
    sx += x - x0;
    sy += y - y0;
    for (int j = 0; j < h; j++)
        px_copy(dst->px + (uint64_t)(y + j) * dst->stride + x,
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
