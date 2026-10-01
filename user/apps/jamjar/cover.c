/* jamjar: album covers, the pictures the owner's MP3s carry in their ID3v2
 * tags, read and decoded by a thread of its own so that a frame never waits
 * for the stick.
 *
 * The UI asks (cover_ready) for every album it draws, every frame, and is
 * told what is ready; an album not asked for yet is put on the list, and
 * every album of the library is put on it once at the back (reading ahead,
 * so the roulette and the lists fill in). The thread takes the newest
 * request first (the albums on the screen now), reads the first ID3_HEADER
 * bytes of the track, then the whole tag (at most TAG_MAX), finds the
 * picture (id3.c), checks its size before decoding it (COVER_MAX_SIDE,
 * COVER_MAX_PIXELS), decodes it (stbi.c's bounded arena), crops it to its
 * middle square and scales it down by area averaging, in premultiplied
 * alpha, to COVER_SMALL (kept for every album: SMALL_SLOTS of them, the
 * least recently drawn going first) and, for an album drawn bigger than
 * that (now playing, the full jar), to COVER_LARGE too (LARGE_SLOTS).
 * An album with no picture, or one that fails, is marked so and keeps its
 * jar label (art.c). cover_render scales a kept image to the size drawn,
 * with rounded corners; art.c keeps that result.
 *
 * Sharing: `lock`, a spinlock, guards the table and the slots. The thread
 * holds it only to pick work and to copy a finished image in; the UI holds
 * it while it scales from a slot, so a slot is never reused under it. */
#include "jamjar.h"

#define ALBUMS      1024u            /* albums known at once: the table */
#define SMALL_SLOTS 128u
#define LARGE_SLOTS 2u
#define TAG_MAX     (6u << 20)       /* a tag bigger than this is not read */
#define STACK       (64u * 1024)

enum { ST_FREE, ST_WANTED, ST_LOADING, ST_READY, ST_NONE };

struct entry {
    uint64_t hash;                   /* 0: free */
    char     path[FS_PATH_MAX];      /* the track to read it from */
    uint8_t  st, lst;                /* the small image's state, the large one's (ST_FREE: not wanted) */
    uint32_t seq, lseq;              /* when last asked for (0: reading ahead) */
    int16_t  slot, lslot;            /* where its images are (-1: none) */
    uint64_t used;                   /* the draw count when last drawn */
};

static struct {
    bool          lock;
    struct entry  tab[ALBUMS];       /* lock */
    uint32_t     *small;             /* SMALL_SLOTS images, COVER_SMALL squared each */
    uint32_t     *large;             /* LARGE_SLOTS images, COVER_LARGE squared each */
    int16_t       small_of[SMALL_SLOTS], large_of[LARGE_SLOTS];   /* lock: the entry, -1 */
    uint32_t      seq;               /* lock */
    uint64_t      draws;             /* lock */
    handle_t      wake;
    bool          trace;
} C;

static uint32_t tmp_small[COVER_SMALL * COVER_SMALL];   /* the thread's own */
static uint32_t tmp_large[COVER_LARGE * COVER_LARGE];

static void lock(void)
{
    while (__atomic_test_and_set(&C.lock, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_clear(&C.lock, __ATOMIC_RELEASE);
}

/* ---- scaling (premultiplied 0xAARRGGBB) ------------------------------------------- */

/* src (sw x sh, `stride` pixels a row) to dst (dw x dh): each output pixel
 * the average of the source area it covers, partial pixels weighted by how
 * much of them it covers. For making smaller; cover_scale picks. */
static void box(const uint32_t *src, int sw, int sh, int stride, uint32_t *dst, int dw, int dh)
{
    float fx = (float)sw / (float)dw, fy = (float)sh / (float)dh;
    for (int y = 0; y < dh; y++) {
        float y0 = (float)y * fy, y1 = y0 + fy;
        for (int x = 0; x < dw; x++) {
            float x0 = (float)x * fx, x1 = x0 + fx, acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            for (int sy = (int)y0; sy < sh && (float)sy < y1; sy++) {
                float wy = ((float)(sy + 1) < y1 ? (float)(sy + 1) : y1) -
                           ((float)sy > y0 ? (float)sy : y0);
                for (int sx = (int)x0; sx < sw && (float)sx < x1; sx++) {
                    float w = wy * (((float)(sx + 1) < x1 ? (float)(sx + 1) : x1) -
                                    ((float)sx > x0 ? (float)sx : x0));
                    uint32_t p = src[(size_t)sy * stride + sx];
                    for (int c = 0; c < 4; c++)
                        acc[c] += w * (float)(p >> (8 * c) & 0xff);
                    wsum += w;
                }
            }
            uint32_t o = 0;
            for (int c = 0; c < 4; c++)
                o |= (uint32_t)(acc[c] / (wsum > 0 ? wsum : 1) + 0.5f) << (8 * c);
            dst[(size_t)y * dw + x] = o;
        }
    }
}

/* Bilinear, for making bigger (pixel centres line up). */
static void bilinear(const uint32_t *src, int sw, int sh, int stride, uint32_t *dst, int dw,
                     int dh)
{
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        fy = fy < 0 ? 0 : fy;
        int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float ty = fy - (float)y0;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            fx = fx < 0 ? 0 : fx;
            int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float tx = fx - (float)x0;
            uint32_t a = src[(size_t)y0 * stride + x0], b = src[(size_t)y0 * stride + x1];
            uint32_t c = src[(size_t)y1 * stride + x0], d = src[(size_t)y1 * stride + x1], o = 0;
            for (int k = 0; k < 32; k += 8) {
                float top = (float)(a >> k & 0xff) * (1 - tx) + (float)(b >> k & 0xff) * tx;
                float bot = (float)(c >> k & 0xff) * (1 - tx) + (float)(d >> k & 0xff) * tx;
                o |= (uint32_t)(top * (1 - ty) + bot * ty + 0.5f) << k;
            }
            dst[(size_t)y * dw + x] = o;
        }
    }
}

void cover_scale(const uint32_t *src, int sw, int sh, int stride, uint32_t *dst, int dw, int dh)
{
    if (dw <= sw && dh <= sh)
        box(src, sw, sh, stride, dst, dw, dh);
    else
        bilinear(src, sw, sh, stride, dst, dw, dh);
}

/* stb_image's RGBA bytes, in place, as premultiplied 0xAARRGGBB. */
static void premultiply(uint8_t *p, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++, p += 4) {
        uint32_t a = p[3], r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
        uint32_t v = a << 24 | r << 16 | g << 8 | b;
        memcpy(p, &v, 4);
    }
}

/* ---- the table ------------------------------------------------------------------------ */

static int find(uint64_t hash, bool add)
{
    hash = hash ? hash : 1;
    for (uint32_t k = 0, i = (uint32_t)(hash % ALBUMS); k < ALBUMS; k++, i = (i + 1) % ALBUMS) {
        struct entry *e = &C.tab[i];
        if (e->hash == hash)
            return (int)i;
        if (!e->hash) {
            if (!add)
                return -1;
            memset(e, 0, sizeof(*e));
            e->hash = hash;
            e->slot = e->lslot = -1;
            return (int)i;
        }
    }
    return -1;
}

int cover_ready(uint64_t hash, const char *path, int size, bool low)
{
    if (!path || !path[0])
        return COVER_NONE;
    bool big = size > (int)COVER_SMALL * 9 / 8, wake = false;
    lock();
    int i = find(hash, true);
    if (i < 0) {
        unlock();
        return COVER_NONE;
    }
    struct entry *e = &C.tab[i];
    if (e->st == ST_FREE) {
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->st = ST_WANTED;
        wake = true;
    }
    if (!low && e->st == ST_WANTED && e->seq != C.seq)
        e->seq = ++C.seq;
    if (big && e->st != ST_NONE && (e->lst == ST_FREE || e->lst == ST_WANTED)) {
        wake |= e->lst == ST_FREE;
        e->lst = ST_WANTED;
        if (e->lseq != C.seq)
            e->lseq = ++C.seq;
    }
    if (!low)
        e->used = ++C.draws;
    int kind = big && e->lst == ST_READY ? COVER_LARGE_KIND
               : e->st == ST_READY ? COVER_SMALL_KIND : COVER_NONE;
    unlock();
    if (wake && C.wake)
        (void)jam_event_signal(C.wake, 0, SIG_SIGNALED);
    return kind;
}

/* ---- drawing a kept image ----------------------------------------------------------- */

/* The share of pixel (i, j) inside a size x size square with corners of
 * radius r, 0..256. */
static uint32_t inside(int i, int j, int size, float r)
{
    float cx = (float)i + 0.5f, cy = (float)j + 0.5f, s = (float)size;
    float kx = cx < r ? r : cx > s - r ? s - r : cx, ky = cy < r ? r : cy > s - r ? s - r : cy;
    float dx = cx - kx, dy = cy - ky, d = sqrtf_(dx * dx + dy * dy) - r;
    float c = 0.5f - d;
    return c <= 0 ? 0 : c >= 1 ? 256 : (uint32_t)(c * 256);
}

static uint32_t times(uint32_t p, uint32_t k)
{
    uint32_t o = 0;
    for (int c = 0; c < 32; c += 8)
        o |= ((p >> c & 0xff) * k >> 8) << c;
    return o;
}

bool cover_render(const struct surf *dst, int x, int y, int size, uint64_t hash, int kind,
                  uint32_t bg)
{
    uint32_t *px = malloc((size_t)size * (size_t)size * 4);
    if (!px)
        return false;
    lock();
    int i = find(hash, false);
    const struct entry *e = i >= 0 ? &C.tab[i] : NULL;
    bool large = kind == COVER_LARGE_KIND;
    bool ok = e && (large ? e->lst == ST_READY && e->lslot >= 0 : e->st == ST_READY && e->slot >= 0);
    if (ok) {
        int side = large ? (int)COVER_LARGE : (int)COVER_SMALL;
        const uint32_t *src = large ? C.large + (size_t)e->lslot * COVER_LARGE * COVER_LARGE
                                    : C.small + (size_t)e->slot * COVER_SMALL * COVER_SMALL;
        cover_scale(src, side, side, side, px, size, size);
    }
    unlock();
    float r = (float)size / 10.0f;
    for (int j = 0; ok && j < size; j++) {
        if (y + j < 0 || y + j >= dst->h)
            continue;
        for (int k = 0; k < size; k++) {
            if (x + k < 0 || x + k >= dst->w)
                continue;
            uint32_t *d = dst->px + (uint64_t)(y + j) * dst->stride + x + k;
            *d = px_over(bg, times(px[(size_t)j * size + k], inside(k, j, size, r)));
        }
    }
    free(px);
    return ok;
}

/* ---- the thread --------------------------------------------------------------------- */

/* The next thing to do: the entry and whether it is for the large image;
 * -1 for nothing. Marks it loading. */
static int next_job(bool *for_large)
{
    int best = -1;
    uint32_t best_seq = 0;
    lock();
    for (uint32_t i = 0; i < ALBUMS; i++) {
        const struct entry *e = &C.tab[i];
        if (!e->hash)
            continue;
        if (e->st == ST_WANTED && (best < 0 || e->seq > best_seq)) {
            best = (int)i;
            best_seq = e->seq;
        }
        if (e->st == ST_READY && e->lst == ST_WANTED && (best < 0 || e->lseq > best_seq)) {
            best = (int)i;
            best_seq = e->lseq;
        }
    }
    if (best >= 0) {
        struct entry *e = &C.tab[best];
        *for_large = e->st == ST_READY;
        if (e->st == ST_WANTED)
            e->st = ST_LOADING;
        if (e->lst == ST_WANTED)
            e->lst = ST_LOADING;
    }
    unlock();
    return best;
}

bool cover_size_ok(int w, int h)
{
    return w >= 1 && h >= 1 && w <= (int)COVER_MAX_SIDE && h <= (int)COVER_MAX_SIDE &&
           (uint64_t)w * (uint64_t)h <= COVER_MAX_PIXELS;
}

/* The whole tag of the file at path, in the arena: its length, 0 (with
 * *why) if there is none or it can't be read. */
static size_t read_tag(const char *path, uint8_t **out, const char **why)
{
    struct jfile f;
    uint8_t h[ID3_HEADER];
    size_t got = 0, n = 0;
    *why = "can't be opened";
    if (file_open(path, FS_READ, &f) != OK)
        return 0;
    *why = "has no ID3v2 tag";
    if (file_read(&f, 0, h, sizeof(h), &got) == OK && got == sizeof(h))
        n = id3_tag_size(h);
    if (n > TAG_MAX) {
        *why = "has a tag too big to read";
        n = 0;
    }
    uint8_t *tag = n ? stbi_arena_take(n) : NULL;
    size_t at = 0;
    while (tag && at < n && file_read(&f, at, tag + at, n - at, &got) == OK && got)
        at += got;
    file_close(&f);
    if (!tag || at < n) {
        *why = tag ? "can't be read" : *why;
        return 0;
    }
    *out = tag;
    return n;
}

/* Decode the picture of the track at path, crop it square and scale it
 * into the thread's buffers. NULL on success, else why not. */
static const char *decode(const char *path, bool large, int *w, int *h)
{
    uint8_t *tag;
    const char *why;
    size_t n = read_tag(path, &tag, &why);
    struct id3_pic pic;
    if (!n)
        return why;
    if (!id3_cover(tag, n, &pic))
        return "has no picture in its tag";
    if (!stbi_size(pic.data, pic.len, w, h))
        return "has a picture that is not a PNG or JPEG it can read";
    if (!cover_size_ok(*w, *h))
        return "has a picture too big to decode";
    uint8_t *rgba = stbi_rgba(pic.data, pic.len, w, h);
    if (!rgba)
        return "has a picture that doesn't decode";
    premultiply(rgba, (size_t)*w * (size_t)*h);
    const uint32_t *px = (const uint32_t *)rgba;
    int side = *w < *h ? *w : *h, ox = (*w - side) / 2, oy = (*h - side) / 2;
    const uint32_t *sq = px + (size_t)oy * *w + ox;
    cover_scale(sq, side, side, *w, tmp_small, COVER_SMALL, COVER_SMALL);
    if (large)
        cover_scale(sq, side, side, *w, tmp_large, COVER_LARGE, COVER_LARGE);
    return NULL;
}

/* A slot for entry `owner`: a free one, else the least recently drawn
 * other album's (its image is dropped). lock held. */
static int claim(int16_t *of, uint32_t nslots, bool large, int owner)
{
    int pick = -1;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t s = 0; s < nslots; s++) {
        if (of[s] < 0)
            return (int)s;
        const struct entry *e = &C.tab[of[s]];
        if (of[s] != owner && e->used < oldest) {
            oldest = e->used;
            pick = (int)s;
        }
    }
    if (pick >= 0) {
        struct entry *e = &C.tab[of[pick]];
        if (large) {
            e->lslot = -1;
            e->lst = ST_FREE;   /* asked for again when drawn big again */
        } else {
            e->slot = -1;
            e->st = ST_FREE;
        }
    }
    return pick;
}

static void store(int i, bool large, bool ok)
{
    lock();
    struct entry *e = &C.tab[i];
    if (!large) {
        int s = ok && e->slot < 0 ? claim(C.small_of, SMALL_SLOTS, false, i) : e->slot;
        if (ok && s >= 0) {
            memcpy(C.small + (size_t)s * COVER_SMALL * COVER_SMALL, tmp_small, sizeof(tmp_small));
            C.small_of[s] = (int16_t)i;
            e->slot = (int16_t)s;
        }
        e->st = ok && s >= 0 ? ST_READY : ST_NONE;
    }
    if (e->lst == ST_LOADING) {
        int s = ok && e->lslot < 0 ? claim(C.large_of, LARGE_SLOTS, true, i) : e->lslot;
        if (ok && s >= 0) {
            memcpy(C.large + (size_t)s * COVER_LARGE * COVER_LARGE, tmp_large, sizeof(tmp_large));
            C.large_of[s] = (int16_t)i;
            e->lslot = (int16_t)s;
        }
        e->lst = ok && s >= 0 ? ST_READY : ST_NONE;
    }
    unlock();
}

static void cover_main(void *arg)
{
    (void)arg;
    static char path[FS_PATH_MAX];
    for (;;) {
        bool large = false;
        int i = next_job(&large);
        if (i < 0) {
            (void)jam_object_wait_one(C.wake, SIG_SIGNALED, DEADLINE_NEVER, NULL);
            (void)jam_event_signal(C.wake, SIG_SIGNALED, 0);
            continue;
        }
        lock();
        memcpy(path, C.tab[i].path, sizeof(path));
        bool want_large = C.tab[i].lst == ST_LOADING;
        unlock();
        int w = 0, h = 0;
        uint64_t t0 = now();
        const char *why = decode(path, want_large, &w, &h);
        stbi_arena_reset();
        store(i, large, !why);
        if (C.trace && why)
            say("jamjar: cover: %s %s: no cover\n", path, why);
        else if (C.trace)
            say("jamjar: cover: %s: %dx%d in %lu ms%s\n", path, w, h,
                (unsigned long)((now() - t0) / NS_PER_MS), want_large ? " (large too)" : "");
    }
}

/* One VMO of `bytes`, mapped; its pages are committed as they are touched. */
static uint32_t *map_new(uint64_t bytes)
{
    handle_t v;
    uint64_t addr = 0;
    if (jam_vmo_create(bytes, 0, HANDLE_INVALID, &v) != OK)
        return NULL;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, bytes, VMAR_READ | VMAR_WRITE,
                               &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st == OK ? (uint32_t *)(uintptr_t)addr : NULL;
}

void cover_start(bool trace)
{
    C.trace = trace;
    for (uint32_t s = 0; s < SMALL_SLOTS; s++)
        C.small_of[s] = -1;
    for (uint32_t s = 0; s < LARGE_SLOTS; s++)
        C.large_of[s] = -1;
    C.small = map_new((uint64_t)SMALL_SLOTS * COVER_SMALL * COVER_SMALL * 4);
    C.large = map_new((uint64_t)LARGE_SLOTS * COVER_LARGE * COVER_LARGE * 4);
    void *stack = malloc(STACK);
    handle_t th;
    if (!C.small || !C.large || !stack || jam_event_create(&C.wake) != OK ||
        thread_spawn("covers", cover_main, NULL, stack, STACK, &th) != OK) {
        say("jamjar: no covers (out of memory): the albums keep their jar labels\n");
        C.wake = HANDLE_INVALID;
        return;
    }
    jam_handle_close(th);
}
