/* gptcheck: the partition table library (<jam/parttable.h>,
 * drivers/lib/parttable*.c, the same files) on the Mac, built with ASan
 * and UBSan. `make check` runs it on tools/mkgpt.py's cases.
 *
 *     gptcheck <dir> [--mutations N] [--fuzz N] [--seed S]
 *
 * <dir> holds mkgpt.py --cases's images and their .expect files. Four
 * parts, each failing the run (exit 1, a line saying what) on the first
 * mistake:
 *   - the cases: each image's table against this file's expectations
 *     (scheme, both headers' verdicts, hybrid, blank, the kind) and
 *     against its .expect (every listed partition, exactly: mkgpt.py's own
 *     reading of the rules, in Python), with the caller's buffer at
 *     several sizes, which must all give the same table;
 *   - each header rule broken alone (gptmutate.c's edits), on either
 *     header of the Windows-like disk at 512 and 4096 bytes a block, every
 *     CRC made right after, so that rule alone must refuse the header;
 *   - mutations (gptmutate.c): N per case (default 150), seeded and so the
 *     same every run: fields of the MBR, the headers and the entries set
 *     to edge values, CRCs mostly made right afterwards so the checks
 *     behind them are reached, the disk's size and block size changed,
 *     reads failed;
 *   - the fuzz pass: N (default 5000) runs of up to 8 mutations at once
 *     on random cases, from --seed (a long pass: --fuzz 1000000).
 * After every read, mutated or not: the library asked only for blocks
 * inside the disk and no more than the buffer held (exactly that big, so
 * ASan sees a byte past it), the table it made lists partitions inside
 * the disk (past block 0 unless it is the whole-disk case, inside GPT's
 * usable range), none overlapping another, at most PT_MAX_PARTS, with
 * names that are UTF-8 without control characters; two buffer sizes gave
 * the same table; and a second reading here (the oracle_* functions)
 * agrees: every header the library called good passes each rule as this
 * file reads it, the listing is what its entries give, and the blank
 * verdict is what the disk's ends hold. ASan and UBSan catch any read out
 * of bounds, overflow or undefined shift. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jam/parttable.h>
#include "gptcheck.h"
#include "parttable_int.h"

static struct img imgs[64];
static struct geo geos[64];
static int nimgs;
uint64_t rng = 0x9e3779b97f4a7c15ull;
const char *where = "";   /* what is being checked, for failures */

void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gptcheck: FAIL %s: ", where);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

uint64_t below(uint64_t n)
{
    return n ? rnd() % n : 0;
}

/* ---- the disk ------------------------------------------------------------------------- */

void read_bytes(struct img *m, uint64_t off, uint64_t len, uint8_t *dst)
{
    while (len) {
        uint64_t n;
        if (off < m->hlen) {
            n = len < m->hlen - off ? len : m->hlen - off;
            memcpy(dst, m->head + off, n);
        } else if (off >= m->toff && off < m->bytes) {
            n = len < m->bytes - off ? len : m->bytes - off;
            memcpy(dst, m->tail + (off - m->toff), n);
        } else if (off < m->toff) {
            n = len < m->toff - off ? len : m->toff - off;
            if (pread(m->fd, dst, n, (off_t)off) != (ssize_t)n)
                fail("pread: %s", strerror(errno));
        } else {
            n = len;
            memset(dst, 0, n);
        }
        off += n;
        dst += n;
        len -= n;
    }
}

/* Blocks of the disk as the library is told it is (m->bs). */
void read_blocks(struct img *m, uint64_t lba, uint64_t count, uint8_t *dst)
{
    uint64_t off;
    if (__builtin_mul_overflow(lba, (uint64_t)m->bs, &off))
        off = UINT64_MAX - count * m->bs;   /* far past the file: zeros */
    read_bytes(m, off, count * m->bs, dst);
}

static status_t disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    struct img *m = ctx;
    if (lba > m->blocks || count > m->blocks - lba || !count)
        fail("read of %u blocks at %llu on a disk of %llu", count, (unsigned long long)lba,
             (unsigned long long)m->blocks);
    if ((uint64_t)count * m->bs > m->buf_size)
        fail("read of %u blocks into a buffer of %u bytes", count, m->buf_size);
    if (++m->reads > READS_MAX)
        fail("more than %d reads", READS_MAX);
    if (m->reads == m->fail_at) {
        m->failed = true;
        return ERR_IO;
    }
    read_blocks(m, lba, count, buf);
    return OK;
}

static status_t run(struct img *m, uint32_t buf_size, struct pt_table *t)
{
    /* Exactly buf_size bytes, so ASan sees a read past them; filled with
     * noise, as the library must not rely on what was there. */
    uint8_t *buf = malloc(buf_size);
    if (!buf)
        fail("out of memory");
    memset(buf, 0xa5, buf_size);
    m->reads = 0;
    m->failed = false;
    m->buf_size = buf_size;
    struct pt_disk d = { .block_size = m->bs, .blocks = m->blocks, .read = disk_read, .ctx = m,
                         .buf = buf, .buf_size = buf_size };
    status_t st = pt_read(&d, t);
    free(buf);
    return st;
}

/* ---- what every table must be --------------------------------------------------------- */

static void check_name(const char *s)
{
    size_t n = strnlen(s, PT_NAME_MAX + 1);
    if (n > PT_NAME_MAX)
        fail("a name without its NUL");
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)s[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 :
                   (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!k || i + k > n)
            fail("a name that isn't UTF-8 (byte %zu)", i);
        if (c < 0x20 || c == 0x7f)
            fail("a control character in a name");
        for (size_t j = 1; j < k; j++)
            if (((uint8_t)s[i + j] & 0xc0) != 0x80)
                fail("a name that isn't UTF-8 (byte %zu)", i + j);
        i += k;
    }
}

static void check_part(const struct pt_table *t, const struct pt_part *p, uint64_t blocks)
{
    if (!p->blocks || p->start >= blocks || p->blocks > blocks - p->start)
        fail("partition %u (%llu + %llu) outside the disk of %llu", p->entry,
             (unsigned long long)p->start, (unsigned long long)p->blocks,
             (unsigned long long)blocks);
    bool whole = t->scheme == PT_SCHEME_NONE && t->nparts == 1 && p->start == 0 &&
                 p->blocks == blocks && p->mbr_type == PT_WHOLE;
    if (!p->start && !whole)
        fail("partition %u starts at block 0", p->entry);
    if (t->scheme == PT_SCHEME_GPT &&
        (p->start < t->first_usable || p->start + p->blocks - 1 > t->last_usable))
        fail("partition %u outside the usable range", p->entry);
    if (t->scheme != PT_SCHEME_GPT && (!pt_guid_zero(p->type_guid) || p->attributes || p->name[0]))
        fail("GPT fields on a partition of an MBR");
    check_name(p->name);
}

static void check_table(const struct pt_table *t, uint64_t blocks)
{
    if (t->nparts > PT_MAX_PARTS || t->nnotes > PT_MAX_NOTES || t->nnotes > t->notes_total)
        fail("counts out of range");
    if (t->scheme == PT_SCHEME_GPT && t->nparts &&
        (t->first_usable <= 1 || t->last_usable >= blocks - 1 || t->first_usable > t->last_usable))
        fail("a usable range that covers a header");
    if (t->scheme == PT_SCHEME_GPT && t->nparts && t->primary && t->backup)
        fail("partitions listed with neither header good");
    if (t->scheme == PT_SCHEME_NONE && t->nparts > 1)
        fail("several partitions with no table");
    if ((t->blank || t->blank_in_progress) && (t->nparts || t->scheme != PT_SCHEME_NONE))
        fail("a blank disk with a table");
    if (pt_kind(t) == PT_KIND_JAMOS && (t->primary || t->backup || t->nparts != 2))
        fail("Jam OS's kind without its mark");
    for (uint32_t i = 0; i < t->nparts; i++) {
        check_part(t, &t->parts[i], blocks);
        for (uint32_t j = 0; j < i; j++) {
            const struct pt_part *a = &t->parts[i], *b = &t->parts[j];
            if (a->start < b->start + b->blocks && b->start < a->start + a->blocks)
                fail("partitions %u and %u overlap", a->entry, b->entry);
        }
    }
}

/* ---- a second reading: what the library calls good must be good ----------------------- */

typedef unsigned __int128 u128;   /* sums of block numbers here can't wrap */

/* A GPT header, as this file reads it. */
struct ohdr {
    uint64_t first, last, arr, nblk;
    uint32_t count, size, crc;
    uint8_t  guid[16];
};

static uint8_t obuf[PT_ARRAY_MAX + 4096];   /* an entry array, or the blank rule's ends */

/* The header at block lba, which the library called good: each of the
 * rules checked again, with this file's own reading of them. */
static void oracle_header(struct img *m, uint64_t lba, uint64_t other, struct ohdr *h)
{
    static uint8_t b[4096];
    u128 end = m->blocks - 1;
    read_blocks(m, lba, 1, b);
    uint32_t hs = get32(b + 12), crc = get32(b + 16);
    put32(b + 16, 0);
    if (memcmp(b, "EFI PART", 8) || get32(b + 8) != 0x00010000 || hs < 92 || hs > m->bs ||
        get32(b + 20) || pt_crc32(0, b, hs) != crc)
        fail("header at %llu called good: signature, revision, size, reserved or CRC",
             (unsigned long long)lba);
    *h = (struct ohdr){ .first = get64(b + 40), .last = get64(b + 48), .arr = get64(b + 72),
                        .count = get32(b + 80), .size = get32(b + 84), .crc = get32(b + 88) };
    memcpy(h->guid, b + 56, 16);
    if (get64(b + 24) != lba || get64(b + 32) != other)
        fail("header at %llu called good: MyLBA or AlternateLBA", (unsigned long long)lba);
    if (h->first < 2 || h->first > h->last || h->last >= end)
        fail("header at %llu called good: usable range", (unsigned long long)lba);
    u128 bytes = (u128)h->count * h->size;
    if (h->count < 128 || h->count > 1024 || h->size < 128 || (h->size & (h->size - 1)) ||
        bytes > PT_ARRAY_MAX)
        fail("header at %llu called good: entries", (unsigned long long)lba);
    h->nblk = (uint64_t)((bytes + m->bs - 1) / m->bs);
    u128 a0 = h->arr, a1 = (u128)h->arr + h->nblk - 1;   /* the array's first and last block */
    if (a0 < 2 || a1 > end - 1 || !(a1 < h->first || a0 > h->last))
        fail("header at %llu called good: entry array's place", (unsigned long long)lba);
    read_blocks(m, h->arr, h->nblk, obuf);
    if (pt_crc32(0, obuf, (size_t)bytes) != h->crc)
        fail("header at %llu called good: entry array's CRC", (unsigned long long)lba);
}

static bool oracle_agree(const struct ohdr *p, const struct ohdr *b)
{
    return !memcmp(p->guid, b->guid, 16) && p->first == b->first && p->last == b->last &&
           p->count == b->count && p->size == b->size && p->crc == b->crc;
}

/* Entry i left out (the n-th), with reason why: as the table's notes say? */
static void oracle_note(const struct pt_table *t, uint32_t n, uint32_t i, uint8_t why)
{
    if (n < PT_MAX_NOTES && (t->notes[n].entry != i || t->notes[n].why != why))
        fail("note %u: entry %u, %s; expected entry %u, %s", n, t->notes[n].entry,
             pt_note_str(t->notes[n].why), i, pt_note_str(why));
}

/* One used entry, listed (the n-th) or left out: as the table says? */
static void oracle_entry(const struct pt_table *t, uint32_t i, const uint8_t *e, uint32_t *n,
                         uint32_t *left, uint8_t why)
{
    uint64_t start = get64(e + 32), end = get64(e + 40);
    if (why) {
        oracle_note(t, (*left)++, i, why);
        return;
    }
    const struct pt_part *p = &t->parts[*n];
    if (*n >= t->nparts || p->entry != i || p->start != start || p->blocks != end - start + 1 ||
        memcmp(p->type_guid, e, 16) || memcmp(p->unique_guid, e + 16, 16) ||
        p->attributes != get64(e + 48))
        fail("listed partition %u isn't entry %u", *n, i);
    (*n)++;
}

/* The listing from header h's entry array (in obuf), by the rules. */
static void oracle_entries(const struct ohdr *h, const struct pt_table *t)
{
    uint64_t s[PT_MAX_PARTS], l[PT_MAX_PARTS];   /* listed: first and last block */
    uint32_t n = 0, used = 0, left = 0;
    for (uint32_t i = 0; i < h->count; i++) {
        const uint8_t *e = obuf + (size_t)i * h->size;
        if (!memcmp(e, (const uint8_t[16]){ 0 }, 16))
            continue;
        used++;
        uint64_t start = get64(e + 32), end = get64(e + 40);
        bool over = false;
        for (uint32_t j = 0; j < n; j++)
            over |= start <= l[j] && s[j] <= end;
        uint8_t why = !(h->first <= start && start <= end && end <= h->last) ? PT_NOTE_OUTSIDE :
                      over ? PT_NOTE_OVERLAP : n == PT_MAX_PARTS ? PT_NOTE_TOO_MANY : 0;
        if (!why) {
            s[n] = start;
            l[n] = end;
        }
        oracle_entry(t, i, e, &n, &left, why);
    }
    if (n != t->nparts || used != t->used || left != t->notes_total)
        fail("listed %u, used %u, left out %u; expected %u, %u, %u", t->nparts, t->used,
             t->notes_total, n, used, left);
}

/* An MBR's listing (block 0 in b), by usb-storage's rules. */
static void oracle_mbr(const struct img *m, const uint8_t *b, const struct pt_table *t)
{
    uint64_t s[4], l[4];
    uint32_t n = 0, used = 0, left = 0;
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t *e = b + 446 + 16 * i;
        uint64_t start = get32(e + 8), count = get32(e + 12);
        if (!e[4] || !count)
            continue;
        used++;
        bool over = false;
        for (uint32_t j = 0; j < n; j++)
            over |= start <= l[j] && s[j] <= start + count - 1;
        uint8_t why = !start || (u128)start + count > m->blocks ? PT_NOTE_OUTSIDE :
                      over ? PT_NOTE_OVERLAP : 0;
        if (why) {
            oracle_note(t, left++, i, why);
            continue;
        }
        s[n] = start;
        l[n] = start + count - 1;
        const struct pt_part *p = &t->parts[n];
        if (n >= t->nparts || p->entry != i || p->start != start || p->blocks != count ||
            p->mbr_type != e[4])
            fail("listed partition %u isn't MBR entry %u", n, i);
        n++;
    }
    if (n != t->nparts || used != t->used || left != t->notes_total || t->mbr_id != get32(b + 440))
        fail("MBR: listed %u, used %u, left out %u; expected %u, %u, %u", t->nparts, t->used,
             t->notes_total, n, used, left);
}

/* The blank rule, read here: block 0 and both ends all zeros. */
static bool oracle_blank(struct img *m)
{
    uint64_t r = 65536 / m->bs, n = m->blocks <= 2 * r ? m->blocks : r;
    bool zero = true;
    for (int k = 0; k < (m->blocks <= 2 * r ? 1 : 2); k++) {
        read_blocks(m, k ? m->blocks - r : 0, n, obuf);
        for (uint64_t i = 0; i < n * m->bs; i++)
            zero = zero && !obuf[i];
    }
    return zero;
}

/* What the library said, against this file's reading of the same disk. */
static void oracle(struct img *m, const struct pt_table *t)
{
    static uint8_t b0[4096];
    uint64_t end = m->blocks - 1;
    struct ohdr p, b;
    if (t->backup == PT_BAD_NONE || t->backup == PT_BAD_MISMATCH)
        oracle_header(m, end, 1, &b);
    if (t->primary == PT_BAD_NONE)
        oracle_header(m, 1, end, &p);   /* last: obuf holds its array */
    bool both = t->primary == PT_BAD_NONE && (t->backup == PT_BAD_NONE ||
                                              t->backup == PT_BAD_MISMATCH);
    if (both && oracle_agree(&p, &b) != (t->backup == PT_BAD_NONE))
        fail("the backup %s the primary, but is called %s", oracle_agree(&p, &b) ?
             "agrees with" : "disagrees with", pt_bad_str(t->backup));
    if (t->scheme == PT_SCHEME_GPT && t->primary != PT_BAD_NONE && t->backup == PT_BAD_NONE)
        read_blocks(m, b.arr, b.nblk, obuf);
    if (t->scheme == PT_SCHEME_GPT && (t->primary == PT_BAD_NONE || t->backup == PT_BAD_NONE))
        oracle_entries(t->primary == PT_BAD_NONE ? &p : &b, t);
    read_blocks(m, 0, 1, b0);
    if (t->scheme == PT_SCHEME_MBR)
        oracle_mbr(m, b0, t);
    if (!m->failed && t->blank != oracle_blank(m))
        fail("blank %d, but the disk's ends say otherwise", t->blank);
}

static bool part_eq(const struct pt_part *a, const struct pt_part *b)
{
    return a->start == b->start && a->blocks == b->blocks && a->attributes == b->attributes &&
           !memcmp(a->type_guid, b->type_guid, 16) && !memcmp(a->unique_guid, b->unique_guid, 16) &&
           a->entry == b->entry && a->mbr_type == b->mbr_type && !strcmp(a->name, b->name);
}

/* The same tables, field by field (padding aside). */
static bool table_eq(const struct pt_table *a, const struct pt_table *b)
{
    if (a->scheme != b->scheme || a->primary != b->primary || a->backup != b->backup ||
        a->hybrid != b->hybrid || a->blank != b->blank ||
        a->blank_in_progress != b->blank_in_progress || a->mbr_id != b->mbr_id ||
        memcmp(a->disk_guid, b->disk_guid, 16) || a->first_usable != b->first_usable ||
        a->last_usable != b->last_usable || a->entry_count != b->entry_count ||
        a->entry_size != b->entry_size || a->used != b->used || a->nparts != b->nparts ||
        a->notes_total != b->notes_total || a->nnotes != b->nnotes)
        return false;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (!part_eq(&a->parts[i], &b->parts[i]))
            return false;
    for (uint32_t i = 0; i < a->nnotes; i++)
        if (a->notes[i].entry != b->notes[i].entry || a->notes[i].why != b->notes[i].why)
            return false;
    return true;
}

/* Read m with two buffer sizes; both tables must pass and be the same. */
status_t read_checked(struct img *m, struct pt_table *t)
{
    static struct pt_table t2;
    static const uint32_t sizes[] = { 1, 2, 3, 7, 16, 32, 48 };   /* in blocks */
    uint32_t a = sizes[below(7)] * m->bs, b = sizes[below(7)] * m->bs;
    if (a > BUF_MAX)
        a = m->bs;
    if (b > BUF_MAX)
        b = BUF_MAX;
    memset(t, 0x5a, sizeof(*t));
    status_t st = run(m, a, t);
    uint32_t fail_at = m->fail_at;
    if (st == OK) {
        check_table(t, m->blocks);
        oracle(m, t);
    } else if (t->nparts || t->scheme != PT_SCHEME_NONE)
        fail("status %d with a table", st);
    if (fail_at)
        return st;   /* the failing read's index depends on the buffer size */
    memset(&t2, 0xa5, sizeof(t2));
    status_t st2 = run(m, b, &t2);
    if (st2 != st)
        fail("buffers of %u and %u bytes: status %d and %d", a, b, st, st2);
    if (st == OK && !table_eq(t, &t2))
        fail("buffers of %u and %u bytes gave different tables", a, b);
    return st;
}

/* ---- the cases ------------------------------------------------------------------------ */

/* What a case must give (tools/mkgpt.py's CASES says how each is made). */
struct want {
    const char *name;
    uint8_t  scheme, primary, backup;
    bool     hybrid, blank, in_progress;
    uint8_t  kind;
    uint32_t used, notes;    /* used entries; entries left out */
};

#define GPT_OK(n, u, k) { n, PT_SCHEME_GPT, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 0, k, u, 0 }
enum { N_ = PT_SCHEME_NONE, M_ = PT_SCHEME_MBR, G_ = PT_SCHEME_GPT, OT = PT_KIND_OTHER,
       BL = PT_KIND_BLANK, JA = PT_KIND_JAMOS };

static const struct want wants[] = {
    GPT_OK("windows", 4, OT),
    GPT_OK("windows-4k", 4, OT),
    { "primary-crc", G_, PT_BAD_CRC, PT_BAD_NONE, 0, 0, 0, OT, 4, 0 },
    { "backup-crc", G_, PT_BAD_NONE, PT_BAD_CRC, 0, 0, 0, OT, 4, 0 },
    { "primary-array", G_, PT_BAD_ARRAY_CRC, PT_BAD_NONE, 0, 0, 0, OT, 4, 0 },
    { "backup-array", G_, PT_BAD_NONE, PT_BAD_ARRAY_CRC, 0, 0, 0, OT, 4, 0 },
    { "both-arrays", G_, PT_BAD_ARRAY_CRC, PT_BAD_ARRAY_CRC, 0, 0, 0, OT, 0, 0 },
    { "both-crc", G_, PT_BAD_CRC, PT_BAD_CRC, 0, 0, 0, OT, 0, 0 },
    { "disagree", G_, PT_BAD_NONE, PT_BAD_MISMATCH, 0, 0, 0, OT, 4, 0 },
    { "primary-zero", G_, PT_BAD_ZERO, PT_BAD_NONE, 0, 0, 0, OT, 4, 0 },
    { "outside", G_, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 0, OT, 7, 4 },
    { "overlap", G_, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 0, OT, 5, 3 },
    GPT_OK("entries-129", 2, OT),
    GPT_OK("entries-1024", 2, OT),
    { "entries-127", G_, PT_BAD_ENTRIES, PT_BAD_ENTRIES, 0, 0, 0, OT, 0, 0 },
    GPT_OK("entry-size-256", 2, OT),
    GPT_OK("entry-size-1024", 2, OT),
    GPT_OK("entries-1024-4k", 2, OT),
    { "too-many", G_, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 0, OT, 20, 4 },
    { "hybrid", G_, PT_BAD_NONE, PT_BAD_NONE, 1, 0, 0, OT, 4, 0 },
    { "no-pmbr", M_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 0, 0, OT, 1, 0 },
    { "zero-mbr", N_, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 0, OT, 0, 0 },
    GPT_OK("odd-names", 11, OT),
    GPT_OK("empty", 0, OT),
    { "blank", N_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 1, 0, BL, 0, 0 },
    { "blank-4k", N_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 1, 0, BL, 0, 0 },
    { "almost-blank-head", N_, PT_BAD_ZERO, PT_BAD_ZERO, 0, 0, 0, OT, 0, 0 },
    { "almost-blank-tail", N_, PT_BAD_ZERO, PT_BAD_ZERO, 0, 0, 0, OT, 0, 0 },
    { "almost-blank-edge", N_, PT_BAD_ZERO, PT_BAD_ZERO, 0, 0, 0, OT, 0, 0 },
    GPT_OK("jamos", 2, JA),
    GPT_OK("jamos-4k", 2, JA),
    { "jamos-backup-crc", G_, PT_BAD_NONE, PT_BAD_CRC, 0, 0, 0, OT, 2, 0 },
    GPT_OK("jamos-extra", 3, OT),
    { "jamos-hybrid", G_, PT_BAD_NONE, PT_BAD_NONE, 1, 0, 0, OT, 2, 0 },
    GPT_OK("jamos-name", 2, OT),
    { "jamos-cut-mbr", N_, PT_BAD_NONE, PT_BAD_NONE, 0, 0, 1, BL, 0, 0 },
    { "jamos-cut-primary", N_, PT_BAD_ZERO, PT_BAD_NONE, 0, 0, 1, BL, 0, 0 },
    { "jamos-cut-bad", N_, PT_BAD_CRC, PT_BAD_NONE, 0, 0, 0, OT, 0, 0 },
    { "jamos-cut-backup-bad", N_, PT_BAD_NONE, PT_BAD_CRC, 0, 0, 0, OT, 0, 0 },
    GPT_OK("pmbr-status", 4, OT),
    { "mbr", M_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 0, 0, OT, 2, 0 },
    { "mbr-bad", M_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 0, 0, OT, 4, 3 },
    { "grown", G_, PT_BAD_ALT_LBA, PT_BAD_ZERO, 0, 0, 0, OT, 0, 0 },
    { "superfloppy", N_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 0, 0, OT, 0, 0 },
    { "superfloppy-junk", N_, PT_BAD_ABSENT, PT_BAD_ABSENT, 0, 0, 0, OT, 0, 0 },
};
#define NWANTS (sizeof(wants) / sizeof(wants[0]))

static void load(struct img *m, const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.img", dir, name);
    snprintf(m->name, sizeof(m->name), "%s", name);
    if ((m->fd = open(path, O_RDONLY)) < 0)
        fail("%s: %s (run tools/mkgpt.py --cases first)", path, strerror(errno));
    off_t end = lseek(m->fd, 0, SEEK_END);
    if (end <= 0)
        fail("%s: empty", path);
    m->bytes = (uint64_t)end;
    m->hlen = m->bytes <= 2 * HEAD_MAX ? m->bytes : HEAD_MAX;
    m->toff = m->bytes <= 2 * HEAD_MAX ? m->bytes : m->bytes - HEAD_MAX;
    m->head0 = malloc(m->hlen);
    m->tail0 = malloc(m->bytes - m->toff + 1);
    m->head = malloc(m->hlen);
    m->tail = malloc(m->bytes - m->toff + 1);
    if (pread(m->fd, m->head0, m->hlen, 0) != (ssize_t)m->hlen ||
        pread(m->fd, m->tail0, m->bytes - m->toff, (off_t)m->toff) != (ssize_t)(m->bytes - m->toff))
        fail("%s: short read", path);
    memcpy(m->head, m->head0, m->hlen);
    memcpy(m->tail, m->tail0, m->bytes - m->toff);
    m->bs0 = !memcmp(m->head0 + 4096, "EFI PART", 8) || strstr(name, "-4k") ? 4096 : 512;
    m->bs = m->bs0;
    m->blocks = m->bytes / m->bs0;
}

/* The listing against the .expect file, line by line. */
static void check_expect(const char *dir, const struct pt_table *t)
{
    char path[512], line[1024], want[1024], name[2 * PT_NAME_MAX + 2], tg[37], ug[37];
    snprintf(path, sizeof(path), "%s/%s.expect", dir, where);
    FILE *f = fopen(path, "r");
    if (!f)
        fail("%s: %s", path, strerror(errno));
    uint32_t i = 0;
    for (; fgets(line, sizeof(line), f); i++) {
        if (i >= t->nparts)
            fail("lists %u partitions, %s more", t->nparts, path);
        const struct pt_part *p = &t->parts[i];
        int o = 0;
        for (const char *c = p->name; *c; c++)
            o += snprintf(name + o, sizeof(name) - (size_t)o, "%02x", (uint8_t)*c);
        pt_guid_str(p->type_guid, tg);
        pt_guid_str(p->unique_guid, ug);
        snprintf(want, sizeof(want), "part %u %llu %llu %u %s %s %llu %s\n", p->entry,
                 (unsigned long long)p->start, (unsigned long long)p->blocks, p->mbr_type, tg, ug,
                 (unsigned long long)p->attributes, p->name[0] ? name : "-");
        if (strcmp(line, want))
            fail("partition %u:\n  listed   %s  expected %s", i, want, line);
    }
    fclose(f);
    if (i != t->nparts)
        fail("lists %u partitions, %s %u", t->nparts, path, i);
}

static void check_case(const char *dir, struct img *m, const struct want *w)
{
    static struct pt_table t;
    where = w->name;
    m->fail_at = 0;
    for (int k = 0; k < 4; k++)   /* different buffer sizes each time */
        if (read_checked(m, &t) != OK)
            fail("pt_read failed");
    if (t.scheme != w->scheme || t.primary != w->primary || t.backup != w->backup ||
        t.hybrid != w->hybrid || t.blank != w->blank || t.blank_in_progress != w->in_progress ||
        pt_kind(&t) != w->kind || t.used != w->used || t.notes_total != w->notes)
        fail("scheme %u primary %s, backup %s, hybrid %d blank %d in-progress %d kind %d used "
             "%u notes %u", t.scheme, pt_bad_str(t.primary), pt_bad_str(t.backup), t.hybrid,
             t.blank, t.blank_in_progress, pt_kind(&t), t.used, t.notes_total);
    if ((t.scheme == PT_SCHEME_MBR) != (t.mbr_id == 0x4A414D21 ||
                                        (t.mbr_id == 0 && !strcmp(w->name, "no-pmbr"))))
        fail("MBR disk id %08x", t.mbr_id);
    check_expect(dir, &t);
}

/* ---- the pieces alone ----------------------------------------------------------------- */

static void check_units(void)
{
    where = "units";
    if (pt_crc32(0, "123456789", 9) != 0xcbf43926 ||
        pt_crc32(pt_crc32(0, "1234", 4), "56789", 5) != 0xcbf43926)
        fail("CRC32 of \"123456789\"");
    char s[37];
    pt_guid_str(pt_type_esp, s);
    if (strcmp(s, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"))
        fail("the ESP's GUID printed as %s", s);
    pt_guid_str(pt_type_jamos_data, s);
    if (strcmp(s, "71BAE565-D4A8-4315-8C81-9B35462CC158"))
        fail("Jam OS's data GUID printed as %s", s);
    for (int i = 0; i < PT_BAD_COUNT + 1; i++)
        if (!pt_bad_str((uint8_t)i)[0])
            fail("no words for reason %d", i);
    /* Disks pt_read must refuse without a read. */
    static uint8_t buf[4096];
    static struct pt_table t;
    struct img m = { .bs = 512, .blocks = 100 };
    struct pt_disk bad[] = {
        { 1000, 100, disk_read, &m, buf, 4096 }, { 256, 100, disk_read, &m, buf, 4096 },
        { 8192, 100, disk_read, &m, buf, 8192 }, { 512, 0, disk_read, &m, buf, 4096 },
        { 512, 100, NULL, &m, buf, 4096 }, { 512, 100, disk_read, &m, NULL, 4096 },
        { 512, 100, disk_read, &m, buf, 511 }, { 4096, 100, disk_read, &m, buf, 4095 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (pt_read(&bad[i], &t) != ERR_INVALID_ARGS || t.nparts || m.reads)
            fail("bad disk %u accepted", i);
    /* The library's reader refuses runs outside the disk, whatever asks. */
    struct pt_disk d = { 512, 100, disk_read, &m, buf, 4096 };
    if (pt_stream(&d, 100, 1, NULL, NULL) != ERR_OUT_OF_RANGE ||
        pt_stream(&d, 1, UINT64_MAX, NULL, NULL) != ERR_OUT_OF_RANGE ||
        pt_stream(&d, UINT64_MAX, 2, NULL, NULL) != ERR_OUT_OF_RANGE || m.reads)
        fail("pt_stream read outside the disk");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gptcheck <dir> [--mutations N] [--fuzz N] [--seed S]\n");
        return 2;
    }
    long mutations = 150, fuzz = 5000;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mutations"))
            mutations = strtol(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz"))
            fuzz = strtol(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--seed"))
            rng = strtoull(argv[i + 1], NULL, 0) | 1;
    }
    check_units();
    for (unsigned i = 0; i < NWANTS; i++) {
        load(&imgs[nimgs], argv[1], wants[i].name);
        find_geo(&imgs[nimgs], &geos[nimgs]);
        check_case(argv[1], &imgs[nimgs++], &wants[i]);
    }
    printf("gptcheck: %u cases ok\n", (unsigned)NWANTS);
    unsigned edits = check_edits(imgs, geos);   /* imgs[0] and [1] are the Windows-like ones */
    printf("gptcheck: %u header rules, each broken alone on both block sizes and headers, ok\n",
           edits);
    struct tally y = { 0 };
    for (int i = 0; i < nimgs; i++) {
        where = imgs[i].name;
        for (long k = 0; k < mutations; k++)
            mutated_run(&imgs[i], &geos[i], 1 + (int)below(3), &y);
    }
    printf("gptcheck: %llu mutated tables ok (GPT %llu, MBR %llu, none %llu; partitions listed "
           "%llu; read failed %llu)\n", (unsigned long long)y.runs, (unsigned long long)y.gpt,
           (unsigned long long)y.mbr, (unsigned long long)y.none, (unsigned long long)y.listed,
           (unsigned long long)y.failed);
    y = (struct tally){ 0 };
    for (long k = 0; k < fuzz; k++) {
        int i = (int)below((uint64_t)nimgs);
        where = imgs[i].name;
        mutated_run(&imgs[i], &geos[i], 1 + (int)below(8), &y);
    }
    printf("gptcheck: fuzz: %llu runs ok (GPT %llu, MBR %llu, none %llu; partitions listed "
           "%llu; read failed %llu)\n", (unsigned long long)y.runs, (unsigned long long)y.gpt,
           (unsigned long long)y.mbr, (unsigned long long)y.none, (unsigned long long)y.listed,
           (unsigned long long)y.failed);
    return 0;
}
