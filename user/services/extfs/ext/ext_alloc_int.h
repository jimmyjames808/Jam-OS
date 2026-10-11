/* ext: what U2a's files (balloc.c, ialloc.c, extwrite.c, fwrite.c) share
 * among themselves and with no one else. */
#pragma once

#include "ext_alloc.h"
#include "ext_int.h"

/* balloc.c: bits of a bitmap block. */
static inline bool ext_bit(const uint8_t *map, uint32_t i)
{
    return map[i >> 3] >> (i & 7) & 1;
}
void ext_bits_set(uint8_t *map, uint32_t from, uint32_t n);
void ext_bits_clear(uint8_t *map, uint32_t from, uint32_t n);
/* The first clear bit in [from, to), or `to` if none. */
uint32_t ext_bit_find_clear(const uint8_t *map, uint32_t from, uint32_t to);
/* Bits set in [from, to). */
uint32_t ext_bits_count(const uint8_t *map, uint32_t from, uint32_t to);

/* balloc.c: group g's bitmap, pinned, made first if the group never wrote
 * it (its *_UNINIT flag cleared); *gr its descriptor as it is now. */
status_t ext_group_bitmap(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                          struct ext_group *gr, struct ext_buf **out);
/* The bitmap b changed: its checksum into *gr, gr written back, b dirty. */
status_t ext_group_bitmap_seal(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                               struct ext_group *gr, struct ext_buf *b);

/* May fs be written? ERR_ACCESS_DENIED if not. */
static inline status_t ext_writable(const struct ext_fs *fs)
{
    return fs->ro || !fs->dev.write ? ERR_ACCESS_DENIED : OK;
}
