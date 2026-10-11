/* libfun: the screen (fun.h): a window on the compositor (wl.c), and
 * drawing on surfaces. */
#include "internal.h"

/* ---- the screen -------------------------------------------------------------------- */

struct screen scr;

status_t gfx_open(void)
{
    return gfx_open_on(0);
}

/* A window if the compositor gives us one (M12's Q14: no screen is
 * borrowed from the console any more). */
static status_t open_screen(uint32_t bg, bool keys, bool full)
{
    status_t st = wl_open(bg, keys, full);
    if (st == OK)
        gfx_present_all();   /* the window shows bg at once */
    else if (st != ERR_NOT_FOUND)   /* a compositor in our namespace, but no window from it */
        printf("libfun: no window (%s)\n", status_str(st));
    else
        printf("libfun: no compositor (no /svc/wayland): no window\n");
    return st;
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
    wl_close();   /* the compositor takes the window off the screen */
}

/* n pixels from s to d: a plain loop of wide stores, never rep movsb
 * (slow under QEMU). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void px_copy(uint32_t *restrict d, const uint32_t *restrict s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void present(bool all)
{
    if (scr.open)
        wl_present(all);
}

void gfx_present(void) { present(false); }

void gfx_present_rects(const struct rect *r, unsigned n)
{
    if (scr.open && n)
        wl_present_rects(r, n);
}
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
