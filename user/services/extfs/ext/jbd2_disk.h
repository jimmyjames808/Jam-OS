/* jbd2's on-disk format: the journal ext4 keeps in an inode of its own
 * volume (s_journal_inum), as it lies on the disk. Facts only, no code:
 * ext_jnl.h is the journal that writes and replays it.
 *
 * Unlike the rest of ext, every field here is BIG-endian on the disk; the
 * structures are read in place and each field goes through jbd2_be32 /
 * jbd2_be64 (which also turn a host value into the disk's order).
 *
 * The journal is the inode's blocks 0 to s_maxlen - 1 (its "log blocks"
 * here): block 0 holds the journal superblock; blocks s_first to
 * s_maxlen - 1 are a circular log of transactions. Every log block a
 * transaction writes starts with a header (magic, type, the
 * transaction's id) except the copies of the volume's blocks, which are
 * written whole:
 *
 *     [revoke block]... [descriptor block, the blocks its tags name]... commit block
 *
 * A descriptor's tags name the volume block each following log block is
 * a copy of; a revoke block lists volume blocks that copies in this and
 * earlier transactions must not be replayed onto; the commit block says
 * the transaction is whole. s_start is the log block of the oldest
 * transaction whose blocks may not be home yet (0: none, the journal is
 * empty), s_sequence its id; a replay walks from there while each block's
 * id is the one expected.
 *
 * With checksum v3 (what this library writes) every block carries a
 * CRC32C seeded from the journal's UUID: the superblock's covers itself
 * (from ~0), a descriptor's and a revoke block's their whole block (in a
 * 4-byte tail), the commit block's its whole block, and each tag the
 * copy it names (seeded with the transaction's id as well).
 *
 * Source: the jbd2 format as documented (the kernel's
 * Documentation/filesystems/ext4/journal.rst, documentation, not code),
 * checked against journals Linux's mke2fs made (tools/extfix/). Names
 * follow the format's own, without its prefixes. */
#pragma once

#include <stdint.h>

#define JBD2_MAGIC       0xC03B3998u
#define JBD2_SUPER_SIZE  1024u          /* the superblock's bytes at the start of log block 0 */
#define JBD2_UUID_SIZE   16u
#define JBD2_CRC32C      4              /* s_checksum_type */

/* h_blocktype */
#define JBD2_DESCRIPTOR  1
#define JBD2_COMMIT      2
#define JBD2_SUPER_V1    3              /* no features, no checksums */
#define JBD2_SUPER_V2    4
#define JBD2_REVOKE      5

/* s_feature_compat */
#define JBD2_COMPAT_CHECKSUM       0x01   /* v1 checksums: one CRC32 in the commit block */
/* s_feature_incompat */
#define JBD2_INCOMPAT_REVOKE       0x01   /* revoke blocks may be in the log */
#define JBD2_INCOMPAT_64BIT        0x02   /* tags and revoke records hold 64-bit block numbers */
#define JBD2_INCOMPAT_ASYNC_COMMIT 0x04   /* a commit block may be written before its blocks */
#define JBD2_INCOMPAT_CSUM_V2      0x08
#define JBD2_INCOMPAT_CSUM_V3      0x10
#define JBD2_INCOMPAT_FAST_COMMIT  0x20

/* A tag's t_flags */
#define JBD2_FLAG_ESCAPE    0x1   /* the copy began with JBD2_MAGIC: it is written with 0 there */
#define JBD2_FLAG_SAME_UUID 0x2   /* no 16-byte UUID follows this tag */
#define JBD2_FLAG_DELETED   0x4   /* never written by Linux; ignored */
#define JBD2_FLAG_LAST_TAG  0x8   /* the descriptor's last tag */

/* The header of every block but the copies. */
struct jbd2_header {
    uint32_t magic;                  /* JBD2_MAGIC */
    uint32_t blocktype;              /* JBD2_DESCRIPTOR ... */
    uint32_t sequence;               /* the transaction's id */
};

/* The journal superblock: the first 1024 bytes of log block 0. */
struct jbd2_super {
    struct jbd2_header h;            /* blocktype JBD2_SUPER_V1 or _V2 */
    uint32_t blocksize;              /* the volume's block size */
    uint32_t maxlen;                 /* log blocks in the journal */
    uint32_t first;                  /* the log's first block (after this one) */
    uint32_t sequence;               /* the oldest live transaction's id (or the next one's) */
    uint32_t start;                  /* its first log block; 0: the journal is empty */
    int32_t  error;                  /* an error the filesystem recorded (s_errno) */
    /* v2 */
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[JBD2_UUID_SIZE];   /* the checksums' seed: crc32c(~0, uuid) */
    uint32_t nr_users;               /* filesystems sharing it: 1 for an internal journal */
    uint32_t dynsuper;
    uint32_t max_transaction;        /* limits a filesystem may keep; 0: none */
    uint32_t max_trans_data;
    uint8_t  checksum_type;          /* JBD2_CRC32C with v2 or v3 */
    uint8_t  padding2[3];
    uint32_t num_fc_blks;            /* fast commit blocks at the log's end */
    uint32_t head;                   /* the log's head when it is empty (Linux 5.x) */
    uint32_t padding[40];
    uint32_t checksum;               /* crc32c(~0, these 1024 bytes with this as 0) */
    uint8_t  users[16 * 48];         /* the sharing filesystems' UUIDs */
};
_Static_assert(sizeof(struct jbd2_super) == JBD2_SUPER_SIZE, "jbd2 superblock size");
_Static_assert(__builtin_offsetof(struct jbd2_super, feature_compat) == 0x24, "s_feature_compat");
_Static_assert(__builtin_offsetof(struct jbd2_super, uuid) == 0x30, "s_uuid");
_Static_assert(__builtin_offsetof(struct jbd2_super, checksum_type) == 0x50, "s_checksum_type");
_Static_assert(__builtin_offsetof(struct jbd2_super, checksum) == 0xFC, "s_checksum");

/* A descriptor's tag with checksum v3 (16 bytes), followed by the
 * journal's UUID unless JBD2_FLAG_SAME_UUID. The tags fill the block from
 * after the header to before its tail. */
struct jbd2_tag3 {
    uint32_t blocknr;                /* the volume block the copy belongs at, low 32 bits */
    uint32_t flags;                  /* JBD2_FLAG_* */
    uint32_t blocknr_high;           /* the high 32 bits, with JBD2_INCOMPAT_64BIT */
    uint32_t checksum;               /* crc32c(seed, be32 sequence, the copy as in the log) */
};
_Static_assert(sizeof(struct jbd2_tag3) == 16, "tag3 size");

/* The last 4 bytes of a descriptor and a revoke block (v2 and v3). */
struct jbd2_tail {
    uint32_t checksum;               /* crc32c(seed, the whole block with this as 0) */
};

/* A revoke block: the header, its bytes in use, then records of 4 bytes
 * (8 with JBD2_INCOMPAT_64BIT) to `count`. */
struct jbd2_revoke_header {
    struct jbd2_header h;
    uint32_t count;                  /* bytes in use, this header's 16 included */
};
#define JBD2_REVOKE_FIRST 16u        /* the first record's offset */

/* The commit block. */
struct jbd2_commit {
    struct jbd2_header h;
    uint8_t  chksum_type;            /* v1 checksums only; 0 with v3 */
    uint8_t  chksum_size;
    uint8_t  padding[2];
    uint32_t chksum[8];              /* v3: chksum[0] = crc32c(seed, the whole block with it 0) */
    uint64_t commit_sec;             /* when it was written: informational */
    uint32_t commit_nsec;
};
_Static_assert(__builtin_offsetof(struct jbd2_commit, chksum) == 0x10, "h_chksum");
_Static_assert(__builtin_offsetof(struct jbd2_commit, commit_sec) == 0x30, "h_commit_sec");

/* A field in the disk's order to the host's and back (the library runs on
 * little-endian machines only: ext_disk.h asserts it). */
static inline uint32_t jbd2_be32(uint32_t v)
{
    return __builtin_bswap32(v);
}
static inline uint64_t jbd2_be64(uint64_t v)
{
    return __builtin_bswap64(v);
}
