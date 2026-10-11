/* ext's on-disk format: the structures of ext4 (and of ext2 and ext3,
 * which are ext4 without some of its features) as they lie on the disk,
 * and the constants that go with them. Facts only, no code: <ext.h> is the
 * library that reads them.
 *
 * Every field is little-endian on the disk. The structures are used in
 * place, over the bytes of a block in the cache, so the library is for
 * little-endian machines only (x86-64 and the Mac's arm64), which the
 * assertion below checks. Every structure keeps its natural alignment at
 * the offsets the format puts it (inodes at multiples of the inode size,
 * descriptors at multiples of their size, directory entries at multiples
 * of 4), so no field is read unaligned.
 *
 * Sources: the ext4 on-disk format as documented (the kernel's
 * Documentation/filesystems/ext4 pages, which are documentation, not
 * code), checked against images Linux's mke2fs made (tools/extfix/).
 * Names follow the format's own, without its prefixes. */
#pragma once

#include <stdint.h>

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "ext reads its on-disk structures in place: little-endian only");

#define EXT_SUPER_OFFSET   1024u        /* the primary superblock's byte offset */
#define EXT_SUPER_SIZE     1024u
#define EXT_MAGIC          0xEF53u      /* s_magic */
#define EXT_MIN_BLOCK_LOG  10           /* block size 1 KiB << s_log_block_size ... */
#define EXT_MAX_BLOCK_LOG  16           /* ... up to 64 KiB */
#define EXT_DYNAMIC_REV    1            /* s_rev_level: inode size, first inode in the superblock */
#define EXT_GOOD_OLD_INODE 128          /* the inode's first part; a larger inode has more */
#define EXT_FIRST_INO_MIN  11           /* s_first_ino is at least this */
#define EXT_ROOT_INO       2
#define EXT_JOURNAL_INO    8            /* where mke2fs puts the journal (s_journal_inum says) */
#define EXT_CRC32C         1            /* s_checksum_type */
#define EXT_NAME_MAX       255          /* bytes of a name */
#define EXT_N_BLOCKS       15           /* words of i_block */
#define EXT_DESC_MIN       32           /* a group descriptor without 64bit */
#define EXT_DESC_MIN_64    64           /* with 64bit, s_desc_size is at least this ... */
#define EXT_DESC_MAX       1024         /* ... and at most this, a power of two */

/* s_state */
#define EXT_STATE_VALID    0x0001       /* unmounted cleanly */
#define EXT_STATE_ERROR    0x0002       /* errors were found */
#define EXT_STATE_ORPHANS  0x0004       /* orphans being recovered */

/* s_feature_compat: a reader that doesn't know one may still read and write. */
#define EXT_COMPAT_DIR_PREALLOC   0x0001
#define EXT_COMPAT_IMAGIC_INODES  0x0002
#define EXT_COMPAT_HAS_JOURNAL    0x0004
#define EXT_COMPAT_EXT_ATTR       0x0008
#define EXT_COMPAT_RESIZE_INODE   0x0010
#define EXT_COMPAT_DIR_INDEX      0x0020
#define EXT_COMPAT_LAZY_BG        0x0040
#define EXT_COMPAT_EXCLUDE_INODE  0x0080
#define EXT_COMPAT_EXCLUDE_BITMAP 0x0100
#define EXT_COMPAT_SPARSE_SUPER2  0x0200
#define EXT_COMPAT_FAST_COMMIT    0x0400
#define EXT_COMPAT_STABLE_INODES  0x0800
#define EXT_COMPAT_ORPHAN_FILE    0x1000

/* s_feature_incompat: a reader that doesn't know one must not mount. */
#define EXT_INCOMPAT_COMPRESSION  0x00001
#define EXT_INCOMPAT_FILETYPE     0x00002   /* directory entries carry the type */
#define EXT_INCOMPAT_RECOVER      0x00004   /* the journal needs replaying */
#define EXT_INCOMPAT_JOURNAL_DEV  0x00008   /* this is an external journal */
#define EXT_INCOMPAT_META_BG      0x00010
#define EXT_INCOMPAT_EXTENTS      0x00040
#define EXT_INCOMPAT_64BIT        0x00080
#define EXT_INCOMPAT_MMP          0x00100
#define EXT_INCOMPAT_FLEX_BG      0x00200
#define EXT_INCOMPAT_EA_INODE     0x00400
#define EXT_INCOMPAT_DIRDATA      0x01000
#define EXT_INCOMPAT_CSUM_SEED    0x02000   /* the checksum seed is s_checksum_seed */
#define EXT_INCOMPAT_LARGEDIR     0x04000
#define EXT_INCOMPAT_INLINE_DATA  0x08000
#define EXT_INCOMPAT_ENCRYPT      0x10000
#define EXT_INCOMPAT_CASEFOLD     0x20000

/* s_feature_ro_compat: a reader that doesn't know one may read, not write. */
#define EXT_RO_COMPAT_SPARSE_SUPER  0x00001
#define EXT_RO_COMPAT_LARGE_FILE    0x00002
#define EXT_RO_COMPAT_BTREE_DIR     0x00004
#define EXT_RO_COMPAT_HUGE_FILE     0x00008
#define EXT_RO_COMPAT_GDT_CSUM      0x00010   /* uninit_bg: CRC16 group descriptors */
#define EXT_RO_COMPAT_DIR_NLINK     0x00020
#define EXT_RO_COMPAT_EXTRA_ISIZE   0x00040
#define EXT_RO_COMPAT_HAS_SNAPSHOT  0x00080
#define EXT_RO_COMPAT_QUOTA         0x00100
#define EXT_RO_COMPAT_BIGALLOC      0x00200
#define EXT_RO_COMPAT_METADATA_CSUM 0x00400   /* CRC32C on every metadata structure */
#define EXT_RO_COMPAT_REPLICA       0x00800
#define EXT_RO_COMPAT_READONLY      0x01000
#define EXT_RO_COMPAT_PROJECT       0x02000
#define EXT_RO_COMPAT_SHARED_BLOCKS 0x04000
#define EXT_RO_COMPAT_VERITY        0x08000
#define EXT_RO_COMPAT_ORPHAN_PRESENT 0x10000

/* The superblock (1024 bytes at byte 1024 of the volume; backups at the
 * start of some groups, not read here). */
struct ext_super {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;      /* reserved for root */
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;       /* 1 with 1 KiB blocks, else 0 */
    uint32_t log_block_size;         /* block size = 1024 << this */
    uint32_t log_cluster_size;       /* = log_block_size unless bigalloc */
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;                  /* last mount */
    uint32_t wtime;                  /* last write */
    uint16_t mnt_count;
    int16_t  max_mnt_count;
    uint16_t magic;                  /* EXT_MAGIC */
    uint16_t state;                  /* EXT_STATE_* */
    uint16_t errors;                 /* what to do on an error */
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;              /* EXT_DYNAMIC_REV */
    uint16_t def_resuid;
    uint16_t def_resgid;
    /* 0x54: the dynamic revision's fields */
    uint32_t first_ino;              /* the first inode not reserved */
    uint16_t inode_size;
    uint16_t block_group_nr;         /* the group holding this copy */
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    char     last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t  prealloc_blocks;
    uint8_t  prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;    /* resize_inode: blocks kept after the descriptors */
    /* 0xD0: the journal */
    uint8_t  journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;            /* the orphan list's first inode */
    uint32_t hash_seed[4];
    uint8_t  def_hash_version;
    uint8_t  jnl_backup_type;
    uint16_t desc_size;              /* group descriptor size with 64bit */
    /* 0x100 */
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t jnl_blocks[17];         /* a copy of the journal inode's i_block and size */
    /* 0x150: 64bit */
    uint32_t blocks_count_hi;
    uint32_t r_blocks_count_hi;
    uint32_t free_blocks_count_hi;
    uint16_t min_extra_isize;
    uint16_t want_extra_isize;
    uint32_t flags;
    uint16_t raid_stride;
    uint16_t mmp_interval;
    uint64_t mmp_block;
    uint32_t raid_stripe_width;
    uint8_t  log_groups_per_flex;
    uint8_t  checksum_type;          /* EXT_CRC32C */
    uint8_t  encryption_level;
    uint8_t  reserved_pad;
    uint64_t kbytes_written;
    uint32_t snapshot_inum;
    uint32_t snapshot_id;
    uint64_t snapshot_r_blocks_count;
    uint32_t snapshot_list;
    uint32_t error_count;
    uint32_t first_error_time;
    uint32_t first_error_ino;
    uint64_t first_error_block;
    uint8_t  first_error_func[32];
    uint32_t first_error_line;
    uint32_t last_error_time;
    uint32_t last_error_ino;
    uint32_t last_error_line;
    uint64_t last_error_block;
    uint8_t  last_error_func[32];
    /* 0x200 */
    uint8_t  mount_opts[64];
    uint32_t usr_quota_inum;
    uint32_t grp_quota_inum;
    uint32_t overhead_clusters;
    uint32_t backup_bgs[2];          /* sparse_super2's two backups */
    uint8_t  encrypt_algos[4];
    uint8_t  encrypt_pw_salt[16];
    uint32_t lpf_ino;                /* lost+found */
    uint32_t prj_quota_inum;
    uint32_t checksum_seed;          /* with csum_seed: crc32c(~0, uuid) as it was */
    uint8_t  wtime_hi;
    uint8_t  mtime_hi;
    uint8_t  mkfs_time_hi;
    uint8_t  lastcheck_hi;
    uint8_t  first_error_time_hi;
    uint8_t  last_error_time_hi;
    uint8_t  first_error_errcode;
    uint8_t  last_error_errcode;
    uint16_t encoding;
    uint16_t encoding_flags;
    uint32_t orphan_file_inum;
    uint32_t reserved[94];
    uint32_t checksum;               /* crc32c(~0, the 1020 bytes before it) */
};
_Static_assert(sizeof(struct ext_super) == EXT_SUPER_SIZE, "superblock size");
_Static_assert(__builtin_offsetof(struct ext_super, first_ino) == 0x54, "first_ino");
_Static_assert(__builtin_offsetof(struct ext_super, journal_uuid) == 0xD0, "journal_uuid");
_Static_assert(__builtin_offsetof(struct ext_super, default_mount_opts) == 0x100, "0x100");
_Static_assert(__builtin_offsetof(struct ext_super, blocks_count_hi) == 0x150, "blocks hi");
_Static_assert(__builtin_offsetof(struct ext_super, mount_opts) == 0x200, "mount_opts");
_Static_assert(__builtin_offsetof(struct ext_super, checksum_seed) == 0x270, "checksum seed");
_Static_assert(__builtin_offsetof(struct ext_super, checksum) == 0x3FC, "checksum");

/* bg_flags */
#define EXT_BG_INODE_UNINIT 0x0001   /* the inode bitmap and table were never written */
#define EXT_BG_BLOCK_UNINIT 0x0002   /* the block bitmap was never written */
#define EXT_BG_INODE_ZEROED 0x0004   /* the inode table is zeroed */

/* A group descriptor: 32 bytes, or s_desc_size (64 and more) with 64bit,
 * the _hi halves then in the second 32. In the blocks after the
 * superblock's (no meta_bg), one per group in order. */
struct ext_group_desc {
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint16_t free_blocks_count_lo;
    uint16_t free_inodes_count_lo;
    uint16_t used_dirs_count_lo;
    uint16_t flags;                  /* EXT_BG_* */
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;       /* inodes never used, at the table's end */
    uint16_t checksum;
    /* 0x20: 64bit */
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t itable_unused_hi;
    uint32_t exclude_bitmap_hi;
    uint16_t block_bitmap_csum_hi;
    uint16_t inode_bitmap_csum_hi;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ext_group_desc) == 64, "group descriptor size");
_Static_assert(__builtin_offsetof(struct ext_group_desc, checksum) == 0x1E, "bg_checksum");

/* i_mode's type (the top four bits) */
#define EXT_S_IFMT   0xF000
#define EXT_S_IFSOCK 0xC000
#define EXT_S_IFLNK  0xA000
#define EXT_S_IFREG  0x8000
#define EXT_S_IFBLK  0x6000
#define EXT_S_IFDIR  0x4000
#define EXT_S_IFCHR  0x2000
#define EXT_S_IFIFO  0x1000

/* i_flags */
#define EXT_SECRM_FL        0x00000001
#define EXT_IMMUTABLE_FL    0x00000010
#define EXT_APPEND_FL       0x00000020
#define EXT_ENCRYPT_FL      0x00000800
#define EXT_INDEX_FL        0x00001000   /* a hashed (indexed) directory */
#define EXT_JOURNAL_DATA_FL 0x00004000
#define EXT_HUGE_FILE_FL    0x00040000   /* i_blocks counts blocks, not 512-byte sectors */
#define EXT_EXTENTS_FL      0x00080000   /* i_block holds an extent tree's root */
#define EXT_VERITY_FL       0x00100000
#define EXT_EA_INODE_FL     0x00200000
#define EXT_INLINE_DATA_FL  0x10000000
#define EXT_CASEFOLD_FL     0x40000000

/* An inode: the first 128 bytes always, the rest when i_extra_isize says
 * (up to s_inode_size; anything after the fields it covers holds extended
 * attributes). Times are seconds, each with an _extra word in the large
 * part: its low 2 bits extend the seconds past 2038, the rest are
 * nanoseconds. */
struct ext_disk_inode {
    uint16_t mode;                   /* EXT_S_IF* and the permission bits */
    uint16_t uid;                    /* low 16 bits */
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;                  /* deletion time; on the orphan list, the next orphan */
    uint16_t gid;                    /* low 16 bits */
    uint16_t links_count;
    uint32_t blocks_lo;              /* 512-byte sectors (or blocks: EXT_HUGE_FILE_FL) */
    uint32_t flags;                  /* EXT_*_FL */
    uint32_t version;
    uint32_t block[EXT_N_BLOCKS];    /* extent root, block map, or a short link's target */
    uint32_t generation;
    uint32_t file_acl_lo;            /* the extended attribute block */
    uint32_t size_high;
    uint32_t obso_faddr;
    uint16_t blocks_high;            /* with huge_file */
    uint16_t file_acl_high;
    uint16_t uid_high;
    uint16_t gid_high;
    uint16_t checksum_lo;            /* metadata_csum: the inode checksum's low half */
    uint16_t reserved;
    /* 0x80: the large inode's part */
    uint16_t extra_isize;            /* bytes of the fields below in use */
    uint16_t checksum_hi;
    uint32_t ctime_extra;
    uint32_t mtime_extra;
    uint32_t atime_extra;
    uint32_t crtime;
    uint32_t crtime_extra;
    uint32_t version_hi;
    uint32_t projid;
};
_Static_assert(sizeof(struct ext_disk_inode) == 160, "inode size");
_Static_assert(__builtin_offsetof(struct ext_disk_inode, block) == 0x28, "i_block");
_Static_assert(__builtin_offsetof(struct ext_disk_inode, checksum_lo) == 0x7C, "checksum lo");
_Static_assert(__builtin_offsetof(struct ext_disk_inode, extra_isize) == 0x80, "extra_isize");

/* Extents: a tree whose root is i_block (a header and 4 entries); each
 * node is a header and entries; leaves (depth 0) hold extents, the others
 * index entries. A node in a block ends in a tail with its checksum. */
#define EXT_EXTENT_MAGIC     0xF30Au
#define EXT_EXTENT_MAX_DEPTH 5        /* the format's limit */
#define EXT_INIT_MAX_LEN     32768u   /* an extent's ee_len up to this is written ... */
                                      /* ... above it, unwritten (reads as zeros), len - this */
struct ext_extent_header {
    uint16_t magic;                   /* EXT_EXTENT_MAGIC */
    uint16_t entries;                 /* entries in use */
    uint16_t max;                     /* entries room is left for */
    uint16_t depth;                   /* 0: a leaf */
    uint32_t generation;
};
struct ext_extent {                   /* a leaf's entry: a run of blocks */
    uint32_t block;                   /* first logical block */
    uint16_t len;                     /* blocks (see EXT_INIT_MAX_LEN) */
    uint16_t start_hi;                /* first physical block, high 16 bits */
    uint32_t start_lo;
};
struct ext_extent_idx {               /* an index entry: a child node */
    uint32_t block;                   /* the first logical block it covers */
    uint32_t leaf_lo;                 /* the child's physical block */
    uint16_t leaf_hi;
    uint16_t unused;
};
struct ext_extent_tail {
    uint32_t checksum;                /* crc32c(inode seed, the node up to max entries) */
};
_Static_assert(sizeof(struct ext_extent_header) == 12, "extent header");
_Static_assert(sizeof(struct ext_extent) == 12, "extent");
_Static_assert(sizeof(struct ext_extent_idx) == 12, "extent index");

/* Directory entries: a directory's blocks are each a chain of entries,
 * rec_len to the next, the last reaching the block's end (less a 12-byte
 * tail with metadata_csum). Inode 0 marks an unused entry. Without
 * filetype, name_len is 16 bits and file_type its high byte (0). */
#define EXT_DIRENT_HEADER 8           /* the bytes before the name */
#define EXT_DIR_TAIL_SIZE 12
#define EXT_DIR_TAIL_FT   0xDE        /* the tail's file_type */
enum ext_ftype {                      /* file_type */
    EXT_FT_UNKNOWN = 0,
    EXT_FT_REG     = 1,
    EXT_FT_DIR     = 2,
    EXT_FT_CHR     = 3,
    EXT_FT_BLK     = 4,
    EXT_FT_FIFO    = 5,
    EXT_FT_SOCK    = 6,
    EXT_FT_SYMLINK = 7,
    EXT_FT_MAX     = 8
};
struct ext_dirent {
    uint32_t inode;
    uint16_t rec_len;                 /* to the next entry (64 KiB blocks: encoded, ext_rec_len) */
    uint8_t  name_len;
    uint8_t  file_type;               /* enum ext_ftype, with filetype */
    char     name[];                  /* name_len bytes, no NUL */
};
struct ext_dir_tail {                 /* the last 12 bytes of a leaf block, metadata_csum */
    uint32_t zero1;                   /* 0: looks like an unused entry */
    uint16_t rec_len;                 /* 12 */
    uint8_t  zero2;                   /* name_len 0 */
    uint8_t  file_type;               /* EXT_DIR_TAIL_FT */
    uint32_t checksum;                /* crc32c(inode seed, the block before the tail) */
};
_Static_assert(sizeof(struct ext_dirent) == EXT_DIRENT_HEADER, "dirent header");
_Static_assert(sizeof(struct ext_dir_tail) == EXT_DIR_TAIL_SIZE, "dir tail");

/* An indexed directory (dir_index and EXT_INDEX_FL): block 0 is the
 * index's root, after an entry for "." and one for ".." that covers the
 * rest of the block, so a reader that doesn't know indexes sees "." and
 * ".." and nothing else; the index's inner nodes are blocks that look
 * like one empty entry the size of the block. The leaves are ordinary
 * entry blocks. With metadata_csum the root and nodes end their entries'
 * room with a dx_tail instead of a dir_tail. */
#define EXT_DX_ROOT_COUNT 32          /* the count and limit's offset in the root */
#define EXT_DX_NODE_COUNT 8           /* and in an inner node */
struct ext_dx_root_info {
    uint32_t reserved_zero;
    uint8_t  hash_version;
    uint8_t  info_length;             /* 8 */
    uint8_t  indirect_levels;
    uint8_t  unused_flags;
};
struct ext_dx_countlimit {
    uint16_t limit;                   /* entries room is left for */
    uint16_t count;                   /* entries in use, this one included */
};
struct ext_dx_entry {
    uint32_t hash;
    uint32_t block;                   /* a logical block of the directory */
};
struct ext_dx_tail {
    uint32_t reserved;
    uint32_t checksum;                /* crc32c(inode seed, up to count entries, the tail as 0s) */
};

/* An extended attribute block (i_file_acl): a header, then the
 * attributes. Inodes with the same attributes may share one block
 * (refcount). Its checksum starts from the volume's seed and the block's
 * own number, so it isn't the inode's. */
#define EXT_XATTR_MAGIC 0xEA020000u
struct ext_xattr_header {
    uint32_t magic;                   /* EXT_XATTR_MAGIC */
    uint32_t refcount;                /* inodes that point at it */
    uint32_t blocks;                  /* 1 */
    uint32_t hash;
    uint32_t checksum;                /* crc32c(fs seed, le64 block number, the block, this as 0) */
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ext_xattr_header) == 32, "xattr header");
