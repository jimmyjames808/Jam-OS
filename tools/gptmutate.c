/* gptcheck's mutations: the MBR's entries, the GPT headers' fields and the
 * entries set to edge values, with CRCs mostly made right afterwards so
 * the checks behind them are reached; the disk's size and block size
 * changed; reads failed. Each run changes the image's ends in memory and
 * puts them back after (restore). And each header rule broken alone on
 * the Windows-like disks (check_edits). gptcheck.c has the model. */
#include <stdio.h>
#include <string.h>
#include <jam/parttable.h>
#include "gptcheck.h"

static uint8_t nbuf[PT_ARRAY_MAX + 4096];   /* an entry array, read to fix its CRC */

/* ---- the image's ends, changed and put back --------------------------------------- */

static uint8_t *dirty_ptr[256];
static uint64_t dirty_len[256];
static int ndirty;
static bool dirty_all;

/* A pointer to bytes [off, off + len) of m's file in memory (head or
 * tail), noted to be put back; NULL if they aren't in memory. */
static uint8_t *at(struct img *m, uint64_t off, uint64_t len)
{
    uint8_t *p = NULL;
    if (off <= m->hlen && len <= m->hlen - off)
        p = m->head + off;
    else if (off >= m->toff && off <= m->bytes && len <= m->bytes - off)
        p = m->tail + (off - m->toff);
    if (!p)
        return NULL;
    if (ndirty < 256) {
        dirty_ptr[ndirty] = p;
        dirty_len[ndirty++] = len;
    } else {
        dirty_all = true;
    }
    return p;
}

static void restore(struct img *m)
{
    if (dirty_all) {
        memcpy(m->head, m->head0, m->hlen);
        memcpy(m->tail, m->tail0, m->bytes - m->toff);
    } else {
        for (int i = ndirty - 1; i >= 0; i--) {
            uint8_t *p = dirty_ptr[i];
            const uint8_t *src = p >= m->head && p < m->head + m->hlen ? m->head0 + (p - m->head)
                                                                       : m->tail0 + (p - m->tail);
            memcpy(p, src, dirty_len[i]);
        }
    }
    ndirty = 0;
    dirty_all = false;
    m->bs = m->bs0;
    m->blocks = m->bytes / m->bs0;
    m->fail_at = 0;
}

/* An edge value for a 64-bit block number on disk m, near `was`. */
static uint64_t edge64(const struct img *m, uint64_t was)
{
    uint64_t last = m->blocks - 1;
    const uint64_t v[] = { 0, 1, 2, 3, 33, 34, last - 34, last - 33, last - 1, last, last + 1,
                           m->blocks, 0xffffffffull, 1ull << 32, 1ull << 63, UINT64_MAX,
                           UINT64_MAX - 1, was - 1, was + 1, was, rnd(), below(m->blocks + 2),
                           last - below(64), was - below(64), was + below(64) };
    return v[below(sizeof(v) / sizeof(v[0]))];
}

static uint32_t edge32(const struct img *m)
{
    const uint32_t v[] = { 0, 1, 91, 92, 93, 127, 128, 129, 255, 256, 512, 1023, 1024, 1025,
                           m->bs - 1, m->bs, m->bs + 1, 0x80000000u, 0xffffffffu, (uint32_t)rnd() };
    return v[below(sizeof(v) / sizeof(v[0]))];
}

/* The header at byte off: its CRC32 made right for its current bytes. */
static void fix_header(struct img *m, uint64_t off)
{
    uint8_t *h = at(m, off, m->bs0);
    if (!h)
        return;
    uint32_t size = get32(h + 12);
    if (size < 92 || size > m->bs0)
        size = 92;
    put32(h + 16, 0);
    put32(h + 16, pt_crc32(0, h, size));
}

/* Header k's array CRC made right for its current array, then its own. */
static void fix_array(struct img *m, const struct geo *g, int k)
{
    uint8_t *a = at(m, g->arr[k], (uint64_t)g->count * g->size);
    uint8_t *h = at(m, g->hdr[k], m->bs0);
    if (!a || !h)
        return;
    put32(h + 88, pt_crc32(0, a, (uint64_t)g->count * g->size));
    fix_header(m, g->hdr[k]);
}

/* The header at byte off: its array CRC made right for whatever array its
 * fields now name (read as the library will, at m->bs), then its own. */
static void fix_named_array(struct img *m, uint64_t off)
{
    uint8_t *h = at(m, off, m->bs0);
    if (!h)
        return;
    uint64_t bytes = (uint64_t)get32(h + 80) * get32(h + 84), at_byte;
    if (bytes && bytes <= PT_ARRAY_MAX &&
        !__builtin_mul_overflow(get64(h + 72), (uint64_t)m->bs, &at_byte)) {
        read_bytes(m, at_byte, bytes, nbuf);
        put32(h + 88, pt_crc32(0, nbuf, bytes));
    }
    fix_header(m, off);
}

static void mut_header(struct img *m, const struct geo *g)
{
    static const int fields[] = { 8, 12, 20, 24, 32, 40, 48, 56, 72, 80, 84, 88 };
    int k = (int)below(2), f = fields[below(12)];
    uint8_t *h = at(m, g->hdr[k], m->bs0);
    if (!h)
        return;
    if (f == 24 || f == 32 || f == 40 || f == 48 || f == 72)
        put64(h + f, edge64(m, get64(h + f)));
    else if (f == 56)
        h[56 + below(16)] ^= (uint8_t)(1 + below(255));
    else
        put32(h + f, edge32(m));
    if ((f == 72 || f == 80 || f == 84) && below(2))
        fix_named_array(m, g->hdr[k]);   /* the array it now names passes: on to the layout */
    else if (below(8))
        fix_header(m, g->hdr[k]);
    if (!below(4)) {   /* the same change to the other header, so they may agree */
        uint8_t *o = at(m, g->hdr[1 - k], m->bs0);
        if (o && f != 24 && f != 32) {
            memcpy(o + f, h + f, f == 56 ? 16 : f == 40 || f == 48 || f == 72 ? 8 : 4);
            fix_header(m, g->hdr[1 - k]);
        }
    }
}

static void mut_entry(struct img *m, const struct geo *g)
{
    uint32_t i = below(4) ? (uint32_t)below(8) : (uint32_t)below(g->count);
    uint64_t eoff = (uint64_t)i * g->size;
    for (int k = 0; k < 2; k++) {
        uint8_t *e = at(m, g->arr[k] + eoff, 128);
        if (!e)
            continue;
        switch (below(6)) {
        case 0:   /* start or end on an edge, or on another entry's */
        case 1: {
            uint8_t *o = at(m, g->arr[k] + below(8) * g->size, 128);
            uint64_t v = o && below(2) ? get64(o + 32 + 8 * below(2)) + below(3) - 1
                                       : edge64(m, get64(e + 32));
            put64(e + 32 + 8 * below(2), v);
            break;
        }
        case 2:   /* the type: unused, the ESP's, Jam OS's, noise */
            memcpy(e, below(3) == 0 ? (const uint8_t[16]){ 0 } : below(2) ? pt_type_esp
                                                                     : pt_type_jamos_data, 16);
            break;
        case 3:   /* name units, surrogates among them */
            for (int n = 0; n < 1 + (int)below(8); n++) {
                uint32_t u = below(3) ? 0xd800 + (uint32_t)below(0x800) : (uint32_t)below(0x10000);
                e[56 + 2 * below(36)] = (uint8_t)u;
                e[57 + 2 * below(36)] = (uint8_t)(u >> 8);
            }
            break;
        case 4:
            put64(e + 48, rnd());
            break;
        default:
            e[below(128)] ^= (uint8_t)(1 + below(255));
        }
        if (below(2))
            break;   /* only the primary's array changed */
    }
    for (int k = 0; k < 2; k++)
        if (below(8))
            fix_array(m, g, k);
}

static void mut_mbr(struct img *m)
{
    uint8_t *s = at(m, 0, 512);
    uint8_t *e = s + 446 + 16 * below(4);
    switch (below(7)) {
    case 0: e[0] = below(2) ? 0x80 : (uint8_t)rnd(); break;
    case 1: e[4] = (const uint8_t[]){ 0xee, 0x0c, 0x00, 0x05, 0xef, 0x07 }[below(6)]; break;
    case 2: put32(e + 8, (uint32_t)edge64(m, get32(e + 8))); break;
    case 3: put32(e + 12, (uint32_t)edge64(m, get32(e + 12))); break;
    case 4: s[510 + below(2)] ^= 0xff; break;
    case 5: memset(s, 0, 512); break;
    default:   /* a FAT boot sector's fields */
        s[0] = 0xeb;
        put32(s + 32, (uint32_t)edge64(m, m->blocks));
        memcpy(s + 82, "FAT32", 5);
        s[11 + below(8)] = (uint8_t)rnd();
    }
}

static void mutate(struct img *m, const struct geo *g)
{
    switch (below(g->gpt ? 9 : 5)) {
    case 0: mut_mbr(m); break;
    case 1: {   /* the disk's size */
        uint64_t n = m->bytes / m->bs;
        const uint64_t v[] = { 1, 2, 3, 4, 34, 67, 68, n - 1, n + 1, n * 2, 1ull << 32,
                               1ull << 40, UINT64_MAX / m->bs, UINT64_MAX, rnd() };
        m->blocks = v[below(sizeof(v) / sizeof(v[0]))];
        if (!m->blocks)
            m->blocks = 1;
        break;
    }
    case 2: m->bs = m->bs == 512 ? 4096 : 512; m->blocks = m->bytes / m->bs; break;
    case 3: m->fail_at = 1 + (uint32_t)below(12); break;
    case 4: {   /* noise anywhere in the first or last blocks */
        uint64_t off = below(2) ? below(m->hlen < 70000 ? m->hlen : 70000)
                                : m->bytes - 1 - below(m->bytes - m->toff);
        uint8_t *p = at(m, off, 1);
        if (p)
            *p ^= (uint8_t)(1 + below(255));
        break;
    }
    case 5: case 6: mut_header(m, g); break;
    default: mut_entry(m, g); break;
    }
}

/* Byte off of m's file as made, if it is in memory. */
static const uint8_t *orig(const struct img *m, uint64_t off, uint64_t len)
{
    if (off <= m->hlen && len <= m->hlen - off)
        return m->head0 + off;
    if (off >= m->toff && off <= m->bytes && len <= m->bytes - off)
        return m->tail0 + (off - m->toff);
    return NULL;
}

/* Where m's GPT is, from its headers as made (nothing is mutated yet). */
void find_geo(const struct img *m, struct geo *g)
{
    const uint8_t *p = orig(m, m->bs0, m->bs0), *b = orig(m, m->bytes - m->bs0, m->bs0);
    *g = (struct geo){ 0 };
    if (!p || !b || memcmp(p, "EFI PART", 8) || memcmp(b, "EFI PART", 8))
        return;
    g->gpt = true;
    g->hdr[0] = m->bs0;
    g->hdr[1] = m->bytes - m->bs0;
    g->arr[0] = get64(p + 72) * m->bs0;
    g->arr[1] = get64(b + 72) * m->bs0;
    g->count = get32(p + 80);
    g->size = get32(p + 84);
}

/* ---- each header rule broken in turn -------------------------------------------------- */

/* One header field of the Windows-like disk set to a value that breaks one
 * rule, every CRC then made right (the array's for whatever array the
 * header now names), so that rule alone must catch it. */
struct edit {
    const char *what;
    int      field;          /* byte offset in the header */
    int      width;          /* 4 or 8 */
    int      rel;            /* the value: 0 absolute, 1 from the last block, 2 from first usable */
    int64_t  value;
    uint8_t  why;            /* the verdict it must get */
};

static const struct edit edits[] = {
    { "signature", 0, 4, 0, 0x20494646, PT_BAD_SIGNATURE },
    { "revision 1.1", 8, 4, 0, 0x00010001, PT_BAD_REVISION },
    { "header size 91", 12, 4, 0, 91, PT_BAD_HEADER_SIZE },
    { "header size past the block", 12, 4, 0, -1, PT_BAD_HEADER_SIZE },
    { "reserved", 20, 4, 0, 1, PT_BAD_RESERVED },
    { "MyLBA", 24, 8, 0, 2, PT_BAD_MY_LBA },
    { "AlternateLBA", 32, 8, 1, -1, PT_BAD_ALT_LBA },
    { "first usable 1", 40, 8, 0, 1, PT_BAD_USABLE },
    { "last usable on the backup", 48, 8, 1, 0, PT_BAD_USABLE },
    { "first past last", 40, 8, 1, -1, PT_BAD_USABLE },
    { "127 entries", 80, 4, 0, 127, PT_BAD_ENTRIES },
    { "1025 entries", 80, 4, 0, 1025, PT_BAD_ENTRIES },
    { "entry size 255", 84, 4, 0, 255, PT_BAD_ENTRIES },
    { "entry size 64", 84, 4, 0, 64, PT_BAD_ENTRIES },
    { "array at block 0", 72, 8, 0, 0, PT_BAD_ARRAY },
    { "array at block 1", 72, 8, 0, 1, PT_BAD_ARRAY },
    { "array over the last block", 72, 8, 1, -2, PT_BAD_ARRAY },   /* -2: set from its size */
    { "array in the usable range", 72, 8, 2, 0, PT_BAD_ARRAY },
    { "array ending in the usable range", 72, 8, 2, -1, PT_BAD_ARRAY },
};

static void apply_edit(struct img *m, const struct geo *g, int k, const struct edit *e)
{
    uint8_t *h = at(m, g->hdr[k], m->bs0);
    uint64_t last = m->blocks - 1, nblk = ((uint64_t)g->count * g->size + m->bs - 1) / m->bs;
    uint64_t v = (uint64_t)e->value;
    if (e->rel == 1)
        v = e->value == -2 ? last + 1 - nblk : last + (uint64_t)e->value;
    else if (e->rel == 2)
        v = get64(h + 40) + (uint64_t)e->value - (e->value < 0 ? nblk - 2 : 0);
    if (e->field == 12 && e->value == -1)
        v = m->bs + 4;
    if (e->width == 8)
        put64(h + e->field, v);
    else
        put32(h + e->field, (uint32_t)v);
    if (e->field != 0)
        fix_named_array(m, g->hdr[k]);   /* the header's CRC too */
}

unsigned check_edits(struct img *m, const struct geo *g)
{
    static struct pt_table t;
    unsigned n = sizeof(edits) / sizeof(edits[0]);
    for (int i = 0; i < 2; i++)   /* "windows" and "windows-4k" */
        for (unsigned e = 0; e < n; e++)
            for (int k = 0; k < 2; k++) {
                where = edits[e].what;
                apply_edit(&m[i], &g[i], k, &edits[e]);
                if (read_checked(&m[i], &t) != OK || (k ? t.backup : t.primary) != edits[e].why ||
                    (k ? t.primary : t.backup) != PT_BAD_NONE || t.nparts != 4)
                    fail("%s %s: primary %s, backup %s, %u listed", m[i].name,
                         k ? "backup" : "primary", pt_bad_str(t.primary), pt_bad_str(t.backup),
                         t.nparts);
                restore(&m[i]);
            }
    return n;
}

void mutated_run(struct img *m, const struct geo *g, int ops, struct tally *y)
{
    static struct pt_table t;
    for (int i = 0; i < ops; i++)
        mutate(m, g);
    status_t st = read_checked(m, &t);
    y->runs++;
    if (st != OK) {
        y->failed++;
    } else {
        y->gpt += t.scheme == PT_SCHEME_GPT;
        y->mbr += t.scheme == PT_SCHEME_MBR;
        y->none += t.scheme == PT_SCHEME_NONE;
        y->listed += t.nparts > 0;
    }
    restore(m);
}
