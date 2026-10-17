/* ext: Jam OS's own code for a subset of ext4's on-disk format
 * (docs/M13.6-PLAN.md), the library the extfs service and the Mac's tools
 * (tools/extcheck.c) are built from. Pure code: no Jam OS calls, no
 * allocation, no logging, no threads. The caller gives it a device (two
 * calls, read and write) and one region of memory, which becomes the
 * block cache; everything else is in struct ext_fs, which the caller owns.
 *
 * The model:
 *   - ext_mount reads the superblock, decides what may be done with the
 *     volume (struct ext_fs's `ro`: written, only read, or not mounted at
 *     all, by the feature table in super.c) and checks every group
 *     descriptor. Nothing is written.
 *   - Every block goes through the cache (cache.c): ext_bread pins a
 *     block in memory, ext_brelse unpins it. A block someone changed is
 *     marked dirty (ext_bdirty) and stays in memory until ext_sync writes
 *     it home; that is the seam the journal (U3) commits through. File
 *     data is read around the cache when whole blocks are asked for.
 *   - Every structure is checked when it is read, before anything uses
 *     it: its checksum (metadata_csum: CRC32C, crc.c and csum.c; CRC16
 *     group descriptors with uninit_bg), every field against the volume's
 *     bounds. A hostile volume (a stick from anywhere) gets ERR_IO and a
 *     reason (`why`) for what it got wrong, never a read outside the
 *     volume, a loop without end or a write outside a buffer.
 *   - A structure's checksum that passed is remembered on its cache
 *     buffer (the kind and seed it passed for), so a block is checked once
 *     per time it is read from the disk; marking a buffer dirty forgets
 *     it, so a block changed without its checksum being set again is
 *     caught when it is next read.
 *
 * Who calls what, and the rules every caller keeps:
 *   - One thread at a time per struct ext_fs (the service has one).
 *   - Every function returns with no buffer left pinned, except
 *     ext_bread/ext_bnew (the caller's pin) and ext_extent_find (the
 *     path's pins, until ext_path_release).
 *   - Writers (U2a, U2b, U3) change a pinned buffer's bytes, set the
 *     changed structure's checksum (the _set functions below), then call
 *     ext_bdirty. Nothing is written to a volume whose `ro` isn't 0.
 *
 * Errors: ERR_IO (the device failed, or the volume is damaged: `why`
 * says what, and `ro` gains EXT_RO_DAMAGED), ERR_NOT_FOUND, ERR_NOT_DIR,
 * ERR_OUT_OF_RANGE (a number past the volume's), ERR_NOT_SUPPORTED (a
 * feature this library doesn't read), ERR_NO_RESOURCES (every cache
 * buffer pinned or dirty), ERR_BUFFER_TOO_SMALL. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>
#include "ext_disk.h"

/* ---- the device ------------------------------------------------------------------------ */

/* The volume (a partition or an image file). The library reads only
 * inside [0, bytes), in multiples of 1024 bytes at multiples of 1024 (the
 * superblock's 1024 bytes at 1024 first, then whole blocks of the
 * volume), and writes only whole blocks, and only from ext_sync. A write
 * that returned OK is on the medium (FUA), unless the device has a
 * `flush`: then it may sit in the device's volatile cache, and reach the
 * medium in any order, until a flush returns. The journal flushes where
 * its order needs it (ext_jnl.h: before a commit block, after it, before
 * the tail moves past what was written home). */
struct ext_dev {
    void    *ctx;                    /* read's and write's first argument */
    uint64_t bytes;                  /* the volume's size */
    status_t (*read)(void *ctx, uint64_t off, uint32_t len, void *buf);
    /* NULL: the volume is only read */
    status_t (*write)(void *ctx, uint64_t off, uint32_t len, const void *buf);
    /* NULL: every write is on the medium when it returns (FUA); else every
     * write returned so far is on the medium when this returns OK (a USB
     * stick's SYNCHRONIZE CACHE) */
    status_t (*flush)(void *ctx);
};

/* The device's flush (OK at once without one). */
static inline status_t ext_flush(const struct ext_dev *dev)
{
    return dev->flush ? dev->flush(dev->ctx) : OK;
}

/* ---- the cache ------------------------------------------------------------------------- */

#define EXT_NO_BLOCK   UINT64_MAX   /* a buffer that holds no block */
#define EXT_CACHE_MIN  16           /* buffers ext_mount needs at least */

/* What a buffer's checksum and structure were checked as (ext_bchecked). */
enum ext_kind {
    EXT_KIND_NONE = 0,       /* not checked since it was read or changed */
    EXT_KIND_RAW,            /* a block with no checksum of its own (block map, data) */
    EXT_KIND_EXTENT,         /* an extent tree node (aux: its depth) */
    EXT_KIND_DIR,            /* a directory block (aux: enum ext_dirblk) */
    EXT_KIND_BITMAP,         /* a bitmap (aux: enum ext_bitmap) */
    EXT_KIND_XATTR,          /* an extended attribute block */
};

struct ext_buf {
    uint64_t blk;            /* the block it holds; EXT_NO_BLOCK when none */
    uint8_t *data;           /* block_size bytes */
    uint64_t used;           /* the cache's clock at its last use: the LRU order */
    uint32_t pins;           /* ext_bread/ext_bnew not yet ext_brelse'd */
    uint32_t next;           /* the next buffer in its hash chain, as index + 1; 0 ends it */
    uint32_t seed;           /* the checksum seed `kind` was checked with */
    uint8_t  kind;           /* enum ext_kind */
    uint8_t  aux;            /* the kind's detail (above) */
    bool     dirty;          /* changed in memory, not yet written home */
};

struct ext_cache {
    struct ext_buf *bufs;
    uint32_t nbufs;
    uint32_t *heads;         /* hash chains: the first buffer's index + 1, 0 when empty */
    uint32_t hash_shift;     /* 64 - log2(the number of chains) */
    uint32_t ndirty;         /* buffers dirty */
    uint64_t clock;          /* bumped on every use */
    uint64_t reads, writes;  /* device calls made: for tests and the log */
    uint64_t discards;       /* ext_discard calls: what was remembered of dirty blocks is stale */
};

/* ---- the volume ------------------------------------------------------------------------ */

/* Why a volume may only be read (struct ext_fs's `ro`, bits). 0: it may be
 * written. */
enum ext_ro {
    EXT_RO_ASKED    = 1u << 0,   /* the mounter asked (EXT_MOUNT_READ_ONLY), or no dev.write */
    EXT_RO_FEATURES = 1u << 1,   /* it has features read here but never written (fs->ro_features) */
    EXT_RO_LACKS    = 1u << 2,   /* it lacks one this library writes with (EXT_RW_NEEDS_*) */
    EXT_RO_ERRORS   = 1u << 3,   /* its superblock says errors were found */
    EXT_RO_RECOVER  = 1u << 4,   /* its journal needs replaying, and nothing has replayed it */
    EXT_RO_DAMAGED  = 1u << 5,   /* damage found since the mount (an ERR_IO with a `why`) */
};

/* ext_mount's flags */
#define EXT_MOUNT_READ_ONLY 0x1

/* The features: which may be written, which only read; anything else
 * refuses the mount (super.c's table has the reasons). */
#define EXT_COMPAT_RW    (EXT_COMPAT_HAS_JOURNAL | EXT_COMPAT_EXT_ATTR | \
                          EXT_COMPAT_RESIZE_INODE | EXT_COMPAT_DIR_INDEX)
#define EXT_INCOMPAT_RW  (EXT_INCOMPAT_FILETYPE | EXT_INCOMPAT_EXTENTS | EXT_INCOMPAT_64BIT | \
                          EXT_INCOMPAT_FLEX_BG | EXT_INCOMPAT_CSUM_SEED)
#define EXT_RO_COMPAT_RW (EXT_RO_COMPAT_SPARSE_SUPER | EXT_RO_COMPAT_LARGE_FILE | \
                          EXT_RO_COMPAT_HUGE_FILE | EXT_RO_COMPAT_DIR_NLINK | \
                          EXT_RO_COMPAT_EXTRA_ISIZE | EXT_RO_COMPAT_METADATA_CSUM)
/* Written only when these are all there (else EXT_RO_LACKS). */
#define EXT_RW_NEEDS_COMPAT    EXT_COMPAT_HAS_JOURNAL
#define EXT_RW_NEEDS_INCOMPAT  (EXT_INCOMPAT_FILETYPE | EXT_INCOMPAT_EXTENTS)
#define EXT_RW_NEEDS_RO_COMPAT (EXT_RO_COMPAT_SPARSE_SUPER | EXT_RO_COMPAT_LARGE_FILE | \
                                EXT_RO_COMPAT_METADATA_CSUM)

/* Feature sets, by their word in the superblock (ext_feature_name). */
enum ext_fset { EXT_FSET_COMPAT, EXT_FSET_INCOMPAT, EXT_FSET_RO_COMPAT };

struct ext_jnl;   /* the journal (ext_jnl.h) */

struct ext_fs {
    struct ext_dev dev;
    struct ext_super sb;             /* the primary superblock, as read (and writers change it) */

    /* The volume's shape, from the superblock, every value checked by ext_mount. */
    uint32_t block_size;             /* bytes: 1024 to 65536 */
    uint32_t block_log;              /* log2(block_size) */
    uint64_t blocks;                 /* blocks in the volume */
    uint32_t first_data_block;       /* 1 with 1 KiB blocks, else 0 */
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;             /* bytes of an inode in the table: 128 to block_size */
    uint32_t inodes_per_block;
    uint32_t itable_blocks;          /* blocks of each group's inode table */
    uint32_t desc_size;              /* bytes of a group descriptor: 32, or 64 to 1024 */
    uint32_t descs_per_block;
    uint32_t groups;
    uint32_t gdt_blocks;             /* blocks of descriptors after the superblock */
    uint32_t inodes;                 /* inodes in the volume (s_inodes_count) */
    uint32_t first_ino;              /* the first inode not reserved */
    uint32_t addr_per_block;         /* block numbers in a block-map block */
    uint32_t csum_seed;              /* checksums start here: crc32c(~0, uuid) or s_checksum_seed */

    /* What it has (its features, as booleans for the code that reads them). */
    bool     csum;                   /* metadata_csum */
    bool     gdt_csum;               /* uninit_bg's CRC16 descriptors (and not metadata_csum) */
    bool     is64;                   /* 64bit: 48-bit block numbers, descriptors of desc_size */
    bool     filetype;               /* directory entries carry the type */
    bool     dir_index;              /* hashed directories may exist */
    bool     huge_file;              /* i_blocks may count blocks (EXT_HUGE_FILE_FL) */
    bool     flex_bg;                /* a group's bitmaps and table may lie in another group */
    bool     sparse_super;           /* backups only in groups 0, 1 and powers of 3, 5 and 7 */
    bool     has_journal;
    bool     needs_recovery;         /* the journal holds transactions not yet in place */

    /* The verdict. */
    uint32_t ro;                     /* enum ext_ro bits; 0: may be written */
    uint32_t ro_features[3];         /* by enum ext_fset: the bits that made it read-only */
    uint32_t refused[3];             /* the bits that refused the mount (ERR_NOT_SUPPORTED) */
    const char *why;                 /* the last problem found, in a few words; NULL: none */
    uint32_t why_ino;                /* the inode it was found in (0: none) */
    uint64_t why_blk;                /* the block it was found in (0: none) */

    struct ext_cache cache;
    struct ext_jnl *jnl;             /* the open journal (ext_jnl_mount); NULL: none */
    struct ext_names *names;         /* the directory hash (ext_names.h); NULL: none */
};

/* Mount the volume on dev: read and check the superblock and every group
 * descriptor, decide fs->ro, and make the cache in mem (mem_size bytes,
 * kept by the caller until it stops using fs; ext_mem_size says how much
 * a number of buffers needs). Reads only.
 * Returns OK; ERR_NOT_SUPPORTED (not ext, or a feature it can't read:
 * fs->refused and fs->why say which); ERR_IO (the device failed, or the
 * volume is damaged: fs->why); ERR_NO_MEMORY (mem holds fewer than
 * EXT_CACHE_MIN buffers of the volume's block size); ERR_INVALID_ARGS. */
status_t ext_mount(struct ext_fs *fs, const struct ext_dev *dev, void *mem, size_t mem_size,
                   uint32_t flags);

/* Bytes of memory a cache of `nbufs` buffers of `block_size` needs. */
size_t ext_mem_size(uint32_t nbufs, uint32_t block_size);

/* A feature bit's name ("metadata_csum"), or NULL for a bit not known. */
const char *ext_feature_name(enum ext_fset set, uint32_t bit);

/* Record that the volume is damaged: why (a few words, static), where;
 * fs->ro gains EXT_RO_DAMAGED. Returns ERR_IO, for `return ext_damaged(...)`. */
status_t ext_damaged(struct ext_fs *fs, const char *why, uint32_t ino, uint64_t blk);

/* Copy fs->sb into its block, set its checksum and mark it dirty. */
status_t ext_super_write(struct ext_fs *fs);

/* ---- blocks ---------------------------------------------------------------------------- */

/* Block `blk` in the cache, pinned: read from the device on a miss.
 * ERR_OUT_OF_RANGE past the volume; ERR_IO; ERR_NO_RESOURCES. */
status_t ext_bread(struct ext_fs *fs, uint64_t blk, struct ext_buf **out);
/* The same without reading it: zeros, for a block about to be written whole. */
status_t ext_bnew(struct ext_fs *fs, uint64_t blk, struct ext_buf **out);
void ext_brelse(struct ext_fs *fs, struct ext_buf *b);
/* b's bytes changed: write it at the next ext_sync; its check is forgotten. */
void ext_bdirty(struct ext_fs *fs, struct ext_buf *b);
/* Drop the cached copies of blocks [blk, blk + count) (none may be pinned
 * or dirty): a writer that wrote them around the cache calls it. */
void ext_binval(struct ext_fs *fs, uint64_t blk, uint64_t count);
/* Drop the cached copies of blocks [blk, blk + count), dirty ones too
 * (none may be pinned): they were freed, so what the cache holds of them
 * must neither be written home nor read again (a freed extent node
 * reused for file data). */
void ext_bforget(struct ext_fs *fs, uint64_t blk, uint64_t count);
/* Write every dirty block home, in block order. ERR_ACCESS_DENIED if the
 * volume may not be written (nothing written); ERR_IO (the rest stay dirty). */
status_t ext_sync(struct ext_fs *fs);
/* Forget every dirty block (they no longer match the disk): a transaction
 * given up. */
void ext_discard(struct ext_fs *fs);
/* Was b checked as `kind` (with `aux`) for `seed` since it was read? Mark it so. */
static inline bool ext_bchecked(const struct ext_buf *b, enum ext_kind kind, uint8_t aux,
                                uint32_t seed)
{
    return b->kind == kind && b->aux == aux && b->seed == seed;
}
static inline void ext_bmark(struct ext_buf *b, enum ext_kind kind, uint8_t aux, uint32_t seed)
{
    b->kind = (uint8_t)kind;
    b->aux = aux;
    b->seed = seed;
}
/* Buffers pinned now (tests: 0 between calls). */
uint32_t ext_cache_pins(const struct ext_fs *fs);

/* Is blk a block that metadata or data may be in: past the superblock's
 * group start and inside the volume? */
static inline bool ext_blk_ok(const struct ext_fs *fs, uint64_t blk)
{
    return blk > fs->first_data_block && blk < fs->blocks;
}
/* And the run [blk, blk + count), count >= 1? */
static inline bool ext_run_ok(const struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    return count && ext_blk_ok(fs, blk) && count <= fs->blocks - blk;
}

/* ---- checksums (crc.c, csum.c) --------------------------------------------------------- */

/* CRC32C (Castagnoli) as ext4 uses it: the running state, no inversion
 * in or out (start with ~0 or a seed). */
uint32_t ext_crc32c(uint32_t crc, const void *p, size_t n);
/* CRC16 (ARC: polynomial 0x8005, reflected), uninit_bg's descriptors. */
uint16_t ext_crc16(uint16_t crc, const void *p, size_t n);

uint32_t ext_super_csum(const struct ext_super *sb);
/* A group descriptor's checksum (desc: desc_size bytes), metadata_csum's
 * or uninit_bg's; 0 with neither. */
uint16_t ext_group_csum(const struct ext_fs *fs, uint32_t g, const void *desc);
/* The seed of everything an inode owns: crc32c(fs seed, ino, generation). */
uint32_t ext_inode_seed(const struct ext_fs *fs, uint32_t ino, uint32_t generation);
/* raw: inode_size bytes. Does its checksum match? Set it. */
bool ext_inode_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *raw);
void ext_inode_csum_set(const struct ext_fs *fs, uint32_t seed, void *raw);
/* An extent tree node in a block. false (and nothing set) if its header's
 * max leaves no room for the tail. */
bool ext_extent_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block);
bool ext_extent_csum_set(const struct ext_fs *fs, uint32_t seed, void *block);
/* A directory leaf block with its tail; _set writes the whole tail. */
bool ext_dirleaf_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block);
void ext_dirleaf_csum_set(const struct ext_fs *fs, uint32_t seed, void *block);
/* An index block (root or node) whose count and limit are at count_off.
 * false (and nothing set) if its limit leaves no room for the tail. */
bool ext_dx_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block, uint32_t count_off);
bool ext_dx_csum_set(const struct ext_fs *fs, uint32_t seed, void *block, uint32_t count_off);
/* An extended attribute block, which lies at block blk. */
uint32_t ext_xattr_csum(const struct ext_fs *fs, uint64_t blk, const void *block);

/* ---- groups (group.c) ------------------------------------------------------------------ */

/* A group descriptor, its halves joined (the _hi halves 0 with 32-byte
 * descriptors; flags and itable_unused 0 without group checksums, as they
 * then mean nothing). */
struct ext_group {
    uint64_t block_bitmap;
    uint64_t inode_bitmap;
    uint64_t inode_table;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t used_dirs;
    uint32_t itable_unused;          /* inodes never used, at the table's end */
    uint32_t block_bitmap_csum;      /* as stored: the low 16 bits, and the high 16 */
    uint32_t inode_bitmap_csum;      /* with 64-byte descriptors */
    uint16_t flags;                  /* EXT_BG_* */
};

/* Group g's descriptor, its checksum and bounds checked. */
status_t ext_group_get(struct ext_fs *fs, uint32_t g, struct ext_group *out);
/* Write it back: its checksum set, the block dirty. ERR_OUT_OF_RANGE for a
 * value the descriptor can't hold. */
status_t ext_group_put(struct ext_fs *fs, uint32_t g, const struct ext_group *in);
/* The group's first block and its number of blocks (the last may be short). */
uint64_t ext_group_first(const struct ext_fs *fs, uint32_t g);
uint32_t ext_group_blocks(const struct ext_fs *fs, uint32_t g);
/* Does group g hold a copy of the superblock and descriptors? */
bool ext_group_has_super(const struct ext_fs *fs, uint32_t g);

enum ext_bitmap { EXT_BITMAP_BLOCK, EXT_BITMAP_INODE };
/* Group g's bitmap, its checksum checked; pinned. ERR_BAD_STATE if the
 * group's flags say it was never written (the caller makes it). */
status_t ext_bitmap_read(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                         struct ext_buf **out);
/* The checksum a bitmap should carry (metadata_csum; else 0). */
uint32_t ext_bitmap_csum(const struct ext_fs *fs, enum ext_bitmap which, const void *bitmap);

/* ---- inodes (inode.c) ------------------------------------------------------------------ */

/* An inode as read: its number, its blocks' checksum seed and the
 * fields its i_extra_isize covers (128 + extra_isize bytes, at most 160;
 * zeros after: those bytes on the disk are extended attributes). */
struct ext_inode {
    uint32_t ino;
    uint32_t seed;                   /* ext_inode_seed(ino, generation); 0 without metadata_csum */
    struct ext_disk_inode d;
};

#define EXT_IGET_UNLINKED 0x1        /* links_count 0 is no error (orphans, open after unlink) */

/* Read inode `ino`: checked (its checksum, i_extra_isize, its type).
 * ERR_OUT_OF_RANGE (0 or past the volume's); ERR_NOT_FOUND (never used,
 * or freed: links_count 0 without EXT_IGET_UNLINKED); ERR_IO. */
status_t ext_iget(struct ext_fs *fs, uint32_t ino, uint32_t flags, struct ext_inode *out);
/* Write it back into its table block (the bytes ext_iget filled), its
 * checksum set; the block dirty. */
status_t ext_iput(struct ext_fs *fs, const struct ext_inode *in);
/* Where inode `ino` lies: its table block and the byte offset in it. */
status_t ext_iloc(struct ext_fs *fs, uint32_t ino, uint64_t *out_blk, uint32_t *out_off);

/* The inode's fields, joined from their halves. */
static inline uint64_t ext_isize(const struct ext_inode *in)
{
    return (uint64_t)in->d.size_high << 32 | in->d.size_lo;
}
static inline uint32_t ext_iuid(const struct ext_inode *in)
{
    return (uint32_t)in->d.uid_high << 16 | in->d.uid;
}
static inline uint32_t ext_igid(const struct ext_inode *in)
{
    return (uint32_t)in->d.gid_high << 16 | in->d.gid;
}
/* The type, as a directory entry's file_type. */
enum ext_ftype ext_itype(const struct ext_inode *in);
/* 512-byte sectors it holds (st_blocks). */
uint64_t ext_iblocks512(const struct ext_fs *fs, const struct ext_inode *in);
/* Does the large part hold a field ending at byte `end` of the inode? */
static inline bool ext_ifits(const struct ext_inode *in, uint32_t end)
{
    return end <= EXT_GOOD_OLD_INODE + (uint32_t)in->d.extra_isize;
}

enum ext_time_which { EXT_ATIME, EXT_CTIME, EXT_MTIME, EXT_CRTIME };
struct ext_time {
    int64_t  sec;                    /* since 1970, to 2446 */
    uint32_t nsec;                   /* 0 to 999999999 (0 in a small inode) */
};
void ext_itime(const struct ext_inode *in, enum ext_time_which w, struct ext_time *out);
/* Set it; the seconds' range is the format's (1901 to 2446): ERR_OUT_OF_RANGE. */
status_t ext_itime_set(struct ext_inode *in, enum ext_time_which w, const struct ext_time *t);
/* A device inode's numbers (0, 0 for anything else). */
void ext_irdev(const struct ext_inode *in, uint32_t *out_major, uint32_t *out_minor);

/* The inode's extended attribute block, pinned: its magic, its one
 * block and its checksum checked (the attributes themselves are never
 * read here: they are kept, and the block's refcount is what a writer
 * that frees the inode needs). ERR_NOT_FOUND if it has none. */
status_t ext_xattr_block(struct ext_fs *fs, const struct ext_inode *in, struct ext_buf **out);

/* The orphan list: inodes linked from s_last_orphan through i_dtime, at
 * most `cap` into list. ERR_NO_RESOURCES if longer (or a loop); ERR_IO. */
status_t ext_orphans(struct ext_fs *fs, uint32_t *list, uint32_t cap, uint32_t *out_n);

/* ---- extents and maps (extent.c, map.c) ------------------------------------------------ */

static inline uint64_t ext_extent_start(const struct ext_extent *e)
{
    return (uint64_t)e->start_hi << 32 | e->start_lo;
}
static inline uint32_t ext_extent_len(const struct ext_extent *e)
{
    return e->len <= EXT_INIT_MAX_LEN ? e->len : e->len - EXT_INIT_MAX_LEN;
}
static inline bool ext_extent_unwritten(const struct ext_extent *e)
{
    return e->len > EXT_INIT_MAX_LEN;
}
static inline uint64_t ext_idx_leaf(const struct ext_extent_idx *ix)
{
    return (uint64_t)ix->leaf_hi << 32 | ix->leaf_lo;
}
static inline struct ext_extent *ext_extents(struct ext_extent_header *h)
{
    return (struct ext_extent *)(h + 1);
}
static inline struct ext_extent_idx *ext_idxs(struct ext_extent_header *h)
{
    return (struct ext_extent_idx *)(h + 1);
}

/* A walk from an extent tree's root to the leaf for a logical block. */
struct ext_path_level {
    struct ext_buf *buf;             /* the node's block, pinned; NULL: the root, in the inode */
    struct ext_extent_header *h;     /* the node */
    int32_t at;                      /* the leaf: the last extent whose block <= the one asked
                                      * for, -1 if none; an index: the entry followed (that
                                      * one, or the first if there is none) */
};
struct ext_path {
    uint32_t depth;                  /* the tree's: lv[0] is the root, lv[depth] the leaf */
    uint32_t bound;                  /* the first logical block past this leaf's range: the
                                      * next index entry's start at some level above; 0: none */
    struct ext_path_level lv[EXT_EXTENT_MAX_DEPTH + 1];
};

/* Walk in's extent tree to lblk's leaf, every node checked; the nodes
 * stay pinned in *p until ext_path_release (also on an error: release
 * then too). ERR_INVALID_ARGS if in has no extent tree. */
status_t ext_extent_find(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                         struct ext_path *p);
void ext_path_release(struct ext_fs *fs, struct ext_path *p);

/* What a logical block is. */
enum ext_run_kind {
    EXT_RUN_HOLE,                    /* nothing there: reads as zeros */
    EXT_RUN_DATA,                    /* blocks [pblk, pblk + len) */
    EXT_RUN_UNWRITTEN,               /* allocated, never written: reads as zeros */
};
struct ext_run {
    uint64_t pblk;                   /* the first block (0 for a hole) */
    uint32_t len;                    /* blocks from lblk on that are the same kind, contiguous */
    uint8_t  kind;                   /* enum ext_run_kind */
};
/* Map logical block lblk of in (an extent tree or a block map). */
status_t ext_map(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, struct ext_run *out);

/* ---- files (file.c) -------------------------------------------------------------------- */

/* Read up to len bytes at off; *out_n is less only at the end of the file.
 * Holes read as zeros. */
status_t ext_read(struct ext_fs *fs, struct ext_inode *in, uint64_t off, void *buf, size_t len,
                  size_t *out_n);
/* A symbolic link's target (not NUL-terminated). ERR_INVALID_ARGS for
 * something that isn't a link; ERR_BUFFER_TOO_SMALL past cap. */
status_t ext_readlink(struct ext_fs *fs, struct ext_inode *in, char *buf, size_t cap,
                      size_t *out_len);

/* ---- directories (dir.c) --------------------------------------------------------------- */

/* What a directory block is (an indexed directory has all three). */
enum ext_dirblk {
    EXT_DIRBLK_LEAF,                 /* entries */
    EXT_DIRBLK_DX_ROOT,              /* "." and "..", then the index's root */
    EXT_DIRBLK_DX_NODE,              /* the index's inner node: no entries */
};

struct ext_dentry {
    uint32_t ino;                    /* 0: the directory's end */
    uint8_t  type;                   /* enum ext_ftype (EXT_FT_UNKNOWN without filetype) */
    uint8_t  name_len;
    uint64_t pos;                    /* its byte offset in the directory (readdir's cookie) */
    char     name[EXT_NAME_MAX + 1]; /* NUL-terminated; never holds '/' or NUL */
};

/* The first entry at or after byte *pos of directory dir ("." and ".."
 * included); *pos then points past it. out->ino 0 at the end.
 * ERR_NOT_DIR; ERR_IO. */
status_t ext_dir_next(struct ext_fs *fs, struct ext_inode *dir, uint64_t *pos,
                      struct ext_dentry *out);
/* The entry named name (len bytes). ERR_NOT_FOUND; ERR_NOT_DIR; ERR_IO. */
status_t ext_dir_lookup(struct ext_fs *fs, struct ext_inode *dir, const char *name, size_t len,
                        struct ext_dentry *out);
/* Directory block lblk, pinned, checked as what it is (*out_kind). */
status_t ext_dir_block(struct ext_fs *fs, struct ext_inode *dir, uint32_t lblk,
                       struct ext_buf **out, enum ext_dirblk *out_kind);
/* rec_len as stored, to bytes (64 KiB blocks encode it). */
uint32_t ext_rec_len(const struct ext_fs *fs, uint16_t stored);
/* The bytes an entry with a name of len bytes needs. */
static inline uint32_t ext_dirent_size(uint32_t name_len)
{
    return (EXT_DIRENT_HEADER + name_len + 3) & ~3u;
}
