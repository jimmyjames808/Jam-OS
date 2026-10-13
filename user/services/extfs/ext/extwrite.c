/* ext: changing an extent tree (extent.c has the tree and its walk).
 *
 * What a writer keeps true, as a reader checks it (extent.c) and Linux's
 * e2fsck does: every node's entries in strictly increasing order of
 * logical block, extents not overlapping, every block node holding at
 * least one entry, each index entry's block equal to its child's first
 * entry's block (so a node's range starts where its key says), a node's
 * depth one less than its parent's, a node's checksum set before its
 * block is marked dirty (a dirty block is checked again when next read),
 * and i_blocks counting every node block with the data.
 *
 * Insert: walk to the leaf for the new run's first block. If the run
 * continues the extent before it (the next logical block on the next
 * physical block, both written or both unwritten, and the sum within an
 * extent's length) it is merged into it, likewise into the extent after
 * it, or into both, which then become one. Else it is put in the leaf,
 * which needs room: make_room splits the full node nearest the root
 * whose parent has room (half its entries to a new block, or only its
 * last when the run goes after everything, so a file written from start
 * to end leaves its nodes full), or grows the tree by a level when the
 * root and every node below it on the path are full (the root's entries
 * move to a new block, the root keeps one index entry). Then it walks
 * again; a level gets room each time, so this ends within the depth.
 *
 * Punch: removes a range from its end backwards, an extent at a time:
 * one inside the range is removed (a node left empty is freed and its
 * index entry removed in turn), one crossing an end of the range is cut,
 * one around the whole range is cut to its first part and its last part
 * inserted as a new extent. Then a root holding a single child with no
 * more entries than the root holds takes that child's entries, and the
 * tree is a level shorter (an empty tree is a leaf again). */
#include "ext_alloc_int.h"

uint16_t ext_extent_test_room;

#define ROOT_ROOM     4u                          /* entries in i_block */
#define MAX_WRITTEN   EXT_INIT_MAX_LEN            /* an extent's blocks, written ... */
#define MAX_UNWRITTEN (EXT_INIT_MAX_LEN - 1)      /* ... and unwritten */
#define MAX_TRIES     (2 * (EXT_EXTENT_MAX_DEPTH + 2))

/* Entries a new node in a block has room for. */
static uint16_t block_room(const struct ext_fs *fs)
{
    uint32_t r = (fs->block_size - sizeof(struct ext_extent_header)) / sizeof(struct ext_extent);
    if (ext_extent_test_room >= ROOT_ROOM && ext_extent_test_room < r)
        r = ext_extent_test_room;
    return (uint16_t)r;
}

static uint32_t len_max(bool unwritten)
{
    return unwritten ? MAX_UNWRITTEN : MAX_WRITTEN;
}

void ext_extent_init(struct ext_inode *in)
{
    __builtin_memset(in->d.block, 0, sizeof(in->d.block));
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    h->magic = EXT_EXTENT_MAGIC;
    h->max = ROOT_ROOM;
    in->d.flags |= EXT_EXTENTS_FL;
}

static void set_extent(struct ext_extent *e, uint32_t lblk, uint64_t pblk, uint32_t len,
                       bool unwritten)
{
    e->block = lblk;
    e->start_lo = (uint32_t)pblk;
    e->start_hi = (uint16_t)(pblk >> 32);
    e->len = (uint16_t)(unwritten ? len + EXT_INIT_MAX_LEN : len);
}

static uint64_t extent_end(const struct ext_extent *e)
{
    return (uint64_t)e->block + ext_extent_len(e);
}

/* Node lv of the path changed: its checksum set and its block dirty (the
 * root is in the inode: the caller's ext_iput). */
static void touch(struct ext_fs *fs, const struct ext_inode *in, struct ext_path *p, uint32_t lv)
{
    struct ext_buf *b = p->lv[lv].buf;
    if (!b)
        return;
    if (fs->csum)
        (void)ext_extent_csum_set(fs, in->seed, b->data);   /* max was checked: the tail fits */
    ext_bdirty(fs, b);
}

/* Node lv's first entry changed: the index entries above it follow. */
static void fix_keys(struct ext_fs *fs, const struct ext_inode *in, struct ext_path *p,
                     uint32_t lv)
{
    for (; lv > 0 && p->lv[lv].h->entries; lv--) {
        uint32_t key = ext_extents(p->lv[lv].h)[0].block;   /* extents and indexes alike */
        struct ext_extent_idx *ix = &ext_idxs(p->lv[lv - 1].h)[p->lv[lv - 1].at];
        if (ix->block == key)
            return;
        ix->block = key;
        touch(fs, in, p, lv - 1);
        if (p->lv[lv - 1].at)
            return;
    }
}

/* Put a 12-byte entry at pos of node h (it has room), or take one out. */
static void put_entry(struct ext_extent_header *h, uint32_t pos, const void *entry)
{
    struct ext_extent *e = ext_extents(h);
    __builtin_memmove(&e[pos + 1], &e[pos], (h->entries - pos) * sizeof(*e));
    __builtin_memcpy(&e[pos], entry, sizeof(*e));
    h->entries++;
}

static void drop_entry(struct ext_extent_header *h, uint32_t pos)
{
    struct ext_extent *e = ext_extents(h);
    __builtin_memmove(&e[pos], &e[pos + 1], (h->entries - pos - 1) * sizeof(*e));
    h->entries--;
    __builtin_memset(&e[h->entries], 0, sizeof(*e));
}

/* ---- new nodes ------------------------------------------------------------------------- */

/* A block for a new node of in's tree, near `goal`, counted in i_blocks,
 * pinned and zeroed. */
static status_t new_node(struct ext_fs *fs, struct ext_inode *in, uint64_t goal,
                         struct ext_buf **out)
{
    uint64_t blk;
    uint32_t n;
    status_t st = ext_balloc(fs, goal, 1, &blk, &n);
    if (st != OK)
        return st;
    st = ext_iblocks_add(fs, in, 1);
    if (st == OK)
        st = ext_bnew(fs, blk, out);
    if (st != OK)
        (void)ext_bfree(fs, blk, 1);   /* taken a moment ago: it can't fail but by damage */
    return st;
}

/* Where a new node goes: after the deepest node of the path, or the data. */
static uint64_t node_goal(const struct ext_fs *fs, const struct ext_inode *in,
                          const struct ext_path *p)
{
    for (int32_t lv = (int32_t)p->depth; lv > 0; lv--)
        if (p->lv[lv].buf)
            return p->lv[lv].buf->blk + 1;
    const struct ext_extent_header *root = (const struct ext_extent_header *)in->d.block;
    if (!root->depth && root->entries)
        return ext_extent_start(&ext_extents((struct ext_extent_header *)root)[0]);
    return ext_group_first(fs, (in->ino - 1) / fs->inodes_per_group);
}

/* Seal a new node: its header, then its checksum and dirty mark. */
static void seal_node(struct ext_fs *fs, const struct ext_inode *in, struct ext_buf *b,
                      uint16_t entries, uint16_t depth)
{
    struct ext_extent_header *h = (struct ext_extent_header *)b->data;
    h->magic = EXT_EXTENT_MAGIC;
    h->entries = entries;
    h->max = block_room(fs);
    h->depth = depth;
    h->generation = 0;
    if (fs->csum)
        (void)ext_extent_csum_set(fs, in->seed, b->data);
    ext_bdirty(fs, b);
}

/* The root and every node under it on the path are full: move the root's
 * entries to a new block, and make the root one index entry above it. */
static status_t grow(struct ext_fs *fs, struct ext_inode *in, struct ext_path *p)
{
    struct ext_extent_header *root = p->lv[0].h;
    if (root->depth >= EXT_EXTENT_MAX_DEPTH)
        return ERR_NO_RESOURCES;
    struct ext_buf *b;
    status_t st = new_node(fs, in, node_goal(fs, in, p), &b);
    if (st != OK)
        return st;
    struct ext_extent_header *h = (struct ext_extent_header *)b->data;
    __builtin_memcpy(ext_extents(h), ext_extents(root), root->entries * sizeof(struct ext_extent));
    seal_node(fs, in, b, root->entries, root->depth);
    struct ext_extent_idx ix = { .block = ext_extents(root)[0].block, .leaf_lo = (uint32_t)b->blk,
                                 .leaf_hi = (uint16_t)(b->blk >> 32), .unused = 0 };
    ext_brelse(fs, b);
    __builtin_memset(ext_extents(root), 0, root->max * sizeof(struct ext_extent));
    root->entries = 0;
    root->depth++;
    put_entry(root, 0, &ix);
    return OK;
}

/* Split node lv (in a block; its parent has room): its upper entries to a
 * new block, and an index entry for that block in the parent. */
static status_t split(struct ext_fs *fs, struct ext_inode *in, struct ext_path *p, uint32_t lv)
{
    struct ext_extent_header *h = p->lv[lv].h;
    uint32_t n = h->entries;
    if (n < 2)
        return ext_damaged(fs, "extent: a full node of one entry", in->ino, p->lv[lv].buf->blk);
    uint32_t keep = (uint32_t)p->lv[lv].at == n - 1 ? n - 1 : n / 2;
    struct ext_buf *b;
    status_t st = new_node(fs, in, p->lv[lv].buf->blk + 1, &b);
    if (st != OK)
        return st;
    struct ext_extent_header *nh = (struct ext_extent_header *)b->data;
    __builtin_memcpy(ext_extents(nh), &ext_extents(h)[keep],
                     (n - keep) * sizeof(struct ext_extent));
    seal_node(fs, in, b, (uint16_t)(n - keep), h->depth);
    struct ext_extent_idx ix = { .block = ext_extents(nh)[0].block, .leaf_lo = (uint32_t)b->blk,
                                 .leaf_hi = (uint16_t)(b->blk >> 32), .unused = 0 };
    ext_brelse(fs, b);
    __builtin_memset(&ext_extents(h)[keep], 0, (n - keep) * sizeof(struct ext_extent));
    h->entries = (uint16_t)keep;
    touch(fs, in, p, lv);
    put_entry(p->lv[lv - 1].h, (uint32_t)p->lv[lv - 1].at + 1, &ix);
    touch(fs, in, p, lv - 1);
    return OK;
}

/* The path's leaf is full: give the lowest full level room (one step). */
static status_t make_room(struct ext_fs *fs, struct ext_inode *in, struct ext_path *p)
{
    int32_t lv = (int32_t)p->depth;
    while (lv >= 0 && p->lv[lv].h->entries >= p->lv[lv].h->max)
        lv--;
    if (lv < 0)
        return grow(fs, in, p);
    return split(fs, in, p, (uint32_t)lv + 1);
}

/* ---- insert ---------------------------------------------------------------------------- */

/* Does run (lblk, pblk, unw) start where extent e ends? */
static bool follows(const struct ext_extent *e, uint32_t lblk, uint64_t pblk, bool unw)
{
    return ext_extent_unwritten(e) == unw && extent_end(e) == lblk &&
           ext_extent_start(e) + ext_extent_len(e) == pblk;
}

/* Merge the run into the extents around it in the leaf: true if it was. */
static bool merge(struct ext_fs *fs, struct ext_inode *in, struct ext_path *p, uint32_t lblk,
                  const struct ext_run *r)
{
    struct ext_extent_header *h = p->lv[p->depth].h;
    struct ext_extent *e = ext_extents(h);
    int32_t at = p->lv[p->depth].at;
    bool unw = r->kind == EXT_RUN_UNWRITTEN;
    uint32_t max = len_max(unw);
    bool left = at >= 0 && follows(&e[at], lblk, r->pblk, unw) &&
                ext_extent_len(&e[at]) + r->len <= max;
    struct ext_extent *nx = (uint32_t)(at + 1) < h->entries ? &e[at + 1] : NULL;
    bool right = nx && ext_extent_unwritten(nx) == unw && lblk + r->len == nx->block &&
                 r->pblk + r->len == ext_extent_start(nx) && ext_extent_len(nx) + r->len <= max;
    if (left && right && ext_extent_len(&e[at]) + r->len + ext_extent_len(nx) <= max) {
        set_extent(&e[at], e[at].block, ext_extent_start(&e[at]),
                   ext_extent_len(&e[at]) + r->len + ext_extent_len(nx), unw);
        drop_entry(h, (uint32_t)at + 1);
    } else if (left) {
        set_extent(&e[at], e[at].block, ext_extent_start(&e[at]), ext_extent_len(&e[at]) + r->len,
                   unw);
    } else if (right) {
        set_extent(nx, lblk, r->pblk, r->len + ext_extent_len(nx), unw);
        if (at + 1 == 0)
            fix_keys(fs, in, p, p->depth);
    } else {
        return false;
    }
    touch(fs, in, p, p->depth);
    return true;
}

/* Is [lblk, lblk + len) free of extents, by the path to lblk's leaf? */
static bool is_hole(const struct ext_path *p, uint32_t lblk, uint32_t len)
{
    struct ext_extent_header *h = p->lv[p->depth].h;
    struct ext_extent *e = ext_extents(h);
    int32_t at = p->lv[p->depth].at;
    if (at >= 0 && extent_end(&e[at]) > lblk)
        return false;
    uint64_t next = p->bound ? p->bound : (uint64_t)1 << 32;
    if ((uint32_t)(at + 1) < h->entries)
        next = e[at + 1].block;
    return (uint64_t)lblk + len <= next;
}

/* One try: merge or put the run in its leaf (*out_done), or make room. */
static status_t insert_once(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                            const struct ext_run *r, bool *out_done)
{
    struct ext_path p;
    status_t st = ext_extent_find(fs, in, lblk, &p);
    if (st == OK && !is_hole(&p, lblk, r->len))
        st = ERR_ALREADY_EXISTS;
    if (st == OK && merge(fs, in, &p, lblk, r)) {
        *out_done = true;
    } else if (st == OK && p.lv[p.depth].h->entries < p.lv[p.depth].h->max) {
        struct ext_extent e;
        set_extent(&e, lblk, r->pblk, r->len, r->kind == EXT_RUN_UNWRITTEN);
        uint32_t pos = (uint32_t)(p.lv[p.depth].at + 1);
        put_entry(p.lv[p.depth].h, pos, &e);
        touch(fs, in, &p, p.depth);
        if (pos == 0)
            fix_keys(fs, in, &p, p.depth);
        *out_done = true;
    } else if (st == OK) {
        st = make_room(fs, in, &p);
    }
    ext_path_release(fs, &p);
    return st;
}

status_t ext_extent_insert(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                           const struct ext_run *r)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    bool unw = r->kind == EXT_RUN_UNWRITTEN;
    if (!(in->d.flags & EXT_EXTENTS_FL) || (r->kind != EXT_RUN_DATA && !unw) || !r->len ||
        r->len > len_max(unw) || !ext_run_ok(fs, r->pblk, r->len))
        return ERR_INVALID_ARGS;
    if ((uint64_t)lblk + r->len > (uint64_t)1 << 32)
        return ERR_OUT_OF_RANGE;
    bool done = false;
    for (uint32_t i = 0; st == OK && !done; i++)
        st = i < MAX_TRIES ? insert_once(fs, in, lblk, r, &done) : ERR_INTERNAL;
    status_t put = ext_iput(fs, in);   /* the root may have changed, even on a failure */
    return st != OK ? st : put;
}

/* ---- punch ----------------------------------------------------------------------------- */

/* Give back blocks [pblk, pblk + n) of in. */
static status_t free_run(struct ext_fs *fs, struct ext_inode *in, uint64_t pblk, uint64_t n)
{
    status_t st = ext_bfree(fs, pblk, n);
    if (st == OK)
        st = ext_iblocks_add(fs, in, -(int64_t)n);
    return st;
}

/* Take entry `at` out of node lv; a block node left empty is freed and its
 * entry in the parent taken out in turn. The path's levels from the
 * freed one down are released. */
static status_t remove_entry(struct ext_fs *fs, struct ext_inode *in, struct ext_path *p,
                             uint32_t lv)
{
    for (;;) {
        struct ext_extent_header *h = p->lv[lv].h;
        uint32_t at = (uint32_t)p->lv[lv].at;
        drop_entry(h, at);
        if (h->entries || lv == 0) {
            if (!h->entries)
                h->depth = 0;   /* the whole tree is empty: the root is a leaf again */
            touch(fs, in, p, lv);
            if (at == 0)
                fix_keys(fs, in, p, lv);
            return OK;
        }
        uint64_t blk = p->lv[lv].buf->blk;
        ext_brelse(fs, p->lv[lv].buf);
        p->lv[lv].buf = NULL;
        status_t st = free_run(fs, in, blk, 1);
        if (st != OK)
            return st;
        lv--;
    }
}

/* A root with one child that fits in it takes the child's entries. */
static status_t collapse(struct ext_fs *fs, struct ext_inode *in)
{
    struct ext_extent_header *root = (struct ext_extent_header *)in->d.block;
    while (root->depth && root->entries == 1) {
        struct ext_path p;
        status_t st = ext_extent_find(fs, in, 0, &p);
        struct ext_extent_header *child = st == OK ? p.lv[1].h : NULL;
        if (st != OK || child->entries > root->max) {
            ext_path_release(fs, &p);
            return st;
        }
        uint64_t blk = p.lv[1].buf->blk;
        __builtin_memcpy(ext_extents(root), ext_extents(child),
                         child->entries * sizeof(struct ext_extent));
        root->entries = child->entries;
        root->depth = child->depth;
        ext_path_release(fs, &p);
        st = free_run(fs, in, blk, 1);
        if (st != OK)
            return st;
    }
    return OK;
}

/* The extent at or before *end - 1 handled: removed, cut or split; *end
 * moves back to what is left of the range; *done when nothing is. */
static status_t punch_one(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint64_t *end,
                          bool *done)
{
    struct ext_path p;
    status_t st = ext_extent_find(fs, in, (uint32_t)(*end - 1), &p);
    int32_t at = st == OK ? p.lv[p.depth].at : -1;
    struct ext_extent *e = at >= 0 ? &ext_extents(p.lv[p.depth].h)[at] : NULL;
    if (st != OK || !e || extent_end(e) <= lblk) {
        ext_path_release(fs, &p);
        *done = st == OK;
        return st;
    }
    uint64_t eb = e->block, ee = extent_end(e), ps = ext_extent_start(e);
    bool unw = ext_extent_unwritten(e);
    if (eb < lblk && ee > *end && ext_free_blocks(fs) < (uint64_t)p.depth + 2) {
        st = ERR_NO_SPACE;   /* the second part's insert might need a node per level */
    } else if (eb < lblk) {   /* cut to its part before the range */
        set_extent(e, (uint32_t)eb, ps, (uint32_t)(lblk - eb), unw);
        touch(fs, in, &p, p.depth);
    } else if (ee > *end) {   /* cut to its part after the range */
        set_extent(e, (uint32_t)*end, ps + (*end - eb), (uint32_t)(ee - *end), unw);
        touch(fs, in, &p, p.depth);
        if (at == 0)
            fix_keys(fs, in, &p, p.depth);
    } else {
        st = remove_entry(fs, in, &p, p.depth);
    }
    ext_path_release(fs, &p);
    if (st != OK)
        return st;
    uint64_t from = eb > lblk ? eb : lblk, to = ee < *end ? ee : *end;
    if (eb < lblk && ee > *end) {   /* split: the part after the range is a new extent */
        struct ext_run right = { .pblk = ps + (*end - eb), .len = (uint32_t)(ee - *end),
                                 .kind = unw ? EXT_RUN_UNWRITTEN : EXT_RUN_DATA };
        st = ext_extent_insert(fs, in, (uint32_t)*end, &right);
    }
    if (st == OK)
        st = free_run(fs, in, ps + (from - eb), to - from);
    *end = from;
    *done = from <= lblk;
    return st;
}

status_t ext_extent_punch(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint64_t end,
                          uint32_t budget, bool *out_done)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!(in->d.flags & EXT_EXTENTS_FL) || end > (uint64_t)1 << 32 || !budget)
        return ERR_INVALID_ARGS;
    bool done = end <= lblk;
    for (; st == OK && !done && budget; budget--)
        st = punch_one(fs, in, lblk, &end, &done);
    if (st == OK)
        st = collapse(fs, in);
    status_t put = ext_iput(fs, in);
    if (st == OK)
        st = put;
    if (st == OK)
        *out_done = done;
    return st;
}

/* ---- unwritten to written -------------------------------------------------------------- */

status_t ext_extent_written(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint32_t len)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!(in->d.flags & EXT_EXTENTS_FL) || !len)
        return ERR_INVALID_ARGS;
    struct ext_path p;
    st = ext_extent_find(fs, in, lblk, &p);
    int32_t at = st == OK ? p.lv[p.depth].at : -1;
    struct ext_extent *e = at >= 0 ? &ext_extents(p.lv[p.depth].h)[at] : NULL;
    if (st == OK && (!e || !ext_extent_unwritten(e) || (uint64_t)lblk + len > extent_end(e)))
        st = ERR_INVALID_ARGS;
    uint64_t eb = e ? e->block : 0, ee = e ? extent_end(e) : 0, ps = e ? ext_extent_start(e) : 0;
    uint32_t before = (uint32_t)(lblk - eb), after = (uint32_t)(ee - lblk - len);
    if (st == OK && (before || after) &&
        ext_free_blocks(fs) < 2 * ((uint64_t)p.depth + 2))
        st = ERR_NO_SPACE;   /* up to two inserts, each might need a node per level */
    if (st == OK) {   /* e becomes its first piece: unwritten before, or written */
        set_extent(e, (uint32_t)eb, ps, before ? before : len, before != 0);
        touch(fs, in, &p, p.depth);
    }
    ext_path_release(fs, &p);
    if (st == OK && before) {
        struct ext_run mid = { .pblk = ps + before, .len = len, .kind = EXT_RUN_DATA };
        st = ext_extent_insert(fs, in, lblk, &mid);
    }
    if (st == OK && after) {
        struct ext_run last = { .pblk = ps + before + len, .len = after,
                                .kind = EXT_RUN_UNWRITTEN };
        st = ext_extent_insert(fs, in, lblk + len, &last);
    }
    status_t put = ext_iput(fs, in);
    return st != OK ? st : put;
}
