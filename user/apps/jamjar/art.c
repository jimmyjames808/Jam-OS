/* jamjar: jar labels, the album art made up from an album's folder.
 *
 * Every album gets a flavour, two fruits picked by a hash of its folder
 * (so the same album always looks the same and two albums rarely match),
 * and a label: a rounded square in the first fruit's colour, lit from the
 * top left, holding a cluster of three to nine berries packed on a
 * hexagonal grid the way the Jam OS mark's drupelets are, grown from the
 * middle one neighbour at a time, turned by the hash, coloured between
 * the two fruits (now and then one gold berry, as in the mark), each
 * with a highlight. It is what an album shows when its tracks carry no
 * cover picture (cover.c), or until the cover is read. Whatever is drawn
 * at a size, the label or the cover scaled, is kept (ART_CACHE of them and
 * ART_BUDGET bytes at most, the least recently used going first), so
 * drawing it again is a copy.
 *
 * Also the mark itself, for the top bar: the seven drupelets of
 * docs/logo/jamos-mark.svg. */
#include "jamjar.h"

#define ART_CACHE  64
/* Half of libos's 16 MiB heap (HEAP_SIZE), which also holds the library
 * and the threads' stacks (stb_image has an arena of its own). At
 * 2560x1440 now playing's cover is 757 KB; a frame draws about 1 MB of
 * pictures, 3 MB with the roulette up. */
#define ART_BUDGET (8u << 20)
#define MAX_BERRIES 9

static const struct fruit {
    const char *name;
    uint32_t    c;
} fruits[] = {
    { "Raspberry", 0xc8284f }, { "Blackberry", 0x4c1f52 }, { "Blueberry", 0x3f53a0 },
    { "Apricot", 0xe0913a },   { "Plum", 0x7d2a63 },       { "Gooseberry", 0x8fa83c },
    { "Cherry", 0xa5162f },    { "Fig", 0x70507a },        { "Strawberry", 0xe0344a },
    { "Blackcurrant", 0x3a1a40 }, { "Quince", 0xd4ae3c },  { "Rhubarb", 0xd65a7c },
};
#define NFRUITS (sizeof(fruits) / sizeof(fruits[0]))

/* splitmix64: the hash spread into as many numbers as a label needs. */
static uint64_t mix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void art_flavour(uint64_t hash, uint32_t *c0, uint32_t *c1, char *name, size_t cap)
{
    uint64_t s = hash;
    uint32_t a = (uint32_t)(mix(&s) % NFRUITS);
    uint32_t b = (a + 1 + (uint32_t)(mix(&s) % (NFRUITS - 1))) % NFRUITS;
    *c0 = fruits[a].c;
    *c1 = fruits[b].c;
    if (name)
        snprintf(name, cap, "%s & %s", fruits[a].name, fruits[b].name);
}

/* The cluster: n cells of a hexagonal grid (axial q, r), grown from (0, 0). */
static int grow(uint64_t *s, int n, int q[MAX_BERRIES], int r[MAX_BERRIES])
{
    static const int dq[6] = { 1, 1, 0, -1, -1, 0 }, dr[6] = { 0, -1, -1, 0, 1, 1 };
    int have = 1;
    q[0] = r[0] = 0;
    for (int guard = 0; have < n && guard < 200; guard++) {
        int from = (int)(mix(s) % (uint64_t)have), d = (int)(mix(s) % 6);
        int nq = q[from] + dq[d], nr = r[from] + dr[d];
        bool taken = false;
        for (int i = 0; i < have; i++)
            taken |= q[i] == nq && r[i] == nr;
        if (!taken) {
            q[have] = nq;
            r[have++] = nr;
        }
    }
    return have;
}

/* The label of hash, size x size, into s at (x, y), over bg. */
static void label(const struct surf *s, int x, int y, int size, uint64_t hash, uint32_t bg)
{
    uint32_t c0, c1;
    art_flavour(hash, &c0, &c1, NULL, 0);
    uint64_t st = hash ^ 0x6a616d6a6172ull;
    fill(s, x, y, size, size, bg);
    float f = (float)size;
    panel(s, x, y, size, size, size / 7, mixc(C_BG, c0, 90), 256);
    /* A soft sheen from the top left: three discs, fainter as they grow. */
    for (int i = 0; i < 3; i++)
        disc_aa(s, (float)x + f * 0.32f, (float)y + f * 0.28f, f * (0.12f + 0.065f * i),
                mixc(c0, C_CREAM, 70), 16);   /* inside the rounded corners */
    int q[MAX_BERRIES], r[MAX_BERRIES];
    int n = grow(&st, 3 + (int)(mix(&st) % 7), q, r);
    /* Cell centres, turned by the hash and centred; then scaled to fit. */
    float px[MAX_BERRIES], py[MAX_BERRIES], ang = (float)(mix(&st) % 6283) / 1000.0f;
    float cs = (float)cosd(ang), sn = (float)sind(ang), mx = 0, my = 0, ext = 0;
    for (int i = 0; i < n; i++) {
        float hx = (float)q[i] + (float)r[i] * 0.5f, hy = (float)r[i] * 0.8660254f;
        px[i] = hx * cs - hy * sn;
        py[i] = hx * sn + hy * cs;
        mx += px[i] / (float)n;
        my += py[i] / (float)n;
    }
    for (int i = 0; i < n; i++) {
        float dx = px[i] - mx, dy = py[i] - my, e = sqrtf_(dx * dx + dy * dy) + 0.5f;
        ext = e > ext ? e : ext;
    }
    float unit = f * 0.36f / (ext > 1.0f ? ext : 1.0f), rad = unit * 0.47f;
    int gold = (mix(&st) & 1) ? (int)(mix(&st) % (uint64_t)n) : -1;
    for (int i = 0; i < n; i++) {
        uint32_t t = (uint32_t)(mix(&st) % 4) * 85;
        uint32_t c = i == gold ? C_GOLD : mixc(c0, c1, t);
        float bx = (float)x + f / 2 + (px[i] - mx) * unit, by = (float)y + f / 2 + (py[i] - my) * unit;
        disc_aa(s, bx + rad * 0.12f, by + rad * 0.16f, rad, 0x000000, 70);   /* a shadow */
        disc_aa(s, bx, by, rad, c, 255);
        disc_aa(s, bx - rad * 0.38f, by - rad * 0.38f, rad * 0.26f, C_CREAM, 80);
    }
}

/* The kept pictures: one per album, size and background, whichever kind
 * it is now (a kind that changes is drawn again over the old one, in
 * place). Their bytes are counted against ART_BUDGET, so the cache can
 * never eat the heap that the library and the rest of the app live in. */
static struct art_slot {
    uint64_t  hash;
    int       size;
    uint32_t  bg;
    int       kind;      /* COVER_NONE: the jar label; else which cover image it came from */
    uint64_t  used;      /* the draw count when last used */
    uint32_t *px;        /* size * size, malloc'd; NULL: free slot */
} cache[ART_CACHE];
static uint64_t draws;
static size_t   kept;    /* bytes of the pictures in the cache */
static void   (*between)(void);   /* the self-test's hook (art_test_hook) */

static size_t bytes_of(int size)
{
    return (size_t)size * (size_t)size * 4;
}

size_t art_cache_bytes(void)
{
    return kept;
}

void art_test_hook(void (*fn)(void))
{
    between = fn;
}

static void forget(struct art_slot *c)
{
    free(c->px);
    c->px = NULL;
    kept -= bytes_of(c->size);
}

/* The kept picture of hash at size over bg, or NULL. */
static struct art_slot *lookup(uint64_t hash, int size, uint32_t bg)
{
    for (int i = 0; i < ART_CACHE; i++)
        if (cache[i].px && cache[i].hash == hash && cache[i].size == size && cache[i].bg == bg)
            return &cache[i];
    return NULL;
}

/* A slot with memory for a size x size picture. The least recently drawn
 * pictures go until it fits in ART_BUDGET and malloc has the memory (each
 * round forgets one, so this ends). NULL: not even an empty cache helps. */
static struct art_slot *new_slot(int size)
{
    size_t need = bytes_of(size);
    for (;;) {
        struct art_slot *empty = NULL, *old = NULL;
        for (int i = 0; i < ART_CACHE; i++) {
            struct art_slot *c = &cache[i];
            if (!c->px)
                empty = empty ? empty : c;
            else if (!old || c->used < old->used)
                old = c;
        }
        if (empty && (kept + need <= ART_BUDGET || !kept) && (empty->px = malloc(need))) {
            kept += need;
            empty->size = size;
            return empty;
        }
        if (!old)
            return NULL;
        forget(old);
    }
}

/* Draw kind (the label, or a cover image) of hash into o; false if a cover
 * is gone meanwhile (then o is as it was). */
static bool render(const struct surf *o, uint64_t hash, int kind, uint32_t bg)
{
    if (kind == COVER_NONE) {
        label(o, 0, 0, o->w, hash, bg);
        return true;
    }
    return cover_render(o, hash, kind, bg);
}

void art_cover(const struct surf *s, int x, int y, int size, uint64_t hash, const char *path,
               uint32_t bg)
{
    if (size < 4)
        return;
    int kind = path ? cover_ready(hash, path, size, false) : COVER_NONE;
    if (between)
        between();
    struct art_slot *c = lookup(hash, size, bg);
    if (c && c->kind != kind) {
        struct surf o = { c->px, size, size, size };
        if (render(&o, hash, kind, bg)) {
            c->kind = kind;
        } else {
            forget(c);
            c = NULL;
        }
    } else if (!c && (c = new_slot(size)) != NULL) {
        struct surf o = { c->px, size, size, size };
        if (render(&o, hash, kind, bg)) {
            c->hash = hash;
            c->bg = bg;
            c->kind = kind;
        } else {
            forget(c);
            c = NULL;
        }
    }
    if (!c) {
        label(s, x, y, size, hash, bg);   /* not kept: drawn straight */
        return;
    }
    c->used = ++draws;
    struct surf src = { c->px, size, size, size };
    blit(s, x, y, &src, 0, 0, size, size);
}

void art_draw(const struct surf *s, int x, int y, int size, uint64_t hash, uint32_t bg)
{
    art_cover(s, x, y, size, hash, NULL, bg);
}

void art_mark(const struct surf *s, int x, int y, int size)
{
    /* docs/logo/jamos-mark.svg: a 256 box, circles of radius 38. */
    static const struct { float x, y; uint32_t c; } drupe[7] = {
        { 128.0f, 128.0f, C_BERRY0 }, { 128.0f, 48.0f, C_BERRY1 }, { 197.282f, 88.0f, C_BERRY2 },
        { 197.282f, 168.0f, C_GOLD }, { 128.0f, 208.0f, C_BERRY2 }, { 58.718f, 168.0f, C_BERRY1 },
        { 58.718f, 88.0f, C_BERRY2 },
    };
    float k = (float)size / 256.0f;
    for (int i = 0; i < 7; i++)
        disc_aa(s, (float)x + drupe[i].x * k, (float)y + drupe[i].y * k, 38.0f * k, drupe[i].c,
                255);
}
