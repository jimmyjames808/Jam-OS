/* What extcheck's journal tests run on (tools/extjnl.c has the model):
 *
 *   - the cut device: a fixture's bytes as a block device that counts the
 *     blocks written to it and, given a cut, stops after the Nth: the
 *     write that crosses the cut lands only some of its blocks (its first
 *     ones, or with `reorder` any of them, as a drive may write a long
 *     write's sectors in any order) and fails, and every write after it
 *     fails without landing. A reset puts back only the kibibytes that
 *     changed, so thousands of runs on one fixture stay cheap. With
 *     `cache` it is a USB stick's: a write sits in a volatile cache until
 *     a flush, and at the cut each block written since the last flush
 *     lands or not, whatever the order it was written in;
 *   - the storm: requests made from a seed, each a transaction of real
 *     metadata changes on the fixture (an inode's times and mode, two
 *     directory entries exchanged, the superblock's write time) and of
 *     free blocks written as metadata, freed (revoked) and written as file
 *     data around the cache, the cases the journal's rules are for. Its
 *     runner stops at the first call that fails, as the service would
 *     die at a cut. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_alloc.h>
#include "extjnl.h"

/* ---- the cut device ---------------------------------------------------------------------- */

void cut_init(struct cutdev *cd, const struct img *m)
{
    memset(cd, 0, sizeof(*cd));
    cd->size = m->size;
    cd->base = malloc(m->size);
    cd->bytes = malloc(m->size);
    cd->touched = malloc((m->size / CUT_UNIT + 1) * sizeof(uint32_t));
    cd->mark = calloc(m->size / CUT_UNIT + 1, 1);
    cd->pmark = calloc(m->size / CUT_UNIT + 1, 1);   /* blocks are at least a unit */
    if (!cd->base || !cd->bytes || !cd->touched || !cd->mark || !cd->pmark)
        fail("out of memory");
    memcpy(cd->base, m->bytes, m->size);
    memcpy(cd->bytes, m->bytes, m->size);
    cd->block_size = 1024;
}

void cut_free(struct cutdev *cd)
{
    free(cd->base);
    free(cd->bytes);
    free(cd->touched);
    free(cd->mark);
    free(cd->pmark);
    free(cd->pend);
    free(cd->pold);
}

/* Every block written since the last flush is on the medium now. */
static void forget_pending(struct cutdev *cd)
{
    for (uint64_t i = 0; i < cd->npend; i++)
        cd->pmark[cd->pend[i]] = 0;
    cd->npend = 0;
}

void cut_reset(struct cutdev *cd)
{
    forget_pending(cd);
    cd->flushes = 0;
    for (uint64_t i = 0; i < cd->ntouched; i++) {
        uint64_t k = cd->touched[i];
        memcpy(cd->bytes + k * CUT_UNIT, cd->base + k * CUT_UNIT, CUT_UNIT);
        cd->mark[k] = 0;
    }
    cd->ntouched = 0;
    cd->units = cd->cut_at = 0;
    cd->dead = false;
    cd->bad_writes = 0;
    cd->commits_alone = cd->commits_with = 0;
}

void cut_rebase(struct cutdev *cd)
{
    for (uint64_t i = 0; i < cd->ntouched; i++) {
        uint64_t k = cd->touched[i];
        memcpy(cd->base + k * CUT_UNIT, cd->bytes + k * CUT_UNIT, CUT_UNIT);
        cd->mark[k] = 0;
    }
    cd->ntouched = 0;
    cut_reset(cd);
}

static void put(struct cutdev *cd, uint64_t off, const uint8_t *p, uint32_t len)
{
    for (uint64_t k = off / CUT_UNIT; k < (off + len) / CUT_UNIT; k++)
        if (!cd->mark[k]) {
            cd->mark[k] = 1;
            cd->touched[cd->ntouched++] = (uint32_t)k;
        }
    memcpy(cd->bytes + off, p, len);
}

void cut_poke(struct cutdev *cd, uint64_t off, const void *p, size_t n)
{
    if (off > cd->size || n > cd->size - off)
        fail("cut device: a poke past the end");
    for (uint64_t k = off / CUT_UNIT; k <= (off + n - 1) / CUT_UNIT; k++)
        if (!cd->mark[k]) {
            cd->mark[k] = 1;
            cd->touched[cd->ntouched++] = (uint32_t)k;
        }
    memcpy(cd->bytes + off, p, n);
}

static status_t cut_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    struct cutdev *cd = ctx;
    if (!len || off % CUT_UNIT || len % CUT_UNIT || off > cd->size || len > cd->size - off)
        fail("cut device: a read of %u bytes at %llu", len, (unsigned long long)off);
    memcpy(buf, cd->bytes + off, len);
    return OK;
}

/* `watch`: a write must land inside the volume and outside the journal's
 * log (its superblock, log block 0, may be written). */
static void watch(struct cutdev *cd, uint64_t off, uint32_t len)
{
    uint64_t first = off / cd->block_size, n = len / cd->block_size;
    for (uint64_t b = first; b < first + n; b++)
        if (b > cd->jstart && b < cd->jstart + cd->jlen)
            cd->bad_writes++;
}

/* A write into the journal that holds a commit block: alone, or with others? */
static void count_commits(struct cutdev *cd, uint64_t off, uint32_t len, const uint8_t *buf)
{
    uint64_t first = off / cd->block_size, n = len / cd->block_size;
    if (!cd->jlen || first + n <= cd->jstart || first >= cd->jstart + cd->jlen)
        return;
    for (uint64_t i = 0; i < n; i++) {
        const struct jbd2_header *h = (const void *)(buf + i * cd->block_size);
        if (jbd2_be32(h->magic) == JBD2_MAGIC && jbd2_be32(h->blocktype) == JBD2_COMMIT) {
            cd->commits_alone += n == 1;
            cd->commits_with += n > 1;
            cd->dead |= cd->die_on_commit;
            return;
        }
    }
}

/* `cache`: blocks [off, off + len) are about to be written: each not yet
 * written since the last flush is noted with what the medium holds. */
static void note_pending(struct cutdev *cd, uint64_t off, uint32_t len)
{
    uint32_t bs = cd->block_size;
    for (uint64_t b = off / bs; b < (off + len) / bs; b++) {
        if (!cd->cache || cd->pmark[b])
            continue;
        if (cd->npend == cd->cappend) {
            cd->cappend = cd->cappend ? 2 * cd->cappend : 256;
            cd->pend = realloc(cd->pend, cd->cappend * sizeof(uint64_t));
            cd->pold = realloc(cd->pold, cd->cappend * bs);
            if (!cd->pend || !cd->pold)
                fail("out of memory");
        }
        cd->pmark[b] = 1;
        cd->pend[cd->npend] = b;
        memcpy(cd->pold + cd->npend * bs, cd->bytes + b * bs, bs);
        cd->npend++;
    }
}

/* `cache`, the power gone: each block written since the last flush is on
 * the medium as last written, or as it was before (the cache never wrote
 * it back). */
static void lose_pending(struct cutdev *cd)
{
    uint32_t bs = cd->block_size;
    for (uint64_t i = 0; i < cd->npend; i++)
        if (below(2))
            put(cd, cd->pend[i] * bs, cd->pold + i * bs, bs);
    forget_pending(cd);
}

static status_t cut_flush(void *ctx)
{
    struct cutdev *cd = ctx;
    if (cd->dead)
        return ERR_IO;
    forget_pending(cd);
    cd->flushes++;
    return OK;
}

static status_t cut_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    struct cutdev *cd = ctx;
    uint32_t bs = cd->block_size;
    if (!len || off % CUT_UNIT || len % bs || off > cd->size || len > cd->size - off)
        fail("cut device: a write of %u bytes at %llu", len, (unsigned long long)off);
    if (cd->watch)
        watch(cd, off, len);
    if (cd->dead)
        return ERR_IO;
    uint32_t n = len / bs;
    if (!cd->cut_at || cd->units + n <= cd->cut_at) {
        note_pending(cd, off, len);
        put(cd, off, buf, len);
        cd->units += n;
        count_commits(cd, off, len, buf);
        return OK;
    }
    /* The cut falls inside this write: k of its n blocks land. */
    uint32_t k = (uint32_t)(cd->cut_at - cd->units), order[1024];
    if (n > 1024)
        fail("cut device: a write of %u blocks", n);
    for (uint32_t i = 0; i < n; i++)
        order[i] = i;
    for (uint32_t i = 0; cd->reorder && i < k; i++) {
        uint32_t pick = i + (uint32_t)below(n - i), t = order[i];
        order[i] = order[pick];
        order[pick] = t;
    }
    for (uint32_t i = 0; i < k; i++) {
        note_pending(cd, off + (uint64_t)order[i] * bs, bs);
        put(cd, off + (uint64_t)order[i] * bs, (const uint8_t *)buf + (size_t)order[i] * bs, bs);
    }
    if (cd->cache)
        lose_pending(cd);
    cd->units = cd->cut_at;
    cd->dead = true;
    return ERR_IO;
}

struct ext_dev cut_dev(struct cutdev *cd)
{
    return (struct ext_dev){ .ctx = cd, .bytes = cd->size, .read = cut_read, .write = cut_write,
                             .flush = cd->cache ? cut_flush : NULL };
}

/* ---- blocks' contents --------------------------------------------------------------------- */

void storm_pattern(uint8_t *block, uint32_t size, uint64_t value)
{
    for (uint32_t i = 0; i < size; i += 8) {
        uint64_t w = value ^ (i * 0x9E3779B97F4A7C15ull);
        memcpy(block + i, &w, 8);
    }
    if (value & PATTERN_MAGIC) {
        /* What a commit block of some transaction begins with: unescaped in
         * the log, a replay walking past the end would believe it. */
        uint32_t head[3] = { jbd2_be32(JBD2_MAGIC), jbd2_be32(JBD2_COMMIT),
                             jbd2_be32((uint32_t)value) };
        memcpy(block, head, sizeof(head));
    }
}

uint64_t block_hash(const uint8_t *block, uint32_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < size; i++)
        h = (h ^ block[i]) * 0x100000001b3ull;
    return h;
}

/* ---- the storm's objects ------------------------------------------------------------------ */

static uint32_t add_obj(struct storm *s, struct obj o)
{
    if (s->nobj == STORM_OBJS)
        fail("too many storm objects");
    s->objs[s->nobj] = o;
    return s->nobj++;
}

/* Regular files in directory `dir`, up to max: their inodes and names. */
static uint32_t files_in(struct ext_fs *fs, uint32_t dir, uint32_t *inos, char (*names)[64],
                         uint32_t max)
{
    struct ext_inode in;
    struct ext_dentry d;
    uint32_t n = 0;
    if (ext_iget(fs, dir, 0, &in) != OK)
        fail("storm: directory %u", dir);
    for (uint64_t pos = 0; n < max;) {
        if (ext_dir_next(fs, &in, &pos, &d) != OK)
            fail("storm: readdir of %u", dir);
        if (!d.ino)
            break;
        bool seen = false;
        for (uint32_t i = 0; i < n; i++)
            seen |= inos[i] == d.ino;
        if (d.type != EXT_FT_REG || seen || d.name_len >= 64)
            continue;
        inos[n] = d.ino;
        snprintf(names[n++], 64, "%.63s", d.name);
    }
    return n;
}

/* Free blocks of the volume, every step-th of a group, groups 1 and up first. */
uint32_t free_blocks(struct ext_fs *fs, uint64_t *out, uint32_t max, uint32_t step)
{
    uint32_t n = 0;
    for (uint32_t gi = 0; gi < fs->groups && n < max; gi++) {
        uint32_t g = (gi + 1) % fs->groups;
        struct ext_buf *b;
        if (ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b) != OK)
            continue;
        for (uint32_t i = 0; i < ext_group_blocks(fs, g) && n < max; i += step)
            if (!(b->data[i / 8] & (1u << (i % 8))))
                out[n++] = ext_group_first(fs, g) + i;
        ext_brelse(fs, b);
    }
    return n;
}

/* Give the journal `maxlen` log blocks (its superblock has no checksum yet). */
static void shrink_journal(struct storm *s, struct ext_fs *fs, uint32_t maxlen)
{
    struct ext_inode in;
    struct ext_run r;
    if (ext_iget(fs, fs->sb.journal_inum, 0, &in) != OK || ext_map(fs, &in, 0, &r) != OK)
        fail("storm: the journal");
    uint32_t v = jbd2_be32(maxlen);
    uint64_t off = (r.pblk << fs->block_log) + __builtin_offsetof(struct jbd2_super, maxlen);
    memcpy(s->cd.base + off, &v, 4);
    memcpy(s->cd.bytes + off, &v, 4);
}

/* The objects: files in the root, entry pairs and files in /many, free
 * blocks, the superblock. */
static void pick_objects(struct storm *s, struct ext_fs *fs)
{
    uint32_t inos[160];
    char names[160][64];
    uint32_t n = files_in(fs, EXT_ROOT_INO, inos, names, 12);
    s->o_root = s->nobj;
    s->n_root = n;
    for (uint32_t i = 0; i < n; i++)
        add_obj(s, (struct obj){ .kind = OBJ_INODE, .ino = inos[i] });
    struct ext_inode root;
    struct ext_dentry many;
    if (ext_iget(fs, EXT_ROOT_INO, 0, &root) != OK ||
        ext_dir_lookup(fs, &root, "many", 4, &many) != OK)
        fail("storm: no /many");
    n = files_in(fs, many.ino, inos, names, 160);
    if (n < 40)
        fail("storm: /many has %u files", n);
    s->o_swap = s->nobj;
    for (uint32_t i = 0; i < 4; i++) {
        struct obj o = { .kind = OBJ_SWAP, .ino = many.ino };
        snprintf(o.na, sizeof(o.na), "%.63s", names[i]);
        snprintf(o.nb, sizeof(o.nb), "%.63s", names[n - 1 - i]);
        add_obj(s, o);
    }
    s->o_many = s->nobj;
    for (uint32_t i = 4; i < 28; i++)
        add_obj(s, (struct obj){ .kind = OBJ_INODE, .ino = inos[i] });
    /* The block objects are blocks of /holes (one-block extents): owned by
     * a file, so no allocation takes them and the checkers see owners. */
    struct ext_inode holes;
    lookup(fs, "holes", &holes);
    s->holes_ino = holes.ino;
    s->o_block = s->nobj;
    for (uint32_t i = 0; i < 12; i++) {
        struct ext_run r;
        if (ext_map(fs, &holes, 2 * i, &r) != OK || r.kind != EXT_RUN_DATA)
            fail("storm: /holes's block %u", 2 * i);
        add_obj(s, (struct obj){ .kind = OBJ_BLOCK, .blk = r.pblk });
    }
    s->o_wtime = add_obj(s, (struct obj){ .kind = OBJ_WTIME });
    static const char *const files[] = { "pattern", "block", "block1", "Makefile" };
    s->o_file = s->nobj;
    s->n_file = 4;
    for (uint32_t i = 0; i < 4; i++) {
        struct ext_inode in;
        lookup(fs, files[i], &in);
        add_obj(s, (struct obj){ .kind = OBJ_FILE, .ino = in.ino });
    }
}

/* ---- the storm's requests ----------------------------------------------------------------- */

static void add_op(struct request *q, uint8_t kind, uint32_t obj, uint64_t value)
{
    if (q->nops == STORM_OPS)
        fail("too many ops in a request");
    q->ops[q->nops++] = (struct op){ .kind = kind, .obj = (uint16_t)obj, .value = value };
}

/* Does the request already have an op of this kind? */
static bool has_op(const struct request *q, uint8_t kind)
{
    for (uint32_t i = 0; i < q->nops; i++)
        if (q->ops[i].kind == kind)
            return true;
    return false;
}

/* A random change: an inode's times or mode, an exchange, a metadata
 * block, a file's end written or cut. A request frees blocks (a cut) or
 * writes new data, never both: blocks freed in a transaction are free in
 * the cache's bitmaps at once, so data written into them before the
 * commit would land in blocks the committed volume still gives the cut
 * file (ext_alloc.h; the service keeps to this). */
static void random_op(struct storm *s, struct request *q)
{
    static const uint16_t modes[] = { 0600, 0644, 0755, 0700, 0640, 04755 };
    uint32_t pick = (uint32_t)below(6);
    if ((pick == 4 && has_op(q, OP_CUT)) || (pick == 5 && has_op(q, OP_APPEND)))
        pick = 0;
    switch (pick) {
    case 4:
        add_op(q, OP_APPEND, s->o_file + (uint32_t)below(s->n_file),
               rnd() << 16 | (1 + below(3 * s->cd.block_size)));
        break;
    case 5:
        add_op(q, OP_CUT, s->o_file + (uint32_t)below(s->n_file), 0);
        break;
    case 0:
        add_op(q, OP_TOUCH, s->o_root + (uint32_t)below(s->n_root), 1600000000 + below(1u << 28));
        break;
    case 1:
        add_op(q, OP_CHMOD, s->o_root + (uint32_t)below(s->n_root), modes[below(6)]);
        break;
    case 2:
        add_op(q, OP_SWAP, s->o_swap + (uint32_t)below(4), 0);
        break;
    default:
        add_op(q, OP_META, s->o_block + (uint32_t)below(8),
               (rnd() & ~(3ull << 62)) | (below(4) ? 0 : PATTERN_MAGIC));
        break;
    }
}

/* The requests that test a rule, at fixed places in the storm (blocks 8
 * to 11 are theirs). */
static void special_ops(struct storm *s, struct request *q, uint32_t at)
{
    uint32_t a = s->o_block + 8, b = a + 1, c = a + 2, d = a + 3;
    switch (at) {
    case 2:   /* the freed-block hazard: journaled, freed, then file data */
        add_op(q, OP_META, a, 0x1111);
        break;
    case 5:
        add_op(q, OP_FREE, a, 0);
        break;
    case 6:
        add_op(q, OP_DATA, a, 0x2222);
        break;
    case 7:   /* journaled, for 8 */
        add_op(q, OP_META, b, 0x3333);
        break;
    case 8:   /* freed and written as metadata again in one transaction */
        add_op(q, OP_FREE, b, 0);
        add_op(q, OP_META, b, 0x4444);
        break;
    case 10:  /* made, freed and written as data in one transaction (never
               * committed as metadata: data into a block whose free is not
               * committed is the writers' rule above) */
        add_op(q, OP_META, c, 0x5556);
        add_op(q, OP_FREE, c, 0);
        add_op(q, OP_DATA, c, 0x6666);
        break;
    case 12:  /* a big operation in steps */
        for (uint32_t i = 0; i < 24; i++) {
            add_op(q, OP_STEP, 0, 2);
            add_op(q, OP_TOUCH, s->o_many + i, 1700000000 + 1000 * at + i);
        }
        break;
    case 14:  /* journaled, freed by a later transaction, metadata again after */
        add_op(q, OP_META, d, 0x7777 | PATTERN_MAGIC);
        break;
    case 16:
        add_op(q, OP_FREE, d, 0);
        break;
    case 18:
        add_op(q, OP_META, d, 0x8888);
        break;
    default:
        break;
    }
}

void storm_make(struct storm *s, const struct storm_cfg *cfg, const struct img *m)
{
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    if (cfg->mkfs_bs) {
        names_make(s, cfg);
        return;
    }
    cut_init(&s->cd, m);
    s->cd.cache = cfg->cache;
    struct ext_fs fs;
    struct ext_dev dev = cut_dev(&s->cd);
    uint8_t *cache = malloc(CACHE_BYTES);
    if (!cache || ext_mount(&fs, &dev, cache, CACHE_BYTES, EXT_MOUNT_READ_ONLY) != OK)
        fail("storm: mount");
    s->cd.block_size = fs.block_size;
    pick_objects(s, &fs);
    if (cfg->maxlen)
        shrink_journal(s, &fs, cfg->maxlen);
    free(cache);
    rng = cfg->seed | 1;
    s->nreq = cfg->nreq;
    if (s->nreq > STORM_REQS)
        fail("storm: %u requests", s->nreq);
    for (uint32_t at = 0; at < s->nreq; at++) {
        struct request *q = &s->req[at];
        special_ops(s, q, at);
        for (uint32_t k = 1 + (uint32_t)below(4); k; k--)
            random_op(s, q);
    }
}

/* ---- running it ------------------------------------------------------------------------- */

static void on_commit(void *ctx, uint32_t tid)
{
    struct run *r = ctx;
    r->last_tid = tid;
    r->attempted = true;
}

status_t run_mount(struct storm *s, struct run *r)
{
    memset(r, 0, sizeof(*r));
    size_t jsize = ext_jnl_mem_size(4096, 1024, 4096);
    r->cache = malloc(CACHE_BYTES);
    r->jmem = malloc(jsize);
    if (!r->cache || !r->jmem)
        fail("out of memory");
    status_t st = ext_jnl_init(&r->j, r->jmem, jsize, s->cfg->jflags);
    if (st != OK)
        fail("ext_jnl_init: %d", st);
    if (s->cfg->tail_every)
        r->j.tail_every = s->cfg->tail_every;
    if (s->cfg->tail_blocks)
        r->j.tail_blocks = s->cfg->tail_blocks;
    r->j.on_commit = on_commit;
    r->j.ctx = r;
    r->j.commit_sec = 1760000000;
    struct ext_dev dev = cut_dev(&s->cd);
    st = ext_jnl_mount(&r->fs, &r->j, &dev, r->cache, CACHE_BYTES, 0);
    r->tid0 = r->j.tid;
    return st;
}

void run_free(struct run *r)
{
    free(r->cache);
    free(r->jmem);
    r->cache = r->jmem = NULL;
}

/* The entry `name` of directory dir: its block, pinned, and its offset. */
static status_t find_entry(struct ext_fs *fs, struct ext_inode *dir, const char *name,
                           struct ext_buf **out, uint32_t *off)
{
    uint32_t nblk = (uint32_t)(ext_isize(dir) >> fs->block_log);
    size_t len = strlen(name);
    for (uint32_t l = 0; l < nblk; l++) {
        struct ext_buf *b;
        enum ext_dirblk kind;
        status_t st = ext_dir_block(fs, dir, l, &b, &kind);
        if (st != OK)
            return st;
        for (uint32_t at = 0; kind == EXT_DIRBLK_LEAF && at + EXT_DIRENT_HEADER <= fs->block_size;) {
            struct ext_dirent *d = (void *)(b->data + at);
            uint32_t rl = ext_rec_len(fs, d->rec_len);
            if (d->inode && d->name_len == len && !memcmp(d->name, name, len)) {
                *out = b;
                *off = at;
                return OK;
            }
            if (!rl)
                break;
            at += rl;
        }
        ext_brelse(fs, b);
    }
    return ERR_NOT_FOUND;
}

/* The two names exchange their inodes (both regular files): a rename that
 * may change two directory blocks. */
static status_t swap_names(struct ext_fs *fs, const struct obj *o)
{
    struct ext_inode dir;
    struct ext_buf *ba, *bb;
    uint32_t oa, ob;
    status_t st = ext_iget(fs, o->ino, 0, &dir);
    if (st == OK)
        st = find_entry(fs, &dir, o->na, &ba, &oa);
    if (st != OK)
        return st;
    st = find_entry(fs, &dir, o->nb, &bb, &ob);
    if (st != OK) {
        ext_brelse(fs, ba);
        return st;
    }
    struct ext_dirent *da = (void *)(ba->data + oa), *db = (void *)(bb->data + ob);
    uint32_t t = da->inode;
    da->inode = db->inode;
    db->inode = t;
    ext_dirleaf_csum_set(fs, dir.seed, ba->data);
    ext_bdirty(fs, ba);
    if (bb != ba) {
        ext_dirleaf_csum_set(fs, dir.seed, bb->data);
        ext_bdirty(fs, bb);
    }
    ext_brelse(fs, ba);
    ext_brelse(fs, bb);
    return OK;
}

static status_t set_inode(struct ext_fs *fs, uint32_t ino, uint8_t kind, uint64_t value)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, ino, 0, &in);
    if (st != OK)
        return st;
    if (kind == OP_CHMOD) {
        in.d.mode = (uint16_t)((in.d.mode & EXT_S_IFMT) | value);
    } else {
        struct ext_time t = { .sec = (int64_t)value, .nsec = 0 };
        ext_itime_set(&in, EXT_MTIME, &t);
        ext_itime_set(&in, EXT_CTIME, &t);
    }
    return ext_iput(fs, &in);
}

static status_t write_block(struct storm *s, struct run *r, uint32_t obj, uint8_t kind,
                            uint64_t value)
{
    struct ext_fs *fs = &r->fs;
    uint64_t blk = s->objs[obj].blk;
    if (kind == OP_META) {
        struct ext_buf *b;
        status_t st = ext_bnew(fs, blk, &b);
        if (st != OK)
            return st;
        storm_pattern(b->data, fs->block_size, value);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
        return OK;
    }
    uint8_t buf[4096];
    storm_pattern(buf, fs->block_size, value);
    status_t st = fs->dev.write(fs->dev.ctx, blk << fs->block_log, fs->block_size, buf);
    if (st != OK)
        return st;
    r->landed[obj] = block_hash(buf, fs->block_size);
    r->has_landed[obj] = true;
    ext_binval(fs, blk, 1);
    return OK;
}

/* U2a's writers on a file: value's low 16 bits of bytes appended (its
 * high bits the pattern), or the file cut to half its size in whole blocks
 * (all of the cut in this transaction: no step between leaves blocks past
 * the size, and no byte inside the old size changes before the commit). */
static status_t change_file(struct ext_fs *fs, uint32_t ino, uint8_t kind, uint64_t value)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, ino, 0, &in);
    if (st != OK)
        return st;
    uint64_t size = ext_isize(&in);
    if (kind == OP_APPEND) {
        static uint8_t buf[3 * 4096];
        size_t len = (size_t)(value & 0xFFFF), n;
        storm_pattern(buf, (uint32_t)((len + 7) & ~(size_t)7), value >> 16);
        return ext_write(fs, &in, size, buf, len, &n);
    }
    uint64_t to = (size / 2) & ~(uint64_t)(fs->block_size - 1);
    bool done = false;
    while (st == OK && !done)
        st = ext_truncate(fs, &in, to, &done);
    return st;
}

void take_snaps(struct storm *s, struct run *r)
{
    while (s->nsnap <= r->j.tid - r->tid0) {
        if (s->nsnap == STORM_SNAPS)
            fail("%s: more than %d commits", s->cfg->name, STORM_SNAPS);
        storm_state(s, r, s->snap[s->nsnap++]);
    }
}

static status_t do_op(struct storm *s, struct run *r, const struct op *o, bool snap)
{
    status_t st;
    switch (o->kind) {
    case OP_TOUCH:
    case OP_CHMOD:
        return set_inode(&r->fs, s->objs[o->obj].ino, o->kind, o->value);
    case OP_SWAP:
        return swap_names(&r->fs, &s->objs[o->obj]);
    case OP_META:
    case OP_DATA:
        return write_block(s, r, o->obj, o->kind, o->value);
    case OP_FREE:
        return ext_jnl_freed(&r->fs, s->objs[o->obj].blk, 1);
    case OP_APPEND:
    case OP_CUT:
        return change_file(&r->fs, s->objs[o->obj].ino, o->kind, o->value);
    case OP_STEP:
        st = ext_jnl_step(&r->fs, (uint32_t)o->value);
        if (st == OK && snap)
            take_snaps(s, r);
        return st;
    default:
        fail("an op of kind %u", o->kind);
    }
}

void storm_run(struct storm *s, struct run *r, bool snap)
{
    if (snap)
        take_snaps(s, r);
    if (s->cfg->mkfs_bs) {
        names_run(s, r, snap);
        return;
    }
    for (uint32_t at = 0; at < s->nreq; at++) {
        const struct request *q = &s->req[at];
        status_t st = ext_jnl_begin(&r->fs, NULL);
        if (st == OK) {
            r->fs.sb.wtime = 1750000000 + at;
            st = ext_super_write(&r->fs);
        }
        for (uint32_t i = 0; st == OK && i < q->nops; i++)
            st = do_op(s, r, &q->ops[i], snap);
        if (st == OK)
            st = ext_jnl_commit(&r->fs);
        if (st != OK) {
            r->why_stop = st;
            return;
        }
        if (snap)
            take_snaps(s, r);
    }
    r->why_stop = OK;
}

void storm_state(struct storm *s, struct run *r, uint64_t *out)
{
    struct ext_fs *fs = &r->fs;
    for (uint32_t i = 0; i < s->nobj; i++) {
        const struct obj *o = &s->objs[i];
        struct ext_inode in;
        struct ext_dentry d;
        struct ext_time t;
        switch (o->kind) {
        case OBJ_INODE:
            if (ext_iget(fs, o->ino, 0, &in) != OK)
                fail("%s: inode %u: %s", s->cfg->name, o->ino, fs->why ? fs->why : "");
            ext_itime(&in, EXT_MTIME, &t);
            out[i] = (uint64_t)t.sec << 16 | in.d.mode;
            break;
        case OBJ_SWAP:
            if (ext_iget(fs, o->ino, 0, &in) != OK ||
                ext_dir_lookup(fs, &in, o->na, strlen(o->na), &d) != OK)
                fail("%s: %s: %s", s->cfg->name, o->na, fs->why ? fs->why : "");
            out[i] = d.ino;
            break;
        case OBJ_BLOCK:
            out[i] = block_hash(s->cd.bytes + (o->blk << fs->block_log), fs->block_size);
            break;
        case OBJ_LISTING:
            out[i] = listing_hash(fs);
            break;
        case OBJ_FILE: {
            char hex[65];
            if (ext_iget(fs, o->ino, 0, &in) != OK)
                fail("%s: file %u: %s", s->cfg->name, o->ino, fs->why ? fs->why : "");
            sparse_hash(fs, &in, hex);
            out[i] = strtoull(hex + 48, NULL, 16) ^ ext_isize(&in) * 0x9E3779B97F4A7C15ull ^
                     (uint64_t)in.d.blocks_lo << 40;
            break;
        }
        default:
            out[i] = fs->sb.wtime;
            break;
        }
    }
    check_pins(fs);
}
