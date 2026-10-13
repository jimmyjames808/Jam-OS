/* ext: making a volume, with Jam OS's feature set (docs/M13.6-PLAN.md's
 * subset): has_journal, filetype, extent, flex_bg, 64bit, sparse_super,
 * large_file, huge_file, dir_nlink, extra_isize and metadata_csum; no
 * resize_inode, dir_index or ext_attr. 256-byte inodes, 64-byte group
 * descriptors, an internal journal (jbd2, clean: nothing to replay).
 *
 * What it writes, and where (mkfs.c's header has the reasons):
 *   - each flex group's bitmaps and inode tables together, at the start
 *     of its first group, after that group's superblock copy;
 *   - the superblock and the descriptors in group 0 and in each backup
 *     group (1 and the powers of 3, 5 and 7);
 *   - every block bitmap; the inode bitmaps (only group 0 has inodes in
 *     use; the others are marked never used: INODE_UNINIT, so their
 *     tables need no zeroing);
 *   - inodes 1 to 11: the reserved ones empty, the root directory (2),
 *     the journal (8), lost+found (11);
 *   - the journal in one run, in a group of its own when one is free.
 * The primary superblock is written last: a volume whose making stopped
 * part way is not ext.
 *
 * The device's writes: whole blocks, from ext_mkfs only (the volume isn't
 * mounted). mem is scratch for the blocks it writes: at least 2 blocks;
 * more makes zeroing (the journal, group 0's inode table) fewer writes. */
#pragma once

#include "ext.h"

#define EXT_MKFS_ZEROED 0x1   /* the device reads zeros already: nothing is zeroed */

struct ext_mkfs {
    uint32_t block_size;       /* 1024, 2048 or 4096; 0: 4096 */
    uint32_t blocks_per_group; /* a multiple of 8, at most 8 * block_size; 0: that most */
    uint32_t inode_ratio;      /* bytes of volume per inode; 0: 16384 */
    uint32_t inodes;           /* inodes wanted, at least (0: by the ratio) */
    uint32_t flex;             /* groups per flex group, a power of two; 0: 16 */
    uint32_t journal_blocks;   /* 0: 64 MiB on volumes over 8 GiB, else 1/128 of it */
    uint32_t uid, gid;         /* the owner of the root directory and lost+found */
    uint8_t  uuid[16];         /* the volume's (the caller's random bytes) */
    char     label[16];        /* its name, NUL-padded */
    int64_t  now;              /* seconds since 1970: every time it sets */
    uint32_t flags;            /* EXT_MKFS_* */
};

/* Make an ext volume on the whole of dev. ERR_INVALID_ARGS (a parameter
 * out of its range, a device without write); ERR_NO_SPACE (the device is
 * too small for the volume's metadata and journal); ERR_NO_MEMORY (mem
 * under 2 blocks); ERR_IO (a write failed: the volume is not made). */
status_t ext_mkfs(const struct ext_dev *dev, const struct ext_mkfs *p, void *mem,
                  size_t mem_size);
