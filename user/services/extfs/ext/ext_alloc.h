/* ext: what the name code (links.c, rename.c, orphan.c) needs from the
 * allocator: inodes and blocks taken and given back, a block added to a
 * file, a file's blocks freed. The allocator itself (bitmaps with flex
 * groups, extent trees that split and merge, truncation a step at a time)
 * is another part of the library; alloc_stub.c is a small stand-in that
 * keeps the same contracts for what the names need (extent trees of one
 * level in the inode, truncation to nothing) until it is there.
 *
 * Every function changes blocks in the cache only (dirty, for the
 * transaction), keeps the group descriptors' and the superblock's free
 * counts and the bitmaps' checksums right, and on an error leaves the
 * transaction to be given up (ext_discard). */
#pragma once

#include "ext.h"

/* Take a free inode for a new file (dir: for a directory, counted in its
 * group's used_dirs), near `parent`'s group: its bit set, the counts
 * lowered. The inode's bytes are not touched (the caller writes it
 * whole). ERR_NO_SPACE when none is free. */
status_t ext_ialloc(struct ext_fs *fs, uint32_t parent, bool dir, uint32_t *out_ino);

/* Give inode `ino` back: its bit cleared, the counts raised (dir: as
 * ext_ialloc's). ERR_IO if its bit was already clear (damage). */
status_t ext_ifree(struct ext_fs *fs, uint32_t ino, bool dir);

/* Give blocks [blk, blk + count) back. ERR_IO if one was already free. */
status_t ext_bfree(struct ext_fs *fs, uint64_t blk, uint64_t count);

/* Give logical block lblk of `in` (a hole) a block of its own, near its
 * others, recorded in its extent tree; in's block count grows. *in
 * changes (the caller writes it back with ext_iput); the block's bytes are
 * not set (the caller writes it whole: ext_bnew). ERR_NO_SPACE. */
status_t ext_alloc_lblk(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                        uint64_t *out_pblk);

/* Free every block of `in` from byte `size` on, and set its size; *in
 * changes (the caller writes it back). Its attribute block is not
 * touched. */
status_t ext_truncate(struct ext_fs *fs, struct ext_inode *in, uint64_t size);
