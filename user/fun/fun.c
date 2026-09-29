/* The fun apps' shared code: see fun.h. */
#include "fun.h"
#include <idl/console.h>
/* The console font (Spleen 8x16), for text(). */
#include "../../kernel/dev/font_8x16.c"

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
            if (scr.native) {
                copy_px(f + x, b + x, n);
            } else {
                for (int i = 0; i < n; i++) {
                    uint32_t c = b[x + i];
                    f[x + i] = (c >> 16 & 0xff) << scr.rs | (c >> 8 & 0xff) << scr.gs |
                               (c & 0xff) << scr.bs;
                }
            }
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

/* ---- text ---------------------------------------------------------------------------- */

/* Per glyph: the first ink column and the advance at scale 1; and the
 * glyph at twice the size smoothed by scale2x (EPX), for scales >= 2. */
static struct { int8_t left; uint8_t adv; } gm[128];
static uint16_t big_glyph[128][32];
static bool text_ready;

static inline int gbit(uint8_t c, int x, int y)
{
    if (x < 0 || x > 7 || y < 0 || y > 15)
        return 0;
    return font_8x16[c][y] >> (7 - x) & 1;
}

static void text_init(void)
{
    int digit_w = 0;
    for (int c = 32; c < 127; c++) {
        int lo = 8, hi = -1;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                if (gbit((uint8_t)c, x, y)) {
                    lo = x < lo ? x : lo;
                    hi = x > hi ? x : hi;
                }
        if (hi < 0) {   /* space */
            gm[c].left = 0;
            gm[c].adv = 4;
        } else {
            gm[c].left = (int8_t)lo;
            gm[c].adv = (uint8_t)(hi - lo + 2);
        }
        if (c >= '0' && c <= '9' && hi - lo + 1 > digit_w)
            digit_w = hi - lo + 1;
        /* scale2x: each pixel P becomes 4, a corner taking a neighbour's
         * value where two neighbours agree (rounds the diagonals). */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++) {
                int p = gbit((uint8_t)c, x, y), a = gbit((uint8_t)c, x, y - 1);
                int b = gbit((uint8_t)c, x + 1, y), l = gbit((uint8_t)c, x - 1, y);
                int d = gbit((uint8_t)c, x, y + 1);
                int e0 = p, e1 = p, e2 = p, e3 = p;
                if (l == a && l != d && a != b)
                    e0 = a;
                if (a == b && a != l && b != d)
                    e1 = b;
                if (d == l && d != b && l != a)
                    e2 = l;
                if (b == d && b != a && d != l)
                    e3 = d;
                big_glyph[c][2 * y] |= (uint16_t)(e0 << (15 - 2 * x) | e1 << (14 - 2 * x));
                big_glyph[c][2 * y + 1] |= (uint16_t)(e2 << (15 - 2 * x) | e3 << (14 - 2 * x));
            }
    }
    /* Digits: all as wide as the widest, centred (numbers don't wobble). */
    for (int c = '0'; c <= '9'; c++) {
        int ink = gm[c].adv - 1;
        gm[c].left = (int8_t)(gm[c].left - (digit_w - ink) / 2);
        gm[c].adv = (uint8_t)(digit_w + 1);
    }
    text_ready = true;
}

static void glyph(const struct surf *s, int x, int y, int scale, uint32_t c, uint8_t ch)
{
    int left = gm[ch].left, w = (gm[ch].adv - 1) * scale;
    for (int j = 0; j < 16 * scale; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= s->h)
            continue;
        uint32_t *row = s->px + (uint64_t)yy * s->stride;
        if (scale == 1) {
            uint8_t bits = (uint8_t)(left >= 0 ? font_8x16[ch][j] << left : font_8x16[ch][j] >> -left);
            for (int i = 0; bits && i < 8; i++, bits <<= 1)
                if ((bits & 0x80) && x + i >= 0 && x + i < s->w)
                    row[x + i] = c;
            continue;
        }
        uint16_t bits = big_glyph[ch][j * 2 / scale];
        for (int i = 0; i < w; i++) {
            int sx = (2 * left) + i * 2 / scale;   /* column in the 16-wide glyph */
            if (sx >= 0 && sx < 16 && (bits >> (15 - sx) & 1) && x + i >= 0 && x + i < s->w)
                row[x + i] = c;
        }
    }
}

static int draw_text(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt,
                     bool shadow, const char *str)
{
    if (!text_ready)
        text_init();
    if (scale < 1)
        scale = 1;
    if (shadow) {   /* a darkened halo one step down-right */
        int o = scale > 1 ? scale / 2 + 1 : 1;
        int xx = x;
        for (const char *p = str; *p; p++) {
            uint8_t ch = (uint8_t)*p;
            if (ch == '\a' || ch < 32 || ch > 126)
                continue;
            glyph(s, xx + o, y + o, scale, 0x000000, ch);
            xx += gm[ch].adv * scale;
        }
    }
    bool use_alt = false;
    for (; *str; str++) {
        uint8_t ch = (uint8_t)*str;
        if (ch == '\a') {
            use_alt = !use_alt;
            continue;
        }
        if (ch < 32 || ch > 126)
            ch = '?';
        glyph(s, x, y, scale, use_alt ? alt : c, ch);
        x += gm[ch].adv * scale;
    }
    return x;
}

int text(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str)
{
    return draw_text(s, x, y, scale, c, c, false, str);
}

int text_shadow(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str)
{
    return draw_text(s, x, y, scale, c, c, true, str);
}

int text2(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt, bool shadow,
          const char *str)
{
    return draw_text(s, x, y, scale, c, alt, shadow, str);
}

int textf(const struct surf *s, int x, int y, int scale, uint32_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return draw_text(s, x, y, scale, c, c, false, buf);
}

int text_width(int scale, const char *str)
{
    if (!text_ready)
        text_init();
    int w = 0;
    for (; *str; str++) {
        uint8_t ch = (uint8_t)*str;
        if (ch == '\a')
            continue;
        if (ch < 32 || ch > 126)
            ch = '?';
        w += gm[ch].adv;
    }
    return w * (scale < 1 ? 1 : scale);
}

void fps_frame(struct fps *f)
{
    uint64_t t = now_ns();
    if (!f->t0)
        f->t0 = t;
    f->n++;
    if (t - f->t0 >= 500000000ull) {
        f->x10 = (uint32_t)((uint64_t)f->n * 10000000000ull / (t - f->t0));
        f->t0 = t;
        f->n = 0;
    }
}

/* ---- keys ---------------------------------------------------------------------------- */

int key_decode(const struct input_key_event *ev)
{
    if (ev->state == INPUT_KEY_UP)
        return KEY_NONE;
    uint32_t cp = ev->codepoint;
    if (cp == 0x1b || ev->usage == 0x29 || cp == 3)
        return KEY_QUIT;
    if ((ev->mods & INPUT_MOD_CTRL) && (cp == 'c' || ev->usage == 0x06))
        return KEY_QUIT;
    switch (ev->usage) {
    case 0x52: return KEY_UP;
    case 0x51: return KEY_DOWN;
    case 0x50: return KEY_LEFT;
    case 0x4f: return KEY_RIGHT;
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x4b: return KEY_PGUP;
    case 0x4e: return KEY_PGDN;
    case 0x4a: return KEY_HOME;
    case 0x57: return '+';   /* keypad */
    case 0x56: return '-';
    }
    if (cp == '\n' || cp == '\r')
        return KEY_ENTER;
    if (cp >= 0x20 && cp < 0x7f)
        return (int)cp;
    return KEY_NONE;
}

int gfx_key(uint64_t deadline)
{
    if (!scr.keys)
        return KEY_QUIT;
    for (;;) {
        struct input_key_event ev;
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = scr.keys, .bytes_cap = sizeof(ev), .bytes = (uint64_t)(uintptr_t)&ev,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            int k = n == sizeof(ev) ? key_decode(&ev) : KEY_NONE;
            if (k != KEY_NONE)
                return k;
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return KEY_QUIT;   /* the console went away */
        if (!deadline)
            return KEY_NONE;
        signals_t seen;
        st = jam_object_wait_one(scr.keys, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return KEY_NONE;
        if (st != OK)
            return KEY_QUIT;
    }
}

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

uint32_t fun_cpu_count(void)
{
    uint32_t r[4], n = 0;
    cpuid(0, 0, r);
    uint32_t max = r[0];
    /* V2 extended topology (0x1F) or extended topology (0xB): the last level
     * before "invalid" counts every logical CPU in the package. */
    for (uint32_t leaf = 0x1f; leaf >= 0xb && !n; leaf = leaf == 0x1f ? 0xb : 0) {
        if (max < leaf)
            continue;
        for (uint32_t sub = 0; sub < 8; sub++) {
            cpuid(leaf, sub, r);
            if (((r[2] >> 8) & 0xff) == 0)
                break;
            if (r[1] & 0xffff)
                n = r[1] & 0xffff;
        }
    }
    if (!n) {
        cpuid(1, 0, r);
        n = (r[1] >> 16) & 0xff;
    }
    if (n < 1)
        n = 1;
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    return n;
}

bool fun_has_avx2(void)
{
    uint32_t r[4];
    cpuid(0, 0, r);
    if (r[0] < 7)
        return false;
    cpuid(1, 0, r);
    bool fma = r[2] >> 12 & 1, osxsave = r[2] >> 27 & 1, avx = r[2] >> 28 & 1;
    if (!fma || !osxsave || !avx)
        return false;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6)   /* the OS saves SSE and AVX state */
        return false;
    cpuid(7, 0, r);
    return r[1] >> 5 & 1;
}

bool fun_is_tcg(void)
{
    uint32_t r[4];
    cpuid(1, 0, r);
    if (!(r[2] >> 31 & 1))   /* no hypervisor */
        return false;
    cpuid(0x40000000, 0, r);
    return r[1] == 0x54474354 && r[2] == 0x43544743 && r[3] == 0x47435447;   /* "TCGTCGTCGTCG" */
}

#define POOL_STACK (64u << 10)
#define POOL_SPIN  20000   /* pauses (~1 ms) before a worker sleeps */

static struct {
    uint32_t n;
    handle_t ev[FUN_MAX_THREADS];
    volatile uint32_t sleeping[FUN_MAX_THREADS];
    volatile uint32_t phase, next, done, items;
    void (*fn)(uint32_t, uint32_t, void *);
    void *arg;
} pool = { .n = 1 };
uint32_t pool_items_by[FUN_MAX_THREADS];

static void pool_work(uint32_t me)
{
    uint32_t i, did = 0;
    while ((i = __atomic_fetch_add(&pool.next, 1, __ATOMIC_RELAXED)) < pool.items) {
        pool.fn(i, me, pool.arg);
        did++;
    }
    pool_items_by[me] = did;
    __atomic_fetch_add(&pool.done, 1, __ATOMIC_RELEASE);
}

static void pool_worker(void *a)
{
    uint32_t me = (uint32_t)(uintptr_t)a, seen = 0;
    for (;;) {
        uint32_t ph, spins = 0;
        while ((ph = __atomic_load_n(&pool.phase, __ATOMIC_ACQUIRE)) == seen) {
            if (++spins < POOL_SPIN) {
                __builtin_ia32_pause();
                continue;
            }
            /* Sleep: announce it, clear the event, look once more (pool_run
             * bumps the phase, then signals every worker that announced). */
            __atomic_store_n(&pool.sleeping[me], 1, __ATOMIC_SEQ_CST);
            jam_event_signal(pool.ev[me], SIG_SIGNALED, 0);
            if (__atomic_load_n(&pool.phase, __ATOMIC_SEQ_CST) == seen)
                jam_object_wait_one(pool.ev[me], SIG_SIGNALED, DEADLINE_NEVER, NULL);
            __atomic_store_n(&pool.sleeping[me], 0, __ATOMIC_SEQ_CST);
            spins = 0;
        }
        seen = ph;
        pool_work(me);
    }
}

uint32_t pool_start(uint32_t n)
{
    if (pool.n > 1)
        return pool.n;   /* already running */
    if (!n)
        n = fun_cpu_count();
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    pool.n = 1;
    for (uint32_t i = 1; i < n; i++) {
        void *stack = malloc(POOL_STACK);
        handle_t th;
        if (!stack || jam_event_create(&pool.ev[i]) != OK)
            break;
        if (thread_spawn("worker", pool_worker, (void *)(uintptr_t)i, stack, POOL_STACK, &th) != OK)
            break;
        jam_handle_close(th);
        pool.n = i + 1;
    }
    return pool.n;
}

uint32_t pool_threads(void) { return pool.n; }

void pool_run(void (*fn)(uint32_t, uint32_t, void *), void *arg, uint32_t items)
{
    pool.fn = fn;
    pool.arg = arg;
    pool.items = items;
    pool.next = 0;
    pool.done = 0;
    __atomic_add_fetch(&pool.phase, 1, __ATOMIC_SEQ_CST);
    for (uint32_t i = 1; i < pool.n; i++)
        if (__atomic_load_n(&pool.sleeping[i], __ATOMIC_SEQ_CST))
            jam_event_signal(pool.ev[i], 0, SIG_SIGNALED);
    pool_work(0);
    while (__atomic_load_n(&pool.done, __ATOMIC_ACQUIRE) < pool.n)
        __builtin_ia32_pause();
}

/* ---- maths ----------------------------------------------------------------------------- */

double log2d(double x)
{
    union { double d; uint64_t u; } u = { x };
    int e = (int)((u.u >> 52) & 0x7ff) - 1023;
    u.u = (u.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;   /* m in [1, 2) */
    double m = u.d;
    if (m > 1.4142135623730951) {   /* m in [0.707, 1.414): a faster series */
        m *= 0.5;
        e++;
    }
    double t = (m - 1) / (m + 1), t2 = t * t;
    /* ln(m) = 2 atanh(t) */
    double ln = 2 * t * (1 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 * (1.0 / 7 + t2 * (1.0 / 9 +
                t2 * (1.0 / 11 + t2 / 13))))));
    return e + ln * 1.4426950408889634;
}

double exp2d(double x)
{
    if (x < -1000)
        return 0;
    if (x > 1000)
        x = 1000;
    double fl = floord(x);
    double f = (x - fl) * 0.6931471805599453, term = 1, sum = 1;
    for (int k = 1; k < 14; k++) {
        term *= f / k;
        sum += term;
    }
    union { double d; uint64_t u; } v = { sum };
    v.u += (uint64_t)(int64_t)fl << 52;
    return v.d;
}

double sind(double x)
{
    const double pi = 3.141592653589793, tau = 2 * pi;
    x -= tau * (double)(int64_t)(x / tau);
    if (x > pi)
        x -= tau;
    if (x < -pi)
        x += tau;
    double x2 = x * x, term = x, sum = x;
    for (int k = 1; k < 11; k++) {
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

/* ---- odds and ends ---------------------------------------------------------------------- */

void *big_alloc(uint64_t bytes)
{
    bytes = (bytes + 4095) & ~4095ull;
    handle_t v;
    if (jam_vmo_create(bytes, 0, HANDLE_INVALID, &v) != OK)
        return NULL;
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, bytes, VMAR_READ | VMAR_WRITE,
                               &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st == OK ? (void *)(uintptr_t)addr : NULL;
}

void say(const char *fmt, ...)
{
    char buf[2048];   /* console_write sends a whole 2048-byte buffer */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    handle_t con = startup_handle(SR_CONSOLE);
    if (!con || console_write(con, (uint16_t)n, (const uint8_t *)buf) != OK)
        printf("%s", buf);
}

char *commas(char *buf, size_t n, uint64_t v)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v), o = 0;
    for (int i = 0; i < len && (size_t)o + 1 < n; i++) {
        if (i && (len - i) % 3 == 0 && (size_t)o + 2 < n)
            buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
    return buf;
}

bool has_arg(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

uint64_t arg_num(int argc, char **argv, const char *name, uint64_t def)
{
    size_t k = strlen(name);
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], name, k) && argv[i][k] == '=') {
            uint64_t v = 0;
            for (const char *p = argv[i] + k + 1; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (uint64_t)(*p - '0');
            return v;
        }
    return def;
}
