/* The partition table library's entry point: block 0 (the MBR, a
 * protective MBR, a FAT boot sector, or zeros), the blank rule, Jam OS's
 * mark, and the small pieces both files use (reading runs of blocks,
 * GUIDs, CRC32). GPT itself is parttable_gpt.c. <jam/parttable.h> has the
 * model and the rules. */
#include <jam/parttable.h>
#include "parttable_int.h"

const uint8_t pt_type_esp[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};
const uint8_t pt_type_msr[16] = {
    0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d, 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae
};
const uint8_t pt_type_basic_data[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};
const uint8_t pt_type_recovery[16] = {
    0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d, 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac
};
const uint8_t pt_type_jamos_data[16] = {
    0x65, 0xe5, 0xba, 0x71, 0xa8, 0xd4, 0x15, 0x43, 0x8c, 0x81, 0x9b, 0x35, 0x46, 0x2c, 0xc1, 0x58
};

#define MBR_ENTRIES   4      /* an MBR's primary entries, at 446 */
#define MBR_TYPE_GPT  0xee   /* a protective MBR's entry type */

/* ---- reading -------------------------------------------------------------------------- */

#define PT_FILL 0xa5   /* the buffer before every read: not zeros, no table's bytes */

status_t pt_get(const struct pt_disk *d, uint64_t lba, uint32_t count)
{
    /* A read that answers OK without its data landing (a DMA the IOMMU
     * dropped, a device that wrote somewhere else) would leave what the
     * buffer held, and a new buffer is zeros: the blank rule would take
     * any drive for an empty one, and create_table could write it. Filled
     * first, such a read looks like someone else's data. */
    uint8_t *b = d->buf;
    for (uint32_t i = 0; i < count * d->block_size; i++)
        b[i] = PT_FILL;
    return d->read(d->ctx, lba, count, d->buf);
}

status_t pt_stream(const struct pt_disk *d, uint64_t lba, uint64_t n, pt_piece_fn fn, void *st)
{
    if (lba > d->blocks || n > d->blocks - lba)
        return ERR_OUT_OF_RANGE;
    uint32_t per = d->buf_size / d->block_size;   /* at least 1: pt_read checked */
    while (n) {
        uint32_t k = n < per ? (uint32_t)n : per;
        status_t st2 = pt_get(d, lba, k);
        if (st2 != OK)
            return st2;
        fn(st, d->buf, k * d->block_size);
        lba += k;
        n -= k;
    }
    return OK;
}

bool pt_all_zero(const uint8_t *p, uint32_t bytes)
{
    uint8_t any = 0;
    for (uint32_t i = 0; i < bytes; i++)
        any |= p[i];
    return !any;
}

static void piece_zero(void *st, const uint8_t *p, uint32_t bytes)
{
    bool *zero = st;
    *zero = *zero && pt_all_zero(p, bytes);
}

/* The blank rule's ends: the first and last PT_BLANK_BYTES (the whole disk
 * if it is smaller than both) all zeros. A read error: not blank. */
static bool ends_zero(const struct pt_disk *d)
{
    uint64_t r = PT_BLANK_BYTES / d->block_size;
    bool zero = true;
    if (d->blocks <= 2 * r)
        return pt_stream(d, 0, d->blocks, piece_zero, &zero) == OK && zero;
    return pt_stream(d, 0, r, piece_zero, &zero) == OK &&
           pt_stream(d, d->blocks - r, r, piece_zero, &zero) == OK && zero;
}

/* ---- the MBR -------------------------------------------------------------------------- */

/* Is s (block 0) a FAT boot sector for a volume that fits on the disk? An
 * MBR's first bytes may be a jump too, and its boot code may hold any
 * bytes, so every field a FAT volume needs is checked: the jump, 512 to
 * 4096 bytes per sector, a power-of-two cluster, reserved sectors, one or
 * two FATs, a size inside the disk, and the "FAT" of the type text (at 54
 * for FAT12/16, at 82 for FAT32). */
static bool fat_boot_sector(const struct pt_disk *d, const uint8_t *s)
{
    if (s[0] != 0xeb && s[0] != 0xe9)
        return false;
    uint32_t bps = pt_le16(s + 11), spc = s[13], total = pt_le16(s + 19);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)))
        return false;
    if (!pt_le16(s + 14) || s[16] < 1 || s[16] > 2)
        return false;
    if (!total)
        total = pt_le32(s + 32);
    /* total * bps < 2^44, so only the disk's side can wrap. */
    if (!total || (d->blocks <= UINT64_MAX / d->block_size &&
                   (uint64_t)total * bps > d->blocks * d->block_size))
        return false;
    bool fat16 = s[54] == 'F' && s[55] == 'A' && s[56] == 'T';
    bool fat32 = s[82] == 'F' && s[83] == 'A' && s[84] == 'T' && s[85] == '3' && s[86] == '2';
    return fat16 || fat32;
}

static bool mbr_in_use(const uint8_t *e)
{
    return e[4] && pt_le32(e + 12);
}

/* Does s (block 0) hold a partition table? Every entry's status byte is
 * 00 or 80, at least one entry is in use, and none starts at block 0 (the
 * table's own block: mtools' mformat writes such an entry, covering the
 * whole disk, into the boot sector of a volume without a table). Asked
 * before fat_boot_sector: some formatters leave a jump and a BPB in the
 * MBR of a partitioned stick, and a disk with partitions must never be
 * served whole. A FAT boot sector has boot code or zeros where the table
 * would be: text and code fail the status bytes, zeros have no entry in
 * use. */
static bool mbr_table(const uint8_t *s)
{
    bool used = false;
    for (int i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        if ((e[0] != 0x00 && e[0] != 0x80) || (mbr_in_use(e) && !pt_le32(e + 8)))
            return false;
        used |= mbr_in_use(e);
    }
    return used;
}

/* Is s (block 0) a protective MBR: an entry of type EEh in use? UEFI
 * (5.2.3) asks for nothing more, and ignores the status bytes. *hybrid:
 * other entries in use beside it. */
static bool mbr_protective(const uint8_t *s, bool *hybrid)
{
    bool gpt = false, other = false;
    for (int i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        if (!mbr_in_use(e))
            continue;
        if (e[4] == MBR_TYPE_GPT)
            gpt = true;
        else
            other = true;
    }
    *hybrid = gpt && other;
    return gpt;
}

bool pt_overlaps_listed(const struct pt_table *t, uint64_t start, uint64_t blocks)
{
    for (uint32_t j = 0; j < t->nparts; j++)
        if (start < t->parts[j].start + t->parts[j].blocks &&
            t->parts[j].start < start + blocks)
            return true;
    return false;
}

/* Block 0 with 55 AA and no protective MBR: an MBR's entries, or a FAT
 * boot sector served whole. A listed entry is inside the disk, past block
 * 0, and overlaps no earlier listed one. */
static void mbr_read(const struct pt_disk *d, const uint8_t *s, struct pt_table *t)
{
    if (!mbr_table(s) && fat_boot_sector(d, s)) {
        t->parts[t->nparts++] = (struct pt_part){ .mbr_type = PT_WHOLE, .start = 0,
                                                  .blocks = d->blocks };
        return;
    }
    t->mbr_id = pt_le32(s + 440);
    t->scheme = PT_SCHEME_MBR;
    for (int i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        uint64_t start = pt_le32(e + 8), count = pt_le32(e + 12);
        if (!mbr_in_use(e))
            continue;
        t->used++;
        if (!start || start >= d->blocks || count > d->blocks - start)
            pt_note(t, (uint32_t)i, PT_NOTE_OUTSIDE);
        else if (pt_overlaps_listed(t, start, count))
            pt_note(t, (uint32_t)i, PT_NOTE_OVERLAP);
        else
            t->parts[t->nparts++] = (struct pt_part){ .mbr_type = e[4], .start = start,
                                                      .blocks = count, .entry = (uint16_t)i };
    }
}

void pt_note(struct pt_table *t, uint32_t entry, enum pt_note_why why)
{
    if (t->nnotes < PT_MAX_NOTES)
        t->notes[t->nnotes++] = (struct pt_note){ .entry = (uint16_t)entry, .why = (uint8_t)why };
    t->notes_total++;
}

/* ---- block 0 all zeros: blank, or blank-in-progress -------------------------------- */

static bool header_zero_or_good(uint8_t why)
{
    return why == PT_BAD_NONE || why == PT_BAD_ZERO;
}

/* What the first piece of create_table leaves: the blank rule's ends are
 * zeros but for the backup entry array. Fed the ends' blocks in order; the
 * array's first PT_JAMOS_HEAD_BYTES are kept, everything else must be zero. */
struct cut {
    bool     zero;                         /* nothing outside the array's head so far */
    uint32_t seen;                         /* bytes of the array fed so far */
    uint8_t  head[PT_JAMOS_HEAD_BYTES];
};

static void piece_array(void *st, const uint8_t *p, uint32_t bytes)
{
    struct cut *c = st;
    for (uint32_t i = 0; i < bytes; i++, c->seen++) {
        if (c->seen < PT_JAMOS_HEAD_BYTES)
            c->head[c->seen] = p[i];
        else
            c->zero = c->zero && !p[i];
    }
}

static void piece_cut_zero(void *st, const uint8_t *p, uint32_t bytes)
{
    struct cut *c = st;
    c->zero = c->zero && pt_all_zero(p, bytes);
}

/* Is the disk a blank one whose create_table was cut after its first
 * write, the backup entry array? Both header blocks zero (they lie in the
 * ends, so the stream covers them), every other block of the ends zero,
 * and the array's blocks, at pt_jamos_plan's place, exactly the two
 * entries pt_jamos_make makes for this disk and zeros after them. Exact,
 * not "non-zero": any other byte anywhere in the ends is someone else's
 * data. (The array's non-zero bytes all lie in its first block, so a
 * write torn between blocks leaves the same bytes or zeros.) */
static bool cut_after_array(const struct pt_disk *d)
{
    struct pt_jamos_plan p;
    uint64_t r = PT_BLANK_BYTES / d->block_size;
    if (d->blocks <= 2 * r || pt_jamos_plan(d->block_size, d->blocks, &p) != OK ||
        p.tail_start < d->blocks - r)
        return false;
    struct cut c = { .zero = true };
    uint64_t after = p.tail_start + p.array_blocks;   /* the backup header's block, and on */
    if (pt_stream(d, 0, r, piece_cut_zero, &c) != OK ||
        pt_stream(d, d->blocks - r, p.tail_start - (d->blocks - r), piece_cut_zero, &c) != OK ||
        pt_stream(d, p.tail_start, p.array_blocks, piece_array, &c) != OK ||
        pt_stream(d, after, d->blocks - after, piece_cut_zero, &c) != OK)
        return false;
    return c.zero && pt_jamos_array_head_is(d->block_size, d->blocks, c.head);
}

/* Does t (a GPT read with Jam OS's layout of entries) have the geometry
 * pt_jamos_make gives this disk: its usable range, 128 entries of 128
 * bytes, both partitions where the plan puts them? Then its entry arrays
 * lie in the plan's table blocks (outside the usable range, clear of the
 * headers), the only blocks create_table writes: a cut prepare's own. A
 * table of Jam OS's types and names at other places is someone's, and
 * the blocks create_table would write on it were never seen. */
static bool jamos_geometry(const struct pt_disk *d, const struct pt_table *t)
{
    struct pt_jamos_plan p;
    if (pt_jamos_plan(d->block_size, d->blocks, &p) != OK)
        return false;
    return t->first_usable == p.head_blocks && t->last_usable == p.tail_start - 1 &&
           t->entry_count == PT_JAMOS_ENTRIES && t->entry_size == 128 &&
           t->parts[0].start == p.esp_start && t->parts[0].blocks == p.esp_blocks &&
           t->parts[1].start == p.data_start && t->parts[1].blocks == p.data_blocks;
}

/* Block 0 is all zeros: nothing is listed (UEFI ignores a GPT without a
 * protective MBR). The disk is blank if its ends are zeros too; if not,
 * it may be a create_table cut short: its protective MBR is written last,
 * so such a disk has block 0 zero and headers that are each zeros or a
 * good Jam OS one at the plan's places (its GPT is read to see), or, cut
 * right after the first write, nothing but the backup entry array
 * (cut_after_array). */
static void zero_disk(const struct pt_disk *d, struct pt_table *t)
{
    t->blank = ends_zero(d);
    if (t->blank)
        return;
    if (cut_after_array(d)) {
        *t = (struct pt_table){ .scheme = PT_SCHEME_NONE, .primary = PT_BAD_ZERO,
                                .backup = PT_BAD_ZERO, .blank_in_progress = true };
        return;
    }
    pt_gpt_read(d, t);
    uint8_t primary = t->primary, backup = t->backup;
    /* (One header at least is good: two zero ones list nothing.) */
    bool in_progress = header_zero_or_good(primary) && header_zero_or_good(backup) &&
                       pt_jamos_layout(t) && jamos_geometry(d, t);
    *t = (struct pt_table){ .scheme = PT_SCHEME_NONE, .primary = primary, .backup = backup,
                            .blank_in_progress = in_progress };
}

/* ---- the entry point ------------------------------------------------------------------ */

static bool disk_ok(const struct pt_disk *d)
{
    uint32_t bs = d->block_size;
    return bs >= 512 && bs <= 4096 && !(bs & (bs - 1)) && d->blocks && d->read && d->buf &&
           d->buf_size >= bs;
}

status_t pt_read(const struct pt_disk *d, struct pt_table *t)
{
    *t = (struct pt_table){ .scheme = PT_SCHEME_NONE, .primary = PT_BAD_ABSENT,
                            .backup = PT_BAD_ABSENT };
    if (!disk_ok(d))
        return ERR_INVALID_ARGS;
    status_t st = pt_get(d, 0, 1);
    if (st != OK)
        return st;
    const uint8_t *s = d->buf;
    if (pt_all_zero(s, d->block_size)) {
        zero_disk(d, t);
        return OK;
    }
    if (s[510] != 0x55 || s[511] != 0xaa)
        return OK;   /* no partition table */
    /* A FAT boot sector whose boot code happens to hold EEh where an
     * entry's type would be stays a FAT volume, as mbr_read decides. */
    bool hybrid = false;
    if (mbr_protective(s, &hybrid) && (mbr_table(s) || !fat_boot_sector(d, s))) {
        t->hybrid = hybrid;
        pt_gpt_read(d, t);   /* overwrites d->buf, and so s */
        return OK;
    }
    mbr_read(d, s, t);
    return OK;
}

/* ---- Jam OS's mark -------------------------------------------------------------------- */

static bool name_is(const char *name, const char *want)
{
    size_t i = 0;
    for (; want[i]; i++)
        if (name[i] != want[i])
            return false;
    return !name[i];
}

bool pt_jamos_layout(const struct pt_table *t)
{
    const struct pt_part *esp = &t->parts[0], *data = &t->parts[1];
    return t->used == 2 && t->nparts == 2 && !t->notes_total && esp->entry == 0 &&
           data->entry == 1 && pt_guid_eq(esp->type_guid, pt_type_esp) &&
           name_is(esp->name, PT_JAMOS_ESP_NAME) &&
           pt_guid_eq(data->type_guid, pt_type_jamos_data) &&
           name_is(data->name, PT_JAMOS_DATA_NAME);
}

bool pt_is_jamos(const struct pt_table *t)
{
    return t->scheme == PT_SCHEME_GPT && t->primary == PT_BAD_NONE &&
           t->backup == PT_BAD_NONE && !t->hybrid && pt_jamos_layout(t);
}

enum pt_kind pt_kind(const struct pt_table *t)
{
    if (t->blank || t->blank_in_progress)
        return PT_KIND_BLANK;
    return pt_is_jamos(t) ? PT_KIND_JAMOS : PT_KIND_OTHER;
}

/* ---- GUIDs, words, CRC32 -------------------------------------------------------------- */

bool pt_guid_eq(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= a[i] ^ b[i];
    return !diff;
}

bool pt_guid_zero(const uint8_t *g)
{
    return pt_all_zero(g, 16);
}

void pt_guid_str(const uint8_t *g, char *out)
{
    /* The bytes in printed order: the first three fields are little-endian. */
    static const uint8_t order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = hex[g[order[i]] >> 4];
        out[o++] = hex[g[order[i]] & 15];
    }
    out[o] = 0;
}

const char *pt_bad_str(uint8_t why)
{
    static const char *const words[PT_BAD_COUNT] = {
        [PT_BAD_NONE] = "good",
        [PT_BAD_ABSENT] = "not read (no protective MBR)",
        [PT_BAD_IO] = "read error",
        [PT_BAD_ZERO] = "no header (zeros)",
        [PT_BAD_SIGNATURE] = "no EFI PART signature",
        [PT_BAD_REVISION] = "unknown revision",
        [PT_BAD_HEADER_SIZE] = "bad header size",
        [PT_BAD_CRC] = "header CRC",
        [PT_BAD_RESERVED] = "reserved field not zero",
        [PT_BAD_MY_LBA] = "MyLBA wrong",
        [PT_BAD_ALT_LBA] = "AlternateLBA wrong",
        [PT_BAD_USABLE] = "bad usable range",
        [PT_BAD_ENTRIES] = "bad entry count or size",
        [PT_BAD_ARRAY] = "entry array out of place",
        [PT_BAD_ARRAY_CRC] = "entry array CRC",
        [PT_BAD_MISMATCH] = "disagrees with the primary",
    };
    return why < PT_BAD_COUNT ? words[why] : "?";
}

const char *pt_note_str(uint8_t why)
{
    switch (why) {
    case PT_NOTE_OUTSIDE:  return "outside the disk or usable range";
    case PT_NOTE_OVERLAP:  return "overlaps an earlier partition";
    case PT_NOTE_TOO_MANY: return "too many partitions";
    default:               return "?";
    }
}

uint32_t pt_crc32(uint32_t crc, const void *p, size_t n)
{
    /* Reflected polynomial EDB88320, a nibble at a time. */
    static const uint32_t tab[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *b = p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = (crc >> 4) ^ tab[(crc ^ b[i]) & 15];
        crc = (crc >> 4) ^ tab[(crc ^ (b[i] >> 4)) & 15];
    }
    return ~crc;
}
