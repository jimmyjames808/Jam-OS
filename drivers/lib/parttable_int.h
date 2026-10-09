/* The partition table library's own declarations, shared by its two
 * files: parttable.c (the entry point, the MBR, the blank rule, the mark,
 * GUIDs and CRC32) and parttable_gpt.c (GPT's headers and entries).
 * <jam/parttable.h> has the model. */
#pragma once

#include <jam/parttable.h>

static inline uint32_t pt_le16(const uint8_t *p)
{
    return (uint32_t)p[1] << 8 | p[0];
}

static inline uint32_t pt_le32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static inline uint64_t pt_le64(const uint8_t *p)
{
    return (uint64_t)pt_le32(p + 4) << 32 | pt_le32(p);
}

/* Called with each piece of a run of blocks, in order (pt_stream). */
typedef void (*pt_piece_fn)(void *st, const uint8_t *p, uint32_t bytes);

/* Read blocks [lba, lba + n) through d->read, as many at a time as the
 * buffer holds, and give each piece to fn. The run must be inside the
 * disk (checked again here: ERR_OUT_OF_RANGE). Returns the first read
 * error. */
status_t pt_stream(const struct pt_disk *d, uint64_t lba, uint64_t n, pt_piece_fn fn, void *st);

/* Are the bytes all zeros? */
bool pt_all_zero(const uint8_t *p, uint32_t bytes);

/* Does [start, start + blocks) overlap a partition t lists? Both inside
 * the disk. */
bool pt_overlaps_listed(const struct pt_table *t, uint64_t start, uint64_t blocks);

/* Read the GPT into t (scheme GPT, the headers' verdicts, the listed
 * partitions), the disk's block 0 having been looked at already. */
void pt_gpt_read(const struct pt_disk *d, struct pt_table *t);

/* Only the entries of Jam OS's layout, nothing about the headers. */
bool pt_jamos_layout(const struct pt_table *t);

/* Remember that used entry `entry` was left out, and why. */
void pt_note(struct pt_table *t, uint32_t entry, enum pt_note_why why);
