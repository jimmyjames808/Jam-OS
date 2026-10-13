/* The partition table library's GPT reader (UEFI 2.10, section 5.3): both
 * headers, each checked alone and then against the other, their entry
 * arrays' CRCs, and the used entries of the one taken. <jam/parttable.h>
 * has the rules.
 *
 * Every number a header holds is the disk's to choose, so each is checked
 * against the disk and the others before it is used, and the checks are
 * written so nothing can wrap: an entry array is at most PT_ARRAY_MAX
 * bytes, read through the caller's buffer a piece at a time, and parsed
 * as it streams by (an entry may be split across two pieces), so its size
 * costs reads, never memory. */
#include <jam/parttable.h>
#include "parttable_int.h"

#define GPT_REVISION   0x00010000u   /* 1.0 */
#define GPT_HDR_MIN    92            /* the header's defined bytes */
#define GPT_ENTRY_MIN  128           /* an entry's defined bytes */
#define GPT_NAME_UNITS 36            /* UTF-16 units of an entry's name */

/* A header's fields (UEFI 2.10, table 5.5), once read. */
struct hdr {
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first;          /* FirstUsableLBA */
    uint64_t last;           /* LastUsableLBA */
    uint64_t array_lba;      /* PartitionEntryLBA */
    uint32_t count;          /* NumberOfPartitionEntries */
    uint32_t size;           /* SizeOfPartitionEntry */
    uint32_t array_crc;      /* PartitionEntryArrayCRC32 */
    uint8_t  guid[16];       /* DiskGUID */
};

/* ---- one header ----------------------------------------------------------------------- */

/* The header's CRC32 over its first `size` bytes, its own field as zero. */
static uint32_t header_crc(const uint8_t *b, uint32_t size)
{
    static const uint8_t zero[4];
    uint32_t crc = pt_crc32(0, b, 16);
    crc = pt_crc32(crc, zero, 4);
    return pt_crc32(crc, b + 20, size - 20);
}

/* The header's numbers against the disk (its last block `end`): the usable
 * range clear of block 0, both headers and the entry array, the entry
 * count and size, the array inside the disk and clear of the headers. */
static uint8_t header_layout(const struct hdr *h, uint32_t block_size, uint64_t end)
{
    if (h->first <= 1 || h->first > h->last || h->last >= end)
        return PT_BAD_USABLE;
    if (h->count < PT_ENTRIES_MIN || h->count > PT_ENTRIES_MAX || h->size < GPT_ENTRY_MIN ||
        (h->size & (h->size - 1)) || h->size > PT_ARRAY_MAX / h->count)
        return PT_BAD_ENTRIES;
    uint64_t blocks = ((uint64_t)h->count * h->size + block_size - 1) / block_size;
    if (h->array_lba < 2 || h->array_lba >= end || blocks > end - h->array_lba)
        return PT_BAD_ARRAY;   /* blocks [array_lba, array_lba + blocks) end before `end` */
    if (h->array_lba + blocks > h->first && h->array_lba <= h->last)
        return PT_BAD_ARRAY;   /* over the usable range */
    return PT_BAD_NONE;
}

/* Read the header at block `lba` (the other header's block: `other`) into
 * *h and check it alone; its entry array isn't read yet. */
static uint8_t header_read(const struct pt_disk *d, uint64_t lba, uint64_t other, struct hdr *h)
{
    if (pt_get(d, lba, 1) != OK)
        return PT_BAD_IO;
    const uint8_t *b = d->buf;
    static const uint8_t sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    if (pt_all_zero(b, d->block_size))
        return PT_BAD_ZERO;
    for (int i = 0; i < 8; i++)
        if (b[i] != sig[i])
            return PT_BAD_SIGNATURE;
    if (pt_le32(b + 8) != GPT_REVISION)
        return PT_BAD_REVISION;
    uint32_t size = pt_le32(b + 12);
    if (size < GPT_HDR_MIN || size > d->block_size)
        return PT_BAD_HEADER_SIZE;
    if (pt_le32(b + 16) != header_crc(b, size))
        return PT_BAD_CRC;
    if (pt_le32(b + 20))
        return PT_BAD_RESERVED;
    *h = (struct hdr){ .my_lba = pt_le64(b + 24), .alt_lba = pt_le64(b + 32),
                       .first = pt_le64(b + 40), .last = pt_le64(b + 48),
                       .array_lba = pt_le64(b + 72), .count = pt_le32(b + 80),
                       .size = pt_le32(b + 84), .array_crc = pt_le32(b + 88) };
    for (int i = 0; i < 16; i++)
        h->guid[i] = b[56 + i];
    if (h->my_lba != lba)
        return PT_BAD_MY_LBA;
    if (h->alt_lba != other)
        return PT_BAD_ALT_LBA;
    return header_layout(h, d->block_size, d->blocks - 1);
}

/* ---- the entries ---------------------------------------------------------------------- */

/* Code point c as UTF-8 at out; returns the bytes written (1 to 4). */
static uint32_t put_utf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3f));
    out[2] = (char)(0x80 | (c >> 6 & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

/* The entry's name (36 UTF-16LE units at u) as UTF-8 in out, up to the
 * first zero unit. A unit alone takes at most 3 bytes and a surrogate pair
 * 4 for its two units, so 36 units fit in PT_NAME_MAX (108) bytes. An
 * unpaired surrogate, and a control character (C0 or DEL: a name is
 * printed in logs and on the shell), becomes '?'. */
static void name_utf8(const uint8_t *u, char *out)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < GPT_NAME_UNITS; i++) {
        uint32_t c = pt_le16(u + 2 * i);
        if (!c)
            break;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < GPT_NAME_UNITS) {
            uint32_t lo = pt_le16(u + 2 * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        if ((c >= 0xd800 && c < 0xe000) || c < 0x20 || c == 0x7f)
            c = '?';
        o += put_utf8(out + o, c);
    }
    out[o] = 0;
}

/* Entry `index` (its first 128 bytes at e) of the array header h
 * describes: listed, left out with a note, or unused. */
static void entry(struct pt_table *t, const struct hdr *h, uint32_t index, const uint8_t *e)
{
    if (pt_guid_zero(e))
        return;   /* unused */
    t->used++;
    uint64_t start = pt_le64(e + 32), end = pt_le64(e + 40);
    if (start < h->first || start > end || end > h->last) {
        pt_note(t, index, PT_NOTE_OUTSIDE);
        return;
    }
    uint64_t blocks = end - start + 1;   /* end <= last < the disk's blocks: no wrap */
    if (pt_overlaps_listed(t, start, blocks)) {
        pt_note(t, index, PT_NOTE_OVERLAP);
        return;
    }
    if (t->nparts == PT_MAX_PARTS) {
        pt_note(t, index, PT_NOTE_TOO_MANY);
        return;
    }
    struct pt_part *p = &t->parts[t->nparts++];
    *p = (struct pt_part){ .start = start, .blocks = blocks, .attributes = pt_le64(e + 48),
                           .entry = (uint16_t)index };
    for (int i = 0; i < 16; i++) {
        p->type_guid[i] = e[i];
        p->unique_guid[i] = e[16 + i];
    }
    name_utf8(e + 56, p->name);
}

/* An entry array as it streams by: its CRC, and (t set) its entries. */
struct walk {
    const struct hdr *h;
    struct pt_table *t;      /* NULL: the CRC alone */
    uint32_t crc;            /* over the bytes seen so far */
    uint64_t left;           /* the array's bytes not seen yet (the last block's tail isn't) */
    uint32_t index;          /* the entry being gathered */
    uint32_t off;            /* bytes of it seen */
    uint8_t  e[GPT_ENTRY_MIN];   /* its first 128 bytes */
};

static void piece_entries(void *st, const uint8_t *p, uint32_t bytes)
{
    struct walk *w = st;
    uint32_t n = bytes < w->left ? bytes : (uint32_t)w->left;
    w->crc = pt_crc32(w->crc, p, n);
    w->left -= n;
    if (!w->t)
        return;
    for (uint32_t i = 0; i < n;) {
        uint32_t take = w->h->size - w->off;
        if (take > n - i)
            take = n - i;
        for (uint32_t j = 0; j < take && w->off + j < GPT_ENTRY_MIN; j++)
            w->e[w->off + j] = p[i + j];
        w->off += take;
        i += take;
        if (w->off == w->h->size) {
            entry(w->t, w->h, w->index++, w->e);
            w->off = 0;
        }
    }
}

static void clear_listing(struct pt_table *t)
{
    t->used = t->nparts = t->nnotes = t->notes_total = 0;
}

/* Read h's entry array and check its CRC; with t, list its entries (and
 * clear the listing again if the CRC is wrong). */
static uint8_t array_read(const struct pt_disk *d, const struct hdr *h, struct pt_table *t)
{
    struct walk w = { .h = h, .t = t, .left = (uint64_t)h->count * h->size };
    uint64_t blocks = (w.left + d->block_size - 1) / d->block_size;
    uint8_t why = PT_BAD_NONE;
    if (pt_stream(d, h->array_lba, blocks, piece_entries, &w) != OK)
        why = PT_BAD_IO;
    else if (w.crc != h->array_crc)
        why = PT_BAD_ARRAY_CRC;
    if (why != PT_BAD_NONE && t)
        clear_listing(t);
    return why;
}

/* ---- both headers --------------------------------------------------------------------- */

static bool headers_agree(const struct hdr *p, const struct hdr *b)
{
    return pt_guid_eq(p->guid, b->guid) && p->first == b->first && p->last == b->last &&
           p->count == b->count && p->size == b->size && p->array_crc == b->array_crc;
}

void pt_gpt_read(const struct pt_disk *d, struct pt_table *t)
{
    t->scheme = PT_SCHEME_GPT;
    if (d->blocks < 3) {
        t->primary = t->backup = PT_BAD_USABLE;   /* no room for two headers and data */
        return;
    }
    uint64_t end = d->blocks - 1;
    struct hdr p, b;
    t->primary = header_read(d, 1, end, &p);
    if (t->primary == PT_BAD_NONE)
        t->primary = array_read(d, &p, t);
    t->backup = header_read(d, end, 1, &b);
    if (t->backup == PT_BAD_NONE)
        t->backup = array_read(d, &b, t->primary == PT_BAD_NONE ? NULL : t);
    if (t->primary == PT_BAD_NONE && t->backup == PT_BAD_NONE && !headers_agree(&p, &b))
        t->backup = PT_BAD_MISMATCH;
    const struct hdr *h = t->primary == PT_BAD_NONE ? &p : t->backup == PT_BAD_NONE ? &b : NULL;
    if (!h)
        return;   /* no readable table: nothing listed */
    for (int i = 0; i < 16; i++)
        t->disk_guid[i] = h->guid[i];
    t->first_usable = h->first;
    t->last_usable = h->last;
    t->entry_count = h->count;
    t->entry_size = h->size;
}
