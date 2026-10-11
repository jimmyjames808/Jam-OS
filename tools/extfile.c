/* extcheck's write tests, part two: extent trees, file writes and
 * truncation (tools/extwrite.h has the model). Each test checks the
 * whole volume (extverify.c) as it goes, and those whose end state
 * Linux's e2fsck should judge save it (--save) with their files linked
 * under /emptydir.
 *
 * What each catches, failing without the behaviour:
 *   - depths: single blocks with holes between, written in order and in
 *     reverse, until the tree is each depth the volume has room for
 *     (jam4k: 2, linux1k: 3), read back, then cut a step at a time
 *     (no step removes more than EXT_TRUNCATE_STEP extents), the last
 *     block's bytes past a size zeroed, and freed;
 *   - deep: with nodes of 4 entries, random inserts to the format's
 *     limit of 5 levels (then ERR_NO_RESOURCES, nothing broken), and
 *     random punches back to an empty root;
 *   - merge: runs that continue each other become one extent, up to
 *     32768 written or 32767 unwritten blocks; written and unwritten
 *     never; an overlap refused;
 *   - punch: a range removed inside an extent (a split), across extents
 *     and leaves, a step of one extent at a time, the tree shortened;
 *   - unwritten: reads as zeros whatever its blocks hold; a write into it
 *     splits it and only the written part reads back;
 *   - sparse: a 6 GiB file of three blocks; cut to past 5 GiB and grown
 *     again, zeros where the cut was;
 *   - max: a byte at the last offset of a 16 TiB (4 KiB blocks) file, and
 *     nothing past it;
 *   - no space: a write larger than the volume stops short, keeps what it
 *     wrote, leaves the tree's reserve free, and the volume is whole;
 *   - forget: a freed extent node's dirty copy is not written over the
 *     data put in its block afterwards;
 *   - goal: a block far into a hole lies as far from the file's block
 *     before it (so a file's runs stay apart as its offsets are), or
 *     right after it when that is past the volume;
 *   - xattr: a freed inode lets go of its attribute block, which stays
 *     while another inode shares it;
 *   - i_blocks past 2^32 sectors (huge_file) and its conversion;
 *   - random: writes, truncates, punches and remounts on three files
 *     against a model of their bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extwrite.h"

static uint32_t room(const struct wfs *w)
{
    return (w->fs.block_size - 12) / 12;
}

static void must(struct wfs *w, status_t st, const char *what)
{
    if (st != OK)
        fail("%s: %s: %d (%s)", w->name, what, st, w->fs.why ? w->fs.why : "");
    check_pins(&w->fs);
}

/* The end of the test: the file truncated to 0 and freed, the volume as
 * it was. */
static void free_file(struct wfs *w, struct ext_inode *in)
{
    truncate_all(w, in, 0);
    if (tree_depth(in) || ext_iblocks512(&w->fs, in))
        fail("%s: inode %u: depth %u and %llu sectors when empty", w->name, in->ino,
             tree_depth(in), (unsigned long long)ext_iblocks512(&w->fs, in));
    must(w, ext_inode_free(&w->fs, in, 1760000002), "free");
    wsync(w);
}

/* ---- depths ---------------------------------------------------------------------------- */

/* Blocks at every other block of the file, n of them (block 2i holds
 * the pattern), to `size`. */
static void expect_every_other(struct wfs *w, struct ext_inode *in, uint32_t n, bool reverse,
                               uint64_t top, uint64_t size)
{
    uint32_t bs = w->fs.block_size;
    for (uint64_t i = 0; i * bs < size; i++) {
        uint64_t off = i * bs, len = size - off < bs ? size - off : bs;
        bool data = !(i & 1) && (reverse ? i <= top && (top - i) / 2 < n : i / 2 < n);
        if (data)
            expect_pat(w, in, off, (size_t)len);
        else if (i < 64 || i % 97 == 0 || i * bs + 2 * bs >= size)
            expect_zero(w, in, off, (size_t)len);
    }
}

/* The tree's node blocks: i_blocks less the n data blocks. */
static uint32_t nodes_of(struct wfs *w, struct ext_inode *in, uint32_t n)
{
    return (uint32_t)(ext_iblocks512(&w->fs, in) / (w->fs.block_size / 512)) - n;
}

/* The nodes n extents written in order may take: each node but the last
 * of a level holds all but one of its room (a split at the end moves only
 * the last entry), up to a level that fits the root. */
static uint32_t full_nodes(struct wfs *w, uint32_t n)
{
    uint32_t total = 0, per = room(w) - 1;
    while (n > 4) {
        n = (n + per - 1) / per;
        total += n;
    }
    return total;
}

static void check_depth(const struct img *m, uint32_t target, bool reverse)
{
    struct wfs w;
    wmount(&w, m);
    struct ext_inode in;
    new_file(&w, 100 + target, &in);
    uint32_t bs = w.fs.block_size, n = 0, extra = 0;
    uint64_t cap = 4;
    for (uint32_t i = 0; i < target; i++)
        cap *= room(&w);
    uint64_t top = 2 * (cap + 64);   /* reverse: the first block written */
    while (tree_depth(&in) < target || extra++ < (target ? 10u : 4u)) {
        write_pat(&w, &in, (reverse ? top - 2ull * n : 2ull * n) * bs, bs);
        n++;
        if (n % 64 == 0)
            wsync(&w);
        if (n % 8192 == 0)
            verify_volume(&w.fs);
    }
    wsync(&w);
    verify_volume(&w.fs);
    if (tree_depth(&in) != target || tree_extents(&w, &in) != n)
        fail("%s: depth %u with %u extents, not %u and %u", w.name, tree_depth(&in),
             tree_extents(&w, &in), target, n);
    if (!reverse && nodes_of(&w, &in, n) > full_nodes(&w, n))
        fail("%s: %u extents written in order take %u nodes, not at most %u", w.name, n,
             nodes_of(&w, &in, n), full_nodes(&w, n));
    uint64_t size = ext_isize(&in);
    expect_every_other(&w, &in, n, reverse, top, size);
    char what[32];
    snprintf(what, sizeof(what), "depth%u%s", target, reverse ? "r" : "");
    const char *name = what;
    save_linked(&w, what, &in.ino, &name, 1);
    /* One step removes at most EXT_TRUNCATE_STEP extents. */
    uint64_t cut = size * 3 / 4 + 123;
    bool done;
    must(&w, ext_truncate(&w.fs, &in, cut, &done), "a truncate step");
    uint32_t removed = n - tree_extents(&w, &in);
    if (removed > EXT_TRUNCATE_STEP || (!done && removed != EXT_TRUNCATE_STEP))
        fail("%s: one step removed %u extents (done %d)", w.name, removed, done);
    truncate_all(&w, &in, cut);
    verify_volume(&w.fs);
    expect_every_other(&w, &in, n, reverse, top, cut);
    truncate_all(&w, &in, cut + bs);   /* grown again: zeros past the cut */
    expect_zero(&w, &in, cut, bs);
    truncate_all(&w, &in, size / 3 / bs * bs);
    verify_volume(&w.fs);
    expect_every_other(&w, &in, n, reverse, top, ext_isize(&in));
    free_file(&w, &in);
    wremount(&w);
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* Nodes of 4 entries: random inserts to 5 levels, then random punches. */
static void check_deep(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    ext_extent_test_room = 4;
    struct ext_inode in;
    new_file(&w, 200, &in);
    uint32_t bs = w.fs.block_size, n = 0;
    status_t st = OK;
    while (st == OK) {
        uint64_t lblk = 2 * below(1u << 20);
        struct ext_run r;
        must(&w, ext_map(&w.fs, &in, (uint32_t)lblk, &r), "map");
        if (r.kind != EXT_RUN_HOLE)
            continue;
        uint8_t buf[65536];
        pattern(in.ino, lblk * bs, buf, bs);
        size_t got;
        st = ext_write(&w.fs, &in, lblk * bs, buf, bs, &got);
        n += st == OK;
        if (n % 32 == 0)
            wsync(&w);
    }
    if (st != ERR_NO_RESOURCES || tree_depth(&in) != EXT_EXTENT_MAX_DEPTH)
        fail("%s: a full tree of nodes of 4: %d at depth %u (%s, %llu)", w.name, st,
             tree_depth(&in), w.fs.why ? w.fs.why : "", (unsigned long long)w.fs.why_blk);
    wsync(&w);
    verify_volume(&w.fs);
    if (tree_extents(&w, &in) != n)
        fail("%s: %u extents, not %u", w.name, tree_extents(&w, &in), n);
    for (unsigned i = 0; tree_extents(&w, &in); i++) {
        uint64_t b = 1 + below((1u << 21) + (1u << 18)), len = 1 + below(1u << 18);
        uint64_t a = b > len ? b - len : 0;
        bool done = false;
        while (!done)
            must(&w, ext_extent_punch(&w.fs, &in, (uint32_t)a, b, 1 + (uint32_t)below(5), &done),
                 "punch");
        wsync(&w);
        if (i % 8 == 0)
            verify_volume(&w.fs);
        if (i > 4000)
            fail("%s: punching never emptied the tree", w.name);
    }
    if (tree_depth(&in))
        fail("%s: an empty tree %u deep", w.name, tree_depth(&in));
    verify_volume(&w.fs);
    free_file(&w, &in);
    ext_extent_test_room = 0;
    wcounts_back(&w);
    wdone(&w);
}

/* ---- merging --------------------------------------------------------------------------- */

static void insert(struct wfs *w, struct ext_inode *in, uint32_t lblk, uint64_t pblk, uint32_t len,
                   bool unw, status_t want)
{
    struct ext_run r = { .pblk = pblk, .len = len, .kind = unw ? EXT_RUN_UNWRITTEN : EXT_RUN_DATA };
    status_t st = ext_extent_insert(&w->fs, in, lblk, &r);
    if (st != want)
        fail("%s: insert %u+%u at %llu: %d, not %d", w->name, lblk, len,
             (unsigned long long)pblk, st, want);
}

static void expect_extents(struct wfs *w, struct ext_inode *in, uint32_t want, const char *what)
{
    if (tree_extents(w, in) != want)
        fail("%s: %s: %u extents, not %u", w->name, what, tree_extents(w, in), want);
}

/* The runs here aren't allocated: nothing is synced, all discarded. */
static void check_merge(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    w.fs.blocks = 1u << 20;   /* runs past the volume's end: never read, never synced */
    struct ext_inode a, b, c, d, e;
    new_file(&w, 300, &a);
    insert(&w, &a, 0, 10, 20000, false, OK);
    insert(&w, &a, 20000, 20010, 12768, false, OK);
    expect_extents(&w, &a, 1, "32768 written blocks");
    insert(&w, &a, 5, 3000, 2, false, ERR_ALREADY_EXISTS);
    insert(&w, &a, 40000, 100, 1, false, OK);
    insert(&w, &a, 39999, 99, 2, false, ERR_ALREADY_EXISTS);   /* runs into the next */
    insert(&w, &a, UINT32_MAX, 99, 2, false, ERR_OUT_OF_RANGE);
    new_file(&w, 301, &b);
    insert(&w, &b, 0, 10, 20000, false, OK);
    insert(&w, &b, 20000, 20010, 12769, false, OK);
    expect_extents(&w, &b, 2, "32769 written blocks");
    new_file(&w, 302, &c);
    insert(&w, &c, 0, 10, 20000, true, OK);
    insert(&w, &c, 20000, 20010, 12767, true, OK);
    expect_extents(&w, &c, 1, "32767 unwritten blocks");
    insert(&w, &c, 32767, 32777, 1, true, OK);
    expect_extents(&w, &c, 2, "32768 unwritten blocks");
    new_file(&w, 303, &d);
    insert(&w, &d, 0, 1000, 10, false, OK);
    insert(&w, &d, 20, 1020, 10, false, OK);
    insert(&w, &d, 10, 1010, 10, false, OK);   /* both sides */
    expect_extents(&w, &d, 1, "left and right");
    insert(&w, &d, 30, 1030, 10, true, OK);   /* written, then unwritten */
    expect_extents(&w, &d, 2, "written and unwritten");
    new_file(&w, 304, &e);   /* in front of a tree a level deep: the keys move */
    for (uint32_t i = 0; i < 8; i++)
        insert(&w, &e, 1000 + 10 * i, 5000 + 10 * i, 5, false, OK);
    insert(&w, &e, 995, 4995, 5, false, OK);
    expect_extents(&w, &e, 8, "merged in front");
    struct ext_run r;
    must(&w, ext_map(&w.fs, &e, 995, &r), "map");
    if (tree_depth(&e) != 1 || r.kind != EXT_RUN_DATA || r.pblk != 4995 || r.len != 10)
        fail("%s: the front merge maps %d %llu+%u", w.name, r.kind, (unsigned long long)r.pblk,
             r.len);
    ext_discard(&w.fs);
    wdone(&w);
}

/* ---- punching -------------------------------------------------------------------------- */

static void punch(struct wfs *w, struct ext_inode *in, uint64_t a, uint64_t b)
{
    for (bool done = false; !done;)
        must(w, ext_extent_punch(&w->fs, in, (uint32_t)a, b, 7, &done), "punch");
    wsync(w);
    verify_volume(&w->fs);
}

static void check_punch(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 400, &in);
    write_pat(&w, &in, 0, 100 * (size_t)bs);
    wsync(&w);
    uint64_t before = ext_free_blocks(&w.fs) + ext_iblocks512(&w.fs, &in) / (bs / 512);
    punch(&w, &in, 10, 20);
    punch(&w, &in, 50, 51);   /* inside an extent: split */
    punch(&w, &in, 0, 5);
    punch(&w, &in, 95, 100);
    if (ext_free_blocks(&w.fs) + ext_iblocks512(&w.fs, &in) / (bs / 512) != before ||
        ext_iblocks512(&w.fs, &in) / (bs / 512) > 100 - 21 + 1)
        fail("%s: punching 21 blocks: %llu free, the file holds %llu sectors", w.name,
             (unsigned long long)ext_free_blocks(&w.fs),
             (unsigned long long)ext_iblocks512(&w.fs, &in));
    for (uint32_t i = 0; i < 100; i++) {
        bool hole = (i >= 10 && i < 20) || i == 50 || i < 5 || i >= 95;
        if (hole)
            expect_zero(&w, &in, (uint64_t)i * bs, bs);
        else
            expect_pat(&w, &in, (uint64_t)i * bs, bs);
    }
    punch(&w, &in, 0, (uint64_t)1 << 32);
    if (tree_extents(&w, &in) || ext_iblocks512(&w.fs, &in))
        fail("%s: punched whole, yet blocks", w.name);
    /* Across leaves, then one extent a call. */
    for (uint32_t i = 0; i < 400; i++)
        write_pat(&w, &in, 2ull * i * bs, bs);
    wsync(&w);
    if (tree_depth(&in) < 1 + (bs == 1024))
        fail("%s: 400 extents at depth %u", w.name, tree_depth(&in));
    punch(&w, &in, 100, 600);
    uint32_t n = tree_extents(&w, &in);
    if (n != 150)
        fail("%s: 150 extents should be left, not %u", w.name, n);
    for (uint32_t i = 100; i < 600; i++)
        expect_zero(&w, &in, (uint64_t)i * bs, bs);
    expect_pat(&w, &in, 98ull * bs, bs);
    expect_pat(&w, &in, 600ull * bs, bs);
    while (n > 3) {
        bool done;
        must(&w, ext_extent_punch(&w.fs, &in, 0, (uint64_t)1 << 32, 1, &done), "punch one");
        if (done || tree_extents(&w, &in) != n - 1)
            fail("%s: a punch of budget 1 left %u extents of %u", w.name, tree_extents(&w, &in), n);
        n--;
    }
    wsync(&w);
    verify_volume(&w.fs);
    if (tree_depth(&in))
        fail("%s: 3 extents in a tree %u deep", w.name, tree_depth(&in));
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

/* ---- unwritten extents ----------------------------------------------------------------- */

static void check_unwritten(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 500, &in);
    uint64_t p;
    uint32_t got;
    uint64_t goal = ext_group_first(&w.fs, w.fs.groups - 1) + 100;
    must(&w, ext_balloc(&w.fs, goal, 10, &p, &got), "balloc");
    if (got != 10)
        fail("%s: 10 blocks in a row at %llu: %u", w.name, (unsigned long long)goal, got);
    memset(w.bytes + (p << w.fs.block_log), 0xA5, 10 * (size_t)bs);   /* what the disk held */
    struct ext_run r = { .pblk = p, .len = 10, .kind = EXT_RUN_UNWRITTEN };
    must(&w, ext_extent_insert(&w.fs, &in, 0, &r), "insert");
    must(&w, ext_iblocks_add(&w.fs, &in, 10), "i_blocks");
    in.d.size_lo = 10 * bs;
    must(&w, ext_iput(&w.fs, &in), "iput");
    wsync(&w);
    verify_volume(&w.fs);
    expect_zero(&w, &in, 0, 10 * (size_t)bs);
    write_pat(&w, &in, 4ull * bs, bs);
    expect_extents(&w, &in, 3, "one block written in the middle");
    write_pat(&w, &in, 0, 100);
    write_pat(&w, &in, 9ull * bs + 10, 20);
    wsync(&w);
    verify_volume(&w.fs);
    expect_pat(&w, &in, 0, 100);
    expect_zero(&w, &in, 100, bs - 100);
    expect_zero(&w, &in, bs, 3 * (size_t)bs);
    expect_pat(&w, &in, 4ull * bs, bs);
    expect_zero(&w, &in, 5ull * bs, 4 * (size_t)bs + 10);
    expect_pat(&w, &in, 9ull * bs + 10, 20);
    expect_zero(&w, &in, 9ull * bs + 30, bs - 30);
    if (ext_extent_written(&w.fs, &in, 4, 1) != ERR_INVALID_ARGS)
        fail("%s: a written extent made written", w.name);
    wremount(&w);
    must(&w, ext_iget(&w.fs, in.ino, 0, &in), "iget");
    expect_pat(&w, &in, 4ull * bs, bs);
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

/* ---- big and sparse -------------------------------------------------------------------- */

#define GIB (1ull << 30)

static void check_sparse(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 600, &in);
    write_pat(&w, &in, 0, 1000);
    write_pat(&w, &in, 5 * GIB, bs);
    write_pat(&w, &in, 6 * GIB - 777, 777);
    wsync(&w);
    verify_volume(&w.fs);
    if (ext_isize(&in) != 6 * GIB || ext_iblocks512(&w.fs, &in) != 3ull * (bs / 512))
        fail("%s: a 6 GiB sparse file: size %llu, %llu sectors", w.name,
             (unsigned long long)ext_isize(&in), (unsigned long long)ext_iblocks512(&w.fs, &in));
    struct ext_run r;
    must(&w, ext_map(&w.fs, &in, 1, &r), "map");
    if (r.kind != EXT_RUN_HOLE || r.len != 5 * GIB / bs - 1)
        fail("%s: the hole after block 0 is %u blocks", w.name, r.len);
    char before[65], after[65];
    sparse_hash(&w.fs, &in, before);
    wremount(&w);
    must(&w, ext_iget(&w.fs, in.ino, 0, &in), "iget");
    sparse_hash(&w.fs, &in, after);
    if (strcmp(before, after))
        fail("%s: the sparse file's bytes changed over a remount", w.name);
    expect_pat(&w, &in, 0, 1000);
    expect_zero(&w, &in, 1000, 3 * (size_t)bs);
    expect_zero(&w, &in, 5 * GIB - bs, bs);
    expect_pat(&w, &in, 5 * GIB, bs);
    expect_zero(&w, &in, 5 * GIB + bs, bs);
    expect_pat(&w, &in, 6 * GIB - 777, 777);
    const char *name = "sparse6g";
    save_linked(&w, "sparse6g", &in.ino, &name, 1);
    truncate_all(&w, &in, 5 * GIB + 100);
    truncate_all(&w, &in, 6 * GIB);
    expect_pat(&w, &in, 5 * GIB, 100);
    expect_zero(&w, &in, 5 * GIB + 100, bs - 100);   /* the cut's zeros */
    verify_volume(&w.fs);
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

static void check_max(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint64_t max = ext_max_size(&w.fs);
    if (w.fs.block_size == 4096 && max != (16ull << 40) - 4096)
        fail("%s: the largest file is %llu bytes", w.name, (unsigned long long)max);
    struct ext_inode in;
    new_file(&w, 700, &in);
    write_pat(&w, &in, max - 10, 10);
    if (ext_isize(&in) != max)
        fail("%s: size %llu, not the largest", w.name, (unsigned long long)ext_isize(&in));
    size_t got = 0;
    uint8_t buf[20];
    pattern(in.ino, max - 5, buf, 20);
    if (ext_write(&w.fs, &in, max, buf, 1, &got) != ERR_OUT_OF_RANGE)
        fail("%s: a write past the largest size", w.name);
    struct ext_inode small;
    new_file(&w, 701, &small);
    must(&w, ext_write(&w.fs, &small, 1000, buf, 0, &got), "a write of nothing");
    if (got || ext_isize(&small))
        fail("%s: a write of nothing made the size %llu", w.name,
             (unsigned long long)ext_isize(&small));
    must(&w, ext_inode_free(&w.fs, &small, 1), "free");
    must(&w, ext_write(&w.fs, &in, max - 5, buf, 20, &got), "write to the end");
    if (got != 5)
        fail("%s: a write across the largest size wrote %zu", w.name, got);
    bool done;
    if (ext_truncate(&w.fs, &in, max + 1, &done) != ERR_OUT_OF_RANGE)
        fail("%s: truncated past the largest size", w.name);
    wsync(&w);
    verify_volume(&w.fs);
    expect_pat(&w, &in, max - 10, 10);
    const char *name = "maxsize";
    save_linked(&w, "maxsize", &in.ino, &name, 1);
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

static void check_nospace(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 800, &in);
    size_t len = (size_t)(ext_free_blocks(&w.fs) + 10) * bs + 100, got = 0;
    uint8_t *buf = malloc(len);
    if (!buf)
        fail("out of memory");
    pattern(in.ino, 0, buf, len);
    must(&w, ext_write(&w.fs, &in, 0, buf, len, &got), "a write larger than the volume");
    free(buf);
    if (!got || got >= len || ext_isize(&in) != got || ext_free_blocks(&w.fs) > 8 ||
        ext_free_blocks(&w.fs) < 3 + tree_depth(&in))   /* kept for the tree's changes */
        fail("%s: a full volume's write: %zu of %zu, %llu blocks left", w.name, got, len,
             (unsigned long long)ext_free_blocks(&w.fs));
    uint64_t next = (got + bs - 1) / bs * bs;
    if (ext_write(&w.fs, &in, next, "x", 1, &got) != ERR_NO_SPACE)
        fail("%s: a write to a full volume", w.name);
    wsync(&w);
    verify_volume(&w.fs);
    expect_pat(&w, &in, 0, (size_t)ext_isize(&in));
    free_file(&w, &in);
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* A node freed with its block dirty, the block taken for data at once. */
static void check_forget(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 900, &in);
    for (uint32_t i = 0; i < 10; i++)
        write_pat(&w, &in, 2ull * i * bs, bs);
    wsync(&w);
    if (tree_depth(&in) != 1)
        fail("%s: 10 extents at depth %u", w.name, tree_depth(&in));
    uint64_t node = ext_idx_leaf(ext_idxs((struct ext_extent_header *)in.d.block));
    bool done;
    must(&w, ext_extent_punch(&w.fs, &in, 8, (uint64_t)1 << 32, 64, &done), "punch");
    if (!done || tree_depth(&in))
        fail("%s: 4 extents left in a tree %u deep", w.name, tree_depth(&in));
    uint64_t blk;
    uint32_t n;
    must(&w, ext_balloc(&w.fs, node, 1, &blk, &n), "balloc");
    if (blk != node)
        fail("%s: the freed node %llu, not %llu, came back", w.name, (unsigned long long)node,
             (unsigned long long)blk);
    uint8_t data[65536];
    pattern(12345, 0, data, bs);
    memcpy(w.bytes + (blk << w.fs.block_log), data, bs);   /* file data, around the cache */
    wsync(&w);
    if (memcmp(w.bytes + (blk << w.fs.block_log), data, bs))
        fail("%s: the freed node's old bytes were written over the data in its block", w.name);
    must(&w, ext_bfree(&w.fs, blk, 1), "bfree");
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

static void check_iblocks(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t per = w.fs.block_size / 512;
    struct ext_inode in = { .ino = 12 };
    in.d.blocks_lo = 0xFFFFFFF0u;
    must(&w, ext_iblocks_add(&w.fs, &in, 16), "past 2^32 sectors");
    if (ext_iblocks512(&w.fs, &in) != 0xFFFFFFF0ull + 16 * per || in.d.blocks_high != 1)
        fail("%s: i_blocks past 2^32: %llu", w.name,
             (unsigned long long)ext_iblocks512(&w.fs, &in));
    w.fs.huge_file = false;
    in.d.blocks_lo = 0xFFFFFFF0u;
    in.d.blocks_high = 0;
    if (ext_iblocks_add(&w.fs, &in, 16) != ERR_NO_SPACE || in.d.blocks_lo != 0xFFFFFFF0u)
        fail("%s: i_blocks past 2^32 without huge_file", w.name);
    w.fs.huge_file = true;
    in.d.blocks_lo = 5;
    in.d.flags = EXT_HUGE_FILE_FL;   /* counted in blocks */
    must(&w, ext_iblocks_add(&w.fs, &in, 1), "a huge file's count");
    if (in.d.flags & EXT_HUGE_FILE_FL || ext_iblocks512(&w.fs, &in) != 6ull * per)
        fail("%s: a huge file's count in sectors: %llu", w.name,
             (unsigned long long)ext_iblocks512(&w.fs, &in));
    if (ext_iblocks_add(&w.fs, &in, -7) != ERR_IO)
        fail("%s: i_blocks below 0", w.name);
    wdone(&w);
}

/* A block far into a hole goes as far from the file's block before it. */
static void check_goal(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t bs = w.fs.block_size;
    struct ext_inode in;
    new_file(&w, 950, &in);
    write_pat(&w, &in, 0, bs);
    write_pat(&w, &in, 300ull * bs, bs);
    struct ext_run a, b;
    must(&w, ext_map(&w.fs, &in, 0, &a), "map");
    must(&w, ext_map(&w.fs, &in, 300, &b), "map");
    if (b.pblk != a.pblk + 300)
        fail("%s: blocks 0 and 300 of a file at %llu and %llu", w.name,
             (unsigned long long)a.pblk, (unsigned long long)b.pblk);
    write_pat(&w, &in, 400ull * w.fs.blocks * bs, bs);   /* its goal is past the volume */
    must(&w, ext_map(&w.fs, &in, (uint32_t)(400ull * w.fs.blocks), &a), "map");
    if (a.pblk != b.pblk + 1)
        fail("%s: a block whose goal is past the volume went to %llu, not after %llu", w.name,
             (unsigned long long)a.pblk, (unsigned long long)b.pblk);
    free_file(&w, &in);
    wcounts_back(&w);
    wdone(&w);
}

/* Freeing an inode lets go of its attribute block: shared by two, the
 * block stays for the other; then freed with the last. */
static void check_xattr(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    struct ext_inode a, b;
    lookup(&w.fs, "pattern", &a);
    struct ext_buf *x;
    if (ext_xattr_block(&w.fs, &a, &x) != OK)
        fail("%s: /pattern has no attribute block", w.name);
    struct ext_xattr_header *h = (struct ext_xattr_header *)x->data;
    h->refcount = 2;
    h->checksum = ext_xattr_csum(&w.fs, x->blk, x->data);
    ext_bdirty(&w.fs, x);
    ext_brelse(&w.fs, x);
    new_file(&w, 960, &b);
    b.d.file_acl_lo = a.d.file_acl_lo;
    b.d.file_acl_high = a.d.file_acl_high;
    must(&w, ext_iblocks_add(&w.fs, &b, 1), "i_blocks");
    must(&w, ext_iput(&w.fs, &b), "iput");
    wsync(&w);
    verify_volume(&w.fs);
    truncate_all(&w, &a, 0);
    must(&w, ext_inode_free(&w.fs, &a, 1), "free the first");
    wsync(&w);
    verify_volume(&w.fs);
    must(&w, ext_inode_free(&w.fs, &b, 1), "free the second");
    wsync(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* ---- random operations against a model ------------------------------------------------- */

#define NFILES 3
#define MODEL_BLOCKS 96

struct model {
    struct ext_inode in;
    uint8_t *bytes;               /* MODEL_BLOCKS blocks */
    uint64_t size;
};

static void compare(struct wfs *w, struct model *f)
{
    if (ext_isize(&f->in) != f->size)
        fail("%s: inode %u: size %llu, the model's %llu", w->name, f->in.ino,
             (unsigned long long)ext_isize(&f->in), (unsigned long long)f->size);
    uint8_t *got = malloc(f->size + 1);
    size_t n;
    if (!got || ext_read(&w->fs, &f->in, 0, got, f->size, &n) != OK || n != f->size)
        fail("%s: inode %u: read", w->name, f->in.ino);
    for (uint64_t i = 0; i < f->size; i++)
        if (got[i] != f->bytes[i])
            fail("%s: inode %u: byte %llu is %#x, the model's %#x", w->name, f->in.ino,
                 (unsigned long long)i, got[i], f->bytes[i]);
    free(got);
}

static void random_op(struct wfs *w, struct model *f, uint32_t op)
{
    uint32_t bs = w->fs.block_size;
    uint64_t cap = (uint64_t)MODEL_BLOCKS * bs, r = below(100);
    if (r < 50) {   /* a write: a quarter of them whole blocks */
        uint64_t off = below(cap - 1), len = 1 + below(3 * bs);
        if (r < 12) {
            off -= off % bs;
            len = bs * (1 + below(3));
        }
        if (len > cap - off)
            len = cap - off;
        uint8_t *buf = f->bytes + off;
        pattern(op, off, buf, (size_t)len);
        size_t got;
        must(w, ext_write(&w->fs, &f->in, off, buf, (size_t)len, &got), "write");
        if (off + len > f->size)
            f->size = off + len;
    } else if (r < 70) {   /* a truncate */
        uint64_t size = below(cap + 1);
        if (size < f->size)
            memset(f->bytes + size, 0, f->size - size);
        truncate_all(w, &f->in, size);
        f->size = size;
    } else if (r < 85) {   /* a punch */
        uint64_t a = below(MODEL_BLOCKS), b = a + 1 + below(MODEL_BLOCKS - a);
        for (bool done = false; !done;)
            must(w, ext_extent_punch(&w->fs, &f->in, (uint32_t)a, b, 1 + (uint32_t)below(3),
                                     &done), "punch");
        memset(f->bytes + a * bs, 0, (b - a) * bs);
    } else {   /* a remount */
        wremount(w);
        must(w, ext_iget(&w->fs, f->in.ino, 0, &f->in), "iget");
    }
}

static void check_random(const struct img *m, unsigned nops)
{
    struct wfs w;
    wmount(&w, m);
    struct model f[NFILES];
    for (unsigned i = 0; i < NFILES; i++) {
        new_file(&w, 1000 + i, &f[i].in);
        f[i].bytes = calloc(MODEL_BLOCKS, w.fs.block_size);
        f[i].size = 0;
    }
    for (unsigned op = 1; op <= nops; op++) {
        struct model *t = &f[below(NFILES)];
        random_op(&w, t, op);
        for (unsigned i = 0; i < NFILES; i++)   /* a remount read every inode again */
            if (&f[i] != t && ext_iget(&w.fs, f[i].in.ino, 0, &f[i].in) != OK)
                fail("%s: iget", w.name);
        wsync(&w);
        compare(&w, t);
        if (op % 25 == 0) {
            verify_volume(&w.fs);
            for (unsigned i = 0; i < NFILES; i++)
                compare(&w, &f[i]);
        }
    }
    verify_volume(&w.fs);
    uint32_t inos[NFILES];
    const char *names[NFILES] = { "random0", "random1", "random2" };
    for (unsigned i = 0; i < NFILES; i++)
        inos[i] = f[i].in.ino;
    save_linked(&w, "random", inos, names, NFILES);
    for (unsigned i = 0; i < NFILES; i++) {
        free_file(&w, &f[i].in);
        free(f[i].bytes);
    }
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

void check_file_writes(const struct img *m)
{
    struct wfs probe;
    wmount(&probe, m);
    uint32_t bs = probe.fs.block_size;
    wdone(&probe);
    uint32_t deepest = bs == 1024 ? 3 : 2;   /* what the fixture has room for */
    for (uint32_t d = 0; d <= deepest; d++)
        check_depth(m, d, false);
    check_depth(m, deepest < 2 ? deepest : 2, true);
    check_deep(m);
    check_merge(m);
    check_punch(m);
    check_unwritten(m);
    check_sparse(m);
    check_max(m);
    check_nospace(m);
    check_forget(m);
    check_goal(m);
    if (bs == 1024)   /* linux1k: /pattern has an attribute block */
        check_xattr(m);
    check_iblocks(m);
    check_random(m, 400);
}
