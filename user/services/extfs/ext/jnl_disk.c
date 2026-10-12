/* ext's journal: its blocks. Where the journal's log blocks lie (the
 * journal inode's runs, mapped once at the mount and kept in struct
 * ext_jnl, so a commit never reads an extent block through a cache full
 * of dirty buffers), reading and writing them with the device, the
 * journal superblock read, checked and written, and the checksums of
 * checksum v3 (jbd2_disk.h has the format).
 *
 * The journal's own blocks never go through the block cache: the cache
 * holds the volume's metadata (a transaction collects there), the log is
 * written from the journal's staging blocks and read back only by a
 * replay, before the cache holds anything that matters. */
#include "jnl_int.h"

/* ---- the journal inode's runs ---------------------------------------------------------- */

/* Add run r (data) to j's table, joining it to the last if it follows on. */
static status_t add_run(struct ext_fs *fs, struct ext_jnl *j, uint32_t lblk, const struct ext_run *r)
{
    struct ext_jnl_run *last = j->nruns ? &j->runs[j->nruns - 1] : NULL;
    if (last && last->pblk + last->len == r->pblk && last->lblk + last->len == lblk &&
        last->len <= UINT32_MAX - r->len) {
        last->len += r->len;
        return OK;
    }
    if (j->nruns == EXT_JNL_RUNS) {
        fs->why = "journal: in too many pieces";
        return ERR_NOT_SUPPORTED;
    }
    j->runs[j->nruns++] = (struct ext_jnl_run){ .pblk = r->pblk, .lblk = lblk, .len = r->len };
    return OK;
}

/* Map the journal inode's first `blocks` blocks: every one must be data. */
static status_t map_inode(struct ext_fs *fs, struct ext_jnl *j, struct ext_inode *in,
                          uint32_t blocks)
{
    j->nruns = 0;
    for (uint32_t lblk = 0; lblk < blocks;) {
        struct ext_run r;
        status_t st = ext_map(fs, in, lblk, &r);
        if (st != OK)
            return st;
        if (r.kind != EXT_RUN_DATA || !r.len)
            return ext_damaged(fs, "journal: a hole in its inode", in->ino, 0);
        if (r.len > blocks - lblk)
            r.len = blocks - lblk;
        st = add_run(fs, j, lblk, &r);
        if (st != OK)
            return st;
        lblk += r.len;
    }
    return OK;
}

/* Log block pos's volume block, and how many log blocks from it on lie
 * contiguous after it in the same run. */
static uint64_t where(const struct ext_jnl *j, uint32_t pos, uint32_t *contig)
{
    for (uint32_t i = 0; i < j->nruns; i++) {
        const struct ext_jnl_run *r = &j->runs[i];
        if (pos - r->lblk < r->len) {
            *contig = r->len - (pos - r->lblk);
            return r->pblk + (pos - r->lblk);
        }
    }
    *contig = 0;   /* not reached: jnl_load maps every block below maxlen */
    return 0;
}

status_t jnl_read(struct ext_fs *fs, struct ext_jnl *j, uint32_t pos, void *buf)
{
    uint32_t contig;
    uint64_t pblk = where(j, pos, &contig);
    if (!contig || fs->dev.read(fs->dev.ctx, pblk << fs->block_log, fs->block_size, buf) != OK)
        return ext_damaged(fs, "journal: read failed", fs->sb.journal_inum, pblk);
    return OK;
}

status_t jnl_write(struct ext_fs *fs, struct ext_jnl *j, uint32_t pos, uint32_t n,
                   const void *buf)
{
    const uint8_t *p = buf;
    while (n) {
        uint32_t contig;
        uint64_t pblk = where(j, pos, &contig);
        uint32_t w = n < contig ? n : contig;
        if (!w || !fs->dev.write ||
            fs->dev.write(fs->dev.ctx, pblk << fs->block_log, w << fs->block_log, p) != OK)
            return ext_damaged(fs, "journal: write failed", fs->sb.journal_inum, pblk);
        pos += w;
        n -= w;
        p += (size_t)w << fs->block_log;
    }
    return OK;
}

/* ---- checksums -------------------------------------------------------------------------- */

static const uint8_t zero4[4];

static uint32_t sb_csum(const struct jbd2_super *sb)
{
    uint32_t c = ext_crc32c(~0u, sb, __builtin_offsetof(struct jbd2_super, checksum));
    c = ext_crc32c(c, zero4, 4);
    return ext_crc32c(c, &sb->users, sizeof(sb->users));
}

/* crc32c(seed, block) with the 4 bytes at `at` read as zeros. */
static uint32_t crc_without(const struct ext_fs *fs, const struct ext_jnl *j, const void *block,
                            uint32_t at)
{
    const uint8_t *b = block;
    uint32_t c = ext_crc32c(j->seed, b, at);
    c = ext_crc32c(c, zero4, 4);
    return ext_crc32c(c, b + at + 4, fs->block_size - at - 4);
}

void jnl_tail_set(const struct ext_fs *fs, const struct ext_jnl *j, void *block)
{
    uint32_t at = fs->block_size - (uint32_t)sizeof(struct jbd2_tail);
    uint32_t c = jbd2_be32(crc_without(fs, j, block, at));
    __builtin_memcpy((uint8_t *)block + at, &c, 4);
}

bool jnl_tail_ok(const struct ext_fs *fs, const struct ext_jnl *j, const void *block)
{
    uint32_t at = fs->block_size - (uint32_t)sizeof(struct jbd2_tail), c;
    __builtin_memcpy(&c, (const uint8_t *)block + at, 4);
    return jbd2_be32(c) == crc_without(fs, j, block, at);
}

void jnl_commit_set(const struct ext_fs *fs, const struct ext_jnl *j, void *block)
{
    struct jbd2_commit *c = block;
    c->chksum[0] = jbd2_be32(crc_without(fs, j, block, __builtin_offsetof(struct jbd2_commit, chksum)));
}

bool jnl_commit_ok(const struct ext_fs *fs, const struct ext_jnl *j, const void *block)
{
    const struct jbd2_commit *c = block;
    uint32_t at = __builtin_offsetof(struct jbd2_commit, chksum);
    return jbd2_be32(c->chksum[0]) == crc_without(fs, j, block, at);
}

uint32_t jnl_tag_csum(const struct ext_fs *fs, const struct ext_jnl *j, uint32_t tid,
                      const uint8_t *copy, bool escaped)
{
    uint32_t seq = jbd2_be32(tid);
    uint32_t c = ext_crc32c(j->seed, &seq, 4);
    if (!escaped)
        return ext_crc32c(c, copy, fs->block_size);
    c = ext_crc32c(c, zero4, 4);
    return ext_crc32c(c, copy + 4, fs->block_size - 4);
}

uint64_t jnl_digest(uint64_t h, const void *block, uint32_t size)
{
    const uint8_t *p = block;
    for (uint32_t i = 0; i < size; i += 8) {
        uint64_t w;
        __builtin_memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;   /* the carries make it non-linear */
        h ^= h >> 29;
    }
    return h;
}

/* ---- the journal superblock ------------------------------------------------------------- */

#define COMPAT_KNOWN   JBD2_COMPAT_CHECKSUM
#define INCOMPAT_KNOWN (JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_64BIT | JBD2_INCOMPAT_ASYNC_COMMIT | \
                        JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3)
#define JNL_MIN_LOG    16u   /* log blocks a journal needs at least (Linux's are 1024 and more) */

/* The superblock's fields against the inode's `blocks` and the volume. */
static status_t check_sb(struct ext_fs *fs, struct ext_jnl *j, uint32_t blocks)
{
    const struct jbd2_super *sb = &j->sb;
    uint32_t ino = fs->sb.journal_inum;
    if (jbd2_be32(sb->h.magic) != JBD2_MAGIC)
        return ext_damaged(fs, "journal: no superblock", ino, 0);
    if (jbd2_be32(sb->h.blocktype) != JBD2_SUPER_V2) {
        fs->why = "journal: version 1";
        return ERR_NOT_SUPPORTED;
    }
    uint32_t maxlen = jbd2_be32(sb->maxlen), first = jbd2_be32(sb->first);
    uint32_t start = jbd2_be32(sb->start);
    if (jbd2_be32(sb->blocksize) != fs->block_size || maxlen > blocks || first < 1 ||
        first >= maxlen || maxlen - first < JNL_MIN_LOG || (start && (start < first ||
                                                                       start >= maxlen)))
        return ext_damaged(fs, "journal: superblock", ino, 0);
    uint32_t incompat = jbd2_be32(sb->feature_incompat);
    if ((jbd2_be32(sb->feature_compat) & ~COMPAT_KNOWN) || (incompat & ~INCOMPAT_KNOWN) ||
        sb->feature_ro_compat) {
        fs->why = "journal: features not known here";
        return ERR_NOT_SUPPORTED;
    }
    if (incompat & (JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3)) {
        if (sb->checksum_type != JBD2_CRC32C || jbd2_be32(sb->checksum) != sb_csum(sb))
            return ext_damaged(fs, "journal: superblock checksum", ino, 0);
    }
    j->maxlen = maxlen;
    j->first = first;
    return OK;
}

status_t jnl_load(struct ext_fs *fs, struct ext_jnl *j)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, fs->sb.journal_inum, 0, &in);
    if (st != OK)
        return st;
    uint64_t blocks = ext_isize(&in) >> fs->block_log;
    if (blocks > UINT32_MAX)
        blocks = UINT32_MAX;
    if (blocks < JNL_MIN_LOG + 1)
        return ext_damaged(fs, "journal: too small", in.ino, 0);
    /* Map what the superblock may use; the superblock itself first. */
    st = map_inode(fs, j, &in, 1);
    if (st == OK)
        st = jnl_read(fs, j, 0, j->stage);
    if (st != OK)
        return st;
    __builtin_memcpy(&j->sb, j->stage, sizeof(j->sb));
    st = check_sb(fs, j, (uint32_t)blocks);
    if (st == OK)
        st = map_inode(fs, j, &in, j->maxlen);
    if (st != OK)
        return st;
    j->seed = ext_crc32c(~0u, j->sb.uuid, JBD2_UUID_SIZE);
    j->tag_bytes = sizeof(struct jbd2_tag3);
    j->rec_bytes = jbd2_be32(j->sb.feature_incompat) & JBD2_INCOMPAT_64BIT ? 8 : 4;
    j->max_tx = (j->maxlen - j->first) / 4;
    return OK;
}

status_t jnl_sb_write(struct ext_fs *fs, struct ext_jnl *j)
{
    struct jbd2_super *sb = &j->sb;
    if (jbd2_be32(sb->feature_incompat) & (JBD2_INCOMPAT_CSUM_V2 | JBD2_INCOMPAT_CSUM_V3))
        sb->checksum = jbd2_be32(sb_csum(sb));
    /* Log block 0 is the superblock and zeros after it, as mke2fs makes it. */
    __builtin_memset(j->stage, 0, fs->block_size);
    __builtin_memcpy(j->stage, sb, sizeof(*sb));
    return jnl_write(fs, j, 0, 1, j->stage);
}
