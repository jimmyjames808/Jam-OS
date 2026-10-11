/* libfun: presenting into a window (fun.h, wl.c's window).
 *
 * The window has two buffers (libjwl's, in a pool the compositor reads),
 * and each present writes into whichever the compositor isn't reading.
 * Both are kept up to date without copying a whole frame: a present
 *   1. compares the back buffer with what was shown (scr.shown), in
 *      pieces of PSEG pixels by bands of PBAND rows,
 *      and copies each changed piece to scr.shown; each one is marked in
 *      `pend` (changed since the last commit: its damage) and in
 *      `owed[i]` for both buffers (what buffer i doesn't have yet);
 *   2. waits for the last commit's frame callback (one commit a frame:
 *      the compositor's pace, about 60 a second; a hidden window's come
 *      once a second, so it slows down instead of drawing for nobody);
 *   3. takes the free buffer i, writes the pieces owed[i] from scr.shown
 *      into it (pieces this present changed and those the last present
 *      wrote into the other one), damages `pend`, commits and asks for
 *      the next frame callback.
 * A buffer the window hasn't held at this size and place yet (the first
 * present, a new size, a reconnect: `seen`) is written whole, with scr.bg
 * round the picture when the window is bigger than it; the damage is
 * still what changed, unless the window's size or the picture's place
 * changed since the last commit (`committed`) or it is a new connection.
 *
 * When no buffer is free (the compositor holds both, which it shouldn't
 * for long) the present ends after a short wait: what it changed stays
 * owed, and the next present writes it.
 *
 * A present of rectangles (gfx_present_rects: the app says where it drew)
 * is the same present with step 1 limited to the bands and pieces the
 * rectangles touch, and step 3 to the pieces owed (which are within
 * them). A frame in which little changes then costs the little: no
 * compare of the whole picture and no walk over all of it. Below
 * INLINE_PX pixels a pass runs on the calling thread: waking every pool
 * worker (each spins for a moment after, too) costs more CPU than a small
 * rectangle's work. */
#include <jwl_client.h>
#include "internal.h"

#define MAX_BANDS    (JWL_SIZE_MAX / PBAND)
#define MASK_WORDS   (JWL_SIZE_MAX / PSEG / 64)
#define DAMAGE_MAX   30                    /* rectangles a commit damages (the compositor keeps 32) */
#define BUFFER_WAIT  (100 * NS_PER_MS)     /* both buffers held: how long a present waits */
#define INLINE_PX    (2u << 20)            /* a pass over fewer pixels runs on the caller alone */

static uint64_t pend[MAX_BANDS][MASK_WORDS];      /* pieces changed since the last commit */
static uint64_t owed[2][MAX_BANDS][MASK_WORDS];   /* pieces buffer i doesn't hold yet */
static bool     damage_all;                       /* the next commit damages the whole window */

/* Where the picture was at the last commit: a window that changed size,
 * or a picture that moved in it, is damaged whole. */
static struct {
    int32_t w, h;
    int ox, oy;
} committed;

/* What each buffer holds: the picture scr.w x scr.h at (ox, oy) of a
 * window w x h, in the memory at px. Anything else: write it whole. */
static struct {
    const uint32_t *px;
    int32_t w, h;
    int ox, oy, sw, sh;
} seen[2];

static uint64_t write_bytes[FUN_MAX_THREADS];

/* The bands a pass visits (pool item i is band act[i]) and, for the
 * compare, the pieces [from[b], to[b]) of each. */
static uint16_t act[MAX_BANDS];
static uint16_t from[MAX_BANDS], to[MAX_BANDS];

void wl_paint_reset(void)
{
    memset(seen, 0, sizeof(seen));
    memset(pend, 0, sizeof(pend));
    memset(owed, 0, sizeof(owed));
    damage_all = true;
}

static int bands(void)
{
    return (scr.h + PBAND - 1) / PBAND;
}

/* v within 0 .. n */
static int clampi_px(int v, int n)
{
    return v < 0 ? 0 : v > n ? n : v;
}

/* ---- 1. what changed ------------------------------------------------------------------ */

static void compare_band(uint32_t item, uint32_t me, void *arg)
{
    bool all = *(const bool *)arg;
    (void)me;
    uint32_t band = act[item];
    int y0 = (int)band * PBAND, y1 = y0 + PBAND < scr.h ? y0 + PBAND : scr.h;
    uint64_t m[MASK_WORDS] = { 0 };
    for (int y = y0; y < y1; y++) {
        const uint32_t *b = scr.s.px + (uint64_t)y * scr.w;
        uint32_t *s = scr.shown + (uint64_t)y * scr.w;
        for (int p = from[band]; p < to[band]; p++) {
            int x = p * PSEG, n = scr.w - x < PSEG ? scr.w - x : PSEG;
            if (!all && px_same(b + x, s + x, n))
                continue;
            px_copy(s + x, b + x, n);
            m[p / 64] |= 1ull << (p % 64);
        }
    }
    for (int k = 0; k < MASK_WORDS; k++) {   /* this band's words are this item's alone */
        pend[band][k] |= m[k];
        owed[0][band][k] |= m[k];
        owed[1][band][k] |= m[k];
    }
}

/* Run fn over the n bands in act[] (px pixels of work): on the pool, or,
 * for a small pass, here alone. */
static void run_bands(void (*fn)(uint32_t, uint32_t, void *), void *arg, unsigned n,
                      uint64_t px, bool small_ok)
{
    if (!n)
        return;
    if (small_ok && px <= INLINE_PX) {
        for (unsigned i = 0; i < n; i++)
            fn(i, 0, arg);
        return;
    }
    pool_run(fn, arg, n);
}

/* Every band, every piece. */
static unsigned scope_all(void)
{
    int pieces = (scr.w + PSEG - 1) / PSEG;
    for (int b = 0; b < bands(); b++) {
        act[b] = (uint16_t)b;
        from[b] = 0;
        to[b] = (uint16_t)pieces;
    }
    return (unsigned)bands();
}

/* The bands and pieces rectangles r[0..n) (clipped to the picture) touch. */
static unsigned scope_rects(const struct rect *r, unsigned n, uint64_t *px)
{
    int pieces = (scr.w + PSEG - 1) / PSEG;
    for (int b = 0; b < bands(); b++) {
        from[b] = (uint16_t)pieces;
        to[b] = 0;
    }
    for (unsigned i = 0; i < n; i++) {
        int x0 = r[i].x < 0 ? 0 : r[i].x, y0 = r[i].y < 0 ? 0 : r[i].y;
        int x1 = r[i].x + r[i].w > scr.w ? scr.w : r[i].x + r[i].w;
        int y1 = r[i].y + r[i].h > scr.h ? scr.h : r[i].y + r[i].h;
        if (x0 >= x1 || y0 >= y1)
            continue;
        int p0 = x0 / PSEG, p1 = (x1 + PSEG - 1) / PSEG;
        for (int b = y0 / PBAND; b <= (y1 - 1) / PBAND; b++) {
            from[b] = p0 < from[b] ? (uint16_t)p0 : from[b];
            to[b] = p1 > to[b] ? (uint16_t)p1 : to[b];
        }
    }
    unsigned k = 0;
    *px = 0;
    for (int b = 0; b < bands(); b++)
        if (to[b] > from[b]) {
            act[k++] = (uint16_t)b;
            *px += (uint64_t)(to[b] - from[b]) * PSEG * PBAND;
        }
    return k;
}

/* ---- 3. into a buffer ------------------------------------------------------------------- */

struct write_job {
    const struct jwl_frame *fr;
    unsigned slot;
    int ox, oy;     /* the picture's place in the window */
    bool whole;     /* every piece, not only those owed */
};

/* Picture columns x .. x + n - 1 of row y into the window, clipped to it. */
static uint64_t put_span(const struct write_job *j, int y, int x, int n)
{
    int wy = y + j->oy, x0 = x + j->ox, x1 = x0 + n;
    if (wy < 0 || wy >= j->fr->height)
        return 0;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > j->fr->width ? j->fr->width : x1;
    if (x0 >= x1)
        return 0;
    uint32_t *d = j->fr->px + (uint64_t)wy * (uint64_t)(j->fr->stride / 4);
    px_copy(d + x0, scr.shown + (uint64_t)y * scr.w + (x0 - j->ox), x1 - x0);
    return (uint64_t)(x1 - x0) * 4;
}

static void write_band(uint32_t item, uint32_t me, void *arg)
{
    const struct write_job *j = arg;
    uint32_t band = act[item];
    uint64_t *m = owed[j->slot][band], bytes = 0;
    int y0 = (int)band * PBAND, y1 = y0 + PBAND < scr.h ? y0 + PBAND : scr.h;
    for (int y = y0; y < y1; y++) {
        if (j->whole) {
            bytes += put_span(j, y, 0, scr.w);
            continue;
        }
        for (int x = 0, p = 0; x < scr.w; x += PSEG, p++)
            if (m[p / 64] >> (p % 64) & 1)
                bytes += put_span(j, y, x, scr.w - x < PSEG ? scr.w - x : PSEG);
    }
    for (int k = 0; k < MASK_WORDS; k++)
        m[k] = 0;
    write_bytes[me] += bytes;
}

/* Window rows of a band of PBAND: scr.bg wherever the picture isn't. */
static void border_band(uint32_t item, uint32_t me, void *arg)
{
    const struct write_job *j = arg;
    (void)me;
    int h = j->fr->height, w = j->fr->width;
    int y0 = (int)item * PBAND, y1 = y0 + PBAND < h ? y0 + PBAND : h;
    for (int y = y0; y < y1; y++) {
        uint32_t *d = j->fr->px + (uint64_t)y * (uint64_t)(j->fr->stride / 4);
        bool in = y >= j->oy && y < j->oy + scr.h;
        int a = in ? clampi_px(j->ox, w) : w, b = in ? clampi_px(j->ox + scr.w, w) : w;
        for (int x = 0; x < a; x++)
            d[x] = scr.bg;
        for (int x = b; x < w; x++)
            d[x] = scr.bg;
    }
}

/* ---- damage ------------------------------------------------------------------------------ */

/* The pieces of pend (cleared) as window rectangles into r (DAMAGE_MAX
 * room): one a band, bands running on with the same columns merged; past
 * DAMAGE_MAX, the box round them all. Returns how many. */
static unsigned damage_rects(struct jwl_rect *r, const struct write_job *j)
{
    unsigned n = 0;
    int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = 0, by1 = 0;
    for (int b = 0; b < bands(); b++) {
        int first = -1, last = -1;
        for (int p = 0; p < MASK_WORDS * 64; p++)
            if (pend[b][p / 64] >> (p % 64) & 1) {
                first = first < 0 ? p : first;
                last = p;
            }
        memset(pend[b], 0, sizeof(pend[b]));
        if (first < 0)
            continue;
        int x0 = clampi_px(first * PSEG + j->ox, j->fr->width);
        int x1 = clampi_px((last + 1) * PSEG + j->ox, j->fr->width);
        int y0 = clampi_px(b * PBAND + j->oy, j->fr->height);
        int y1 = clampi_px((b + 1) * PBAND + j->oy, j->fr->height);
        if (x0 >= x1 || y0 >= y1)
            continue;
        bx0 = x0 < bx0 ? x0 : bx0, by0 = y0 < by0 ? y0 : by0;
        bx1 = x1 > bx1 ? x1 : bx1, by1 = y1 > by1 ? y1 : by1;
        struct jwl_rect *prev = n && n <= DAMAGE_MAX ? &r[n - 1] : NULL;
        if (prev && prev->x == x0 && prev->w == x1 - x0 && prev->y + prev->h == y0)
            prev->h += y1 - y0;
        else if (n++ < DAMAGE_MAX)
            r[n - 1] = (struct jwl_rect){ x0, y0, x1 - x0, y1 - y0 };
    }
    if (n > DAMAGE_MAX) {
        r[0] = (struct jwl_rect){ bx0, by0, bx1 - bx0, by1 - by0 };
        n = 1;
    }
    return n;
}

/* ---- a present --------------------------------------------------------------------------- */

/* A free buffer of the window's, waiting a little if both are held. */
static status_t free_buffer(struct jwl_frame *fr)
{
    uint64_t until = now() + BUFFER_WAIT;
    status_t st = jwl_window_begin(wl_window(), fr);
    while (st == ERR_SHOULD_WAIT && now() < until && !wl_dead()) {
        wl_wait_until(until);
        st = jwl_window_begin(wl_window(), fr);
    }
    return st;
}

/* Write buffer j->slot: everything (with the border) or what it is owed. */
static void write_buffer(struct write_job *j)
{
    unsigned s = j->slot;
    j->whole = seen[s].px != j->fr->px || seen[s].w != j->fr->width ||
               seen[s].h != j->fr->height || seen[s].ox != j->ox || seen[s].oy != j->oy ||
               seen[s].sw != scr.w || seen[s].sh != scr.h;
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        write_bytes[i] = 0;
    if (j->whole)
        pool_run(border_band, j, (uint32_t)((j->fr->height + PBAND - 1) / PBAND));
    unsigned n = 0;
    uint64_t px = 0;
    if (j->whole) {
        n = scope_all();
    } else {   /* the bands with something owed */
        for (int b = 0; b < bands(); b++) {
            unsigned pieces = 0;
            for (int k = 0; k < MASK_WORDS; k++)
                pieces += (unsigned)__builtin_popcountll(owed[s][b][k]);
            if (pieces) {
                act[n++] = (uint16_t)b;
                px += (uint64_t)pieces * PSEG * PBAND;
            }
        }
    }
    run_bands(write_band, j, n, px, !j->whole);
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        scr.bytes += write_bytes[i];
    seen[s].px = j->fr->px;
    seen[s].w = j->fr->width;
    seen[s].h = j->fr->height;
    seen[s].ox = j->ox;
    seen[s].oy = j->oy;
    seen[s].sw = scr.w;
    seen[s].sh = scr.h;
}

/* Anything changed since the last commit. */
static bool changed(void)
{
    for (int b = 0; b < bands(); b++)
        for (int k = 0; k < MASK_WORDS; k++)
            if (pend[b][k])
                return true;
    return damage_all;
}

/* A present of the bands in act[0..n) (compared as from[] and to[] say,
 * px pixels of it; all: every piece is taken as changed). */
static void present(bool all, unsigned n, uint64_t px, bool small_ok)
{
    run_bands(compare_band, &all, n, px, small_ok);
    if (!changed())
        return;   /* nothing to write, nothing to wait for */
    wl_wait_frame();
    struct jwl_frame fr;
    if (free_buffer(&fr) != OK)
        return;   /* owed: the next present writes it */
    int32_t w, h;
    struct write_job j = { .fr = &fr, .slot = fr.slot };
    wl_place(&w, &h, &j.ox, &j.oy);
    write_buffer(&j);
    struct jwl_rect r[DAMAGE_MAX];
    unsigned nr = damage_rects(r, &j);
    bool whole = damage_all || committed.w != fr.width || committed.h != fr.height ||
                 committed.ox != j.ox || committed.oy != j.oy;
    damage_all = false;
    committed.w = fr.width;
    committed.h = fr.height;
    committed.ox = j.ox;
    committed.oy = j.oy;
    if (jwl_window_present(wl_window(), whole ? NULL : r, whole ? 0 : nr, true) == OK)
        wl_frame_asked();   /* else the connection went: a reconnect shows it all again */
    scr.presents++;
}

void wl_present(bool all)
{
    if (wl_dead())
        return;
    present(all, scope_all(), 0, false);
}

void wl_present_rects(const struct rect *r, unsigned n)
{
    if (wl_dead())
        return;
    uint64_t px;
    unsigned k = scope_rects(r, n, &px);
    present(false, k, px, true);
}
