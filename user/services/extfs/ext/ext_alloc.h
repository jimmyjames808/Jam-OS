/* ext: allocation and extents (docs/M13.6-PLAN.md's U2a), the writers on
 * top of the read side (<ext.h>): blocks and inodes taken from and given
 * back to their groups' bitmaps, with the groups' and the superblock's
 * free counts kept exact; a file's extent tree changed (an extent
 * inserted, merged with its neighbours, nodes split and the tree grown;
 * a range removed, nodes freed and the tree shortened); file data
 * written, holes filled, unwritten extents made written; a file cut a
 * step at a time; inodes made and freed.
 *
 * The rules every function here keeps (on top of <ext.h>'s):
 *   - Metadata changes only in the cache (dirty buffers): bitmaps,
 *     descriptors, the superblock, extent nodes, inodes. They reach the
 *     disk at ext_sync (or U3's commit), so a request's changes are one
 *     transaction.
 *   - File data goes around the cache, straight to the device
 *     (ext_write): to blocks the committed bitmaps still call free, or
 *     over the file's own blocks in place (ordered mode). This widens
 *     struct ext_dev's "writes only from ext_sync": ext_write writes
 *     whole blocks of file data too.
 *   - A function given a `struct ext_inode *in` that changes it (its
 *     extent root, size, block count) writes it back (ext_iput) before
 *     returning, also on most errors; the caller's copy is the inode.
 *   - A freed block's cached copy is dropped (ext_bforget), so a freed
 *     extent node can't be written over data later put in its block.
 *   - Nothing here is written to a volume whose `ro` isn't 0
 *     (ERR_ACCESS_DENIED), and only extent-mapped files are changed:
 *     ext2's and ext3's block maps are read, never written
 *     (ERR_NOT_SUPPORTED).
 *   - The root reserve (s_r_blocks_count) is not kept back: Jam OS has
 *     one user and no root (the plan's "No users").
 *
 * For U3 (the journal): blocks freed in a transaction are free in the
 * cache's bitmaps at once, so a later allocation in the same transaction
 * may take them. For metadata that is the jbd2 freed-block hazard the
 * plan names; for file data it would put new data into blocks the
 * committed metadata still gives an old file. One request frees or
 * writes new data, never both (a truncate and a write are two requests),
 * so today nothing does it; a batched transaction (Q6's later speed-up)
 * must hold freed blocks back until its commit. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ext.h"

/* ---- blocks (balloc.c) ----------------------------------------------------------------- */

/* Take up to `want` (>= 1) free blocks in one run: the first free block
 * at or after `goal` (then the groups after its, then from the volume's
 * start), and as many free blocks after it as are free, up to want and
 * the group's end. Their bits set, the counts lowered. A group whose
 * block bitmap was never written (BLOCK_UNINIT) gets one first.
 * ERR_NO_SPACE: no block free; ERR_ACCESS_DENIED; ERR_IO. */
status_t ext_balloc(struct ext_fs *fs, uint64_t goal, uint32_t want, uint64_t *out_blk,
                    uint32_t *out_n);
/* Give blocks [blk, blk + count) back: every one must be in use and none
 * a group's metadata (else ERR_IO: damage); their cached copies dropped. */
status_t ext_bfree(struct ext_fs *fs, uint64_t blk, uint64_t count);
/* Free blocks in the volume, as the superblock counts them. */
uint64_t ext_free_blocks(const struct ext_fs *fs);
/* Set the superblock's free block and inode counts to the sums of the
 * groups' (written back only if they differ). Linux keeps them only
 * roughly while a volume is mounted (e2fsck mends them), and every count
 * here moves the superblock's with the group's, so a writer's mount
 * calls this first. */
status_t ext_count_free(struct ext_fs *fs);

/* ---- inodes (ialloc.c) ----------------------------------------------------------------- */

/* A new inode's fields. */
struct ext_inode_spec {
    uint32_t parent;                 /* its directory's inode (where to put it); 0: anywhere */
    uint16_t mode;                   /* type and permission bits */
    uint16_t links;                  /* links_count to start with */
    uint32_t uid, gid;
    uint32_t generation;             /* the caller's: NFS-style, and the checksums' seed */
    struct ext_time time;            /* atime, ctime, mtime and crtime */
};

/* Make an inode: a free one taken from a group's inode bitmap (near the
 * parent's group for a file; for a directory, a flex group with more
 * free inodes and blocks than the average), its bytes in the table
 * zeroed and filled from spec; regular files and directories get an
 * empty extent tree, everything else none (a fast symbolic link's target
 * or a device's numbers go in i_block: the caller's). Written back.
 * ERR_INVALID_ARGS (a type that isn't one); ERR_NO_SPACE (no inode
 * free); ERR_ACCESS_DENIED; ERR_IO. */
status_t ext_inode_new(struct ext_fs *fs, const struct ext_inode_spec *spec,
                       struct ext_inode *out);
/* Free inode `in`, which holds no blocks any more (ext_truncate to 0
 * first: ERR_BAD_STATE otherwise; a block-mapped file's blocks:
 * ERR_NOT_SUPPORTED) and is on no orphan list (its dtime is set to
 * `dtime`, which a list would read as a link; a dtime no larger than the
 * volume's inode count, which a checker would read as one too, is raised
 * past it): its attribute block
 * released (freed when no other inode shares it), its bitmap bit cleared,
 * the counts raised; links_count 0. */
status_t ext_inode_free(struct ext_fs *fs, struct ext_inode *in, uint32_t dtime);

/* ---- extent trees (extwrite.c) --------------------------------------------------------- */

/* Give in an empty extent tree (EXT_EXTENTS_FL; the caller's iput). */
void ext_extent_init(struct ext_inode *in);
/* Map [lblk, lblk + r->len) to blocks [r->pblk, ...) (r->kind
 * EXT_RUN_DATA or EXT_RUN_UNWRITTEN): the range must be a hole
 * (ERR_ALREADY_EXISTS) inside the format's logical range. Merged with
 * the extent before or after when they continue each other; a full node
 * split, a full root grown into a new level (ERR_NO_RESOURCES past
 * EXT_EXTENT_MAX_DEPTH). The blocks are the caller's (allocated); the
 * nodes' blocks are taken here and counted in i_blocks, the data's are
 * not (ext_iblocks_add). */
status_t ext_extent_insert(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                           const struct ext_run *r);
/* Unmap [lblk, end) (end <= 2^32), from its end backwards, freeing the
 * blocks and counting them out of i_blocks: at most `budget` extents
 * removed, cut or split per call. *out_done: nothing in the range is
 * mapped any more. A split extent's second part is inserted, so a call
 * may take one node block; it fails with ERR_NO_SPACE before changing
 * anything when the volume can't give the tree its worst case. */
status_t ext_extent_punch(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint64_t end,
                          uint32_t budget, bool *out_done);
/* Make [lblk, lblk + len), which must lie inside one unwritten extent
 * (else ERR_INVALID_ARGS), written: the extent split in up to three. */
status_t ext_extent_written(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                            uint32_t len);

/* Tests only: the room of every new extent node in a block, when it is
 * at least the root's 4 and less than the block's (0: the block's). A
 * tree of 4 entries a node is 5 levels deep at a few hundred extents. Volumes made with it are not
 * the format (Linux's e2fsck wants the block's room). */
extern uint16_t ext_extent_test_room;

/* ---- files (fwrite.c) ------------------------------------------------------------------ */

/* The largest size an extent-mapped file may have: 2^32 - 1 blocks
 * (16 TiB less one block with 4 KiB blocks), as Linux's own limit. */
static inline uint64_t ext_max_size(const struct ext_fs *fs)
{
    return (((uint64_t)1 << 32) - 1) << fs->block_log;
}
/* Add n (may be negative) blocks of the volume to in's i_blocks (in
 * 512-byte sectors; past 2^32 of them only with huge_file: else
 * ERR_NO_SPACE, nothing changed). Not written back. */
status_t ext_iblocks_add(struct ext_fs *fs, struct ext_inode *in, int64_t n);
/* lblk's run; a hole is first filled: up to `want` blocks of it allocated
 * near the file's other blocks and mapped as data (their bytes are
 * whatever the disk holds: the caller writes all of them, as a directory
 * block through ext_bnew). ERR_NO_SPACE. */
status_t ext_map_alloc(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint32_t want,
                       struct ext_run *out);
/* Write len bytes at off: overwrites in place, holes filled with new
 * blocks (zeros around the bytes in a new block), unwritten extents made
 * written; the size raised to off + len if it was less. *out_n is all of
 * len, or less on ERR_NO_SPACE (what was written stays, the size
 * covering it). A write that would end past ext_max_size is cut there;
 * none at all past it: ERR_OUT_OF_RANGE. */
status_t ext_write(struct ext_fs *fs, struct ext_inode *in, uint64_t off, const void *buf,
                   size_t len, size_t *out_n);

/* Extents removed per step of ext_truncate. Each dirties a few blocks (a
 * leaf, a bitmap or two, a descriptor), so a step fits a transaction
 * however big the file. */
#define EXT_TRUNCATE_STEP 64

/* Set in's size to `size`, a step at a time: the first step sets the size
 * (the bytes of the last block past it zeroed on the disk), then each
 * removes up to EXT_TRUNCATE_STEP extents from the file's end; *out_done
 * when nothing past the size is mapped. Between steps the inode holds
 * blocks past its size, which a checker calls damage: the caller keeps
 * it on the orphan list from the first step to the last (U2b's list,
 * U4's request), so a cut between them is finished at the next mount. A
 * size larger than the file's is a hole after it (no blocks taken). */
status_t ext_truncate(struct ext_fs *fs, struct ext_inode *in, uint64_t size, bool *out_done);
