/* ext: the read-only checker (docs/M13.6-PLAN.md's Q10, B). It looks at a
 * mounted volume the way a read-only fsck does and says what it finds; it
 * never writes, repairs nothing (repairs are Linux's e2fsck), and the same
 * code runs on the Mac (tools/jamext check, extcheck's invariants) and in
 * Jam OS (`disk check`).
 *
 * What it checks, in the order it looks:
 *   - the orphan list: no inode twice, a loop is damage;
 *   - the tree: every directory reached once from the root, its "." itself
 *     and its ".." its parent, its names unique, each entry's type its
 *     inode's, readdir's offsets rising;
 *   - link counts: a file's is its names, a directory's 2 and one per
 *     subdirectory; an inode with no names is on the orphan list with no
 *     links (one on the list with links is a truncation: its count must
 *     still be right);
 *   - inodes: a bit set in the inode bitmap for exactly the reserved
 *     inodes, the named ones and the orphans; each in use inside its
 *     group's used part (itable_unused); each group's free inodes and
 *     directories counted, the superblock's free inodes their sum; the
 *     bitmap's padding set;
 *   - blocks: each owned once (data and extent tree nodes; an attribute
 *     block by exactly its refcount of inodes), each extent inode's
 *     i_blocks what it owns, every owned block set in its group's bitmap
 *     and, when every block's owner is known (no block-mapped file with
 *     blocks, no resize inode), nothing else set but the groups'
 *     metadata; each group's free blocks counted, the superblock's their
 *     sum; the padding set.
 * The library's own checks (every checksum, every structure's bounds) run
 * as each block is read: what they find comes out as a problem too.
 *
 * It is given a mounted volume (read-only is enough), and as much memory
 * as it asks for through `alloc`: about 13 bytes an inode, an eighth of a
 * byte a block, and a few bytes an entry of the largest directory. On a
 * volume in use the answers can be torn (a transaction half checkpointed):
 * the caller checks a quiet volume, or twice. Problems are reported one
 * line each through `report`; after EXT_CHECK_MAX_PROBLEMS the check
 * stops. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ext.h"

#define EXT_CHECK_MAX_PROBLEMS 50

struct ext_check_io {
    void *ctx;                       /* the callbacks' first argument */
    /* Zeroed memory of n bytes; NULL when there is none. Anything handed
     * out is given back with release before ext_check returns. */
    void *(*alloc)(void *ctx, size_t n);
    void  (*release)(void *ctx, void *p);
    /* One problem found, as a printf format and its arguments (no newline). */
    void  (*report)(void *ctx, const char *fmt, va_list ap);
    /* NULL, or true to stop at once (a key pressed): ext_check then returns
     * ERR_CANCELED. Asked once per directory and once per group. */
    bool  (*cancel)(void *ctx);
};

struct ext_check_result {
    uint32_t problems;               /* reported */
    bool     stopped;                /* the check ended early: the problem limit */
    bool     exact;                  /* every block's owner was known */
    uint32_t dirs, names, inodes;    /* directories walked, names seen, inodes in use */
    uint32_t orphans;                /* on the orphan list */
    uint32_t free_inodes;            /* the groups' sum */
    uint64_t free_blocks;
};

/* Check fs. OK when the check ran to its end or its problem limit (look at
 * out->problems); ERR_NO_MEMORY (reported as a problem too) when alloc
 * gave nothing; ERR_CANCELED. The volume's cache is used and left with no
 * buffer pinned. */
status_t ext_check(struct ext_fs *fs, const struct ext_check_io *io, struct ext_check_result *out);
