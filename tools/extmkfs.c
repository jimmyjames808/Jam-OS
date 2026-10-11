/* extmkfs: mkfs.c's tests. The jam4k fixture's shape (4 KiB blocks, 1024
 * per group, flex groups of 4, 512 inodes, a 1024-block journal) made
 * again and compared with what Linux's mke2fs made: the superblock's
 * shape and features, where each group's bitmaps and table lie, the
 * journal's superblock, the root's, the journal's and lost+found's
 * inodes. Then shapes of every block size, small and odd-sized volumes,
 * small groups and flex groups: each made, checked (extinv.c), written
 * to and checked again after a remount. And mkfs's refusals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extvol.h"

/* Linux's jam4k and mkfs's image of the same shape agree where they must. */
static void mkfs_parity(const struct img *jam)
{
    where = "mkfs: jam4k's shape";
    const struct ext_super *a = (const struct ext_super *)(jam->bytes + EXT_SUPER_OFFSET);
    struct ext_mkfs p = { .block_size = 4096, .blocks_per_group = 1024, .inodes = 512,
                          .flex = 4, .journal_blocks = 1024, .now = 1760000000 };
    memcpy(p.uuid, a->uuid, 16);
    struct vol v;
    vol_new(&v, jam->size);
    expect(vol_mkfs(&v, &p), OK, "mkfs");
    const struct ext_super *b = (const struct ext_super *)(v.bytes + EXT_SUPER_OFFSET);
#define SAME(f) do {                                                    \
        if (memcmp(&a->f, &b->f, sizeof(a->f)))                         \
            fail("superblock's " #f " differs");                        \
    } while (0)
    SAME(inodes_count); SAME(blocks_count_lo); SAME(blocks_count_hi); SAME(first_data_block);
    SAME(log_block_size); SAME(log_cluster_size); SAME(blocks_per_group);
    SAME(clusters_per_group); SAME(inodes_per_group); SAME(magic); SAME(rev_level);
    SAME(first_ino); SAME(inode_size); SAME(feature_compat); SAME(feature_incompat);
    SAME(feature_ro_compat); SAME(uuid); SAME(journal_inum); SAME(jnl_backup_type);
    SAME(jnl_blocks); SAME(desc_size); SAME(min_extra_isize); SAME(want_extra_isize);
    SAME(log_groups_per_flex); SAME(checksum_type); SAME(max_mnt_count); SAME(mkfs_time);
    SAME(def_hash_version); SAME(flags); SAME(reserved_gdt_blocks); SAME(first_meta_bg);
#undef SAME
    for (uint32_t g = 0; g < 4; g++) {
        const uint8_t *da = jam->bytes + 4096 + g * 64, *db = v.bytes + 4096 + g * 64;
        if (memcmp(da, db, 12))
            fail("group %u's bitmaps and table are elsewhere", g);
    }
    if (memcmp(jam->bytes + 2048ull * 4096, v.bytes + 2048ull * 4096, 1024))
        fail("the journal's superblock differs");
    static const uint32_t inos[] = { EXT_ROOT_INO, EXT_JOURNAL_INO, 11 };
    for (unsigned i = 0; i < 3; i++) {
        uint64_t off = 10ull * 4096 + (inos[i] - 1) * 256;
        const struct ext_disk_inode *ia = (const void *)(jam->bytes + off);
        const struct ext_disk_inode *ib = (const void *)(v.bytes + off);
        if (ia->mode != ib->mode || memcmp(ia->block, ib->block, sizeof(ia->block)) ||
            (inos[i] != EXT_ROOT_INO && (ia->size_lo != ib->size_lo ||
                                         ia->blocks_lo != ib->blocks_lo)))
            fail("inode %u differs", inos[i]);
    }
    vol_mount(&v);
    struct inv inv;
    vol_check(&v, &inv);
    if (inv.dirs != 2 || inv.names != 1 || !inv.exact)
        fail("a new volume: %u directories, %u names", inv.dirs, inv.names);
    vol_free(&v);
}

struct shape {
    uint32_t block_size, per_group, flex, flags;
    uint64_t size;
};

/* Each shape made, mounted writable, checked, written to, checked again. */
static void mkfs_shapes(void)
{
    static const struct shape shapes[] = {
        { 1024, 0, 0, 0, 8u << 20 },
        { 1024, 0, 0, 0, (40u << 20) + 3072 },
        { 2048, 0, 0, EXT_MKFS_ZEROED, (24u << 20) + 2048 },
        { 4096, 0, 0, 0, 64u << 20 },
        { 4096, 4096, 0, 0, 100u << 20 },
        { 4096, 256, 0, 0, 32u << 20 },
        { 4096, 1024, 2, 0, (48u << 20) + 4096 * 7 },
    };
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const struct shape *s = &shapes[i];
        char what[96];
        snprintf(what, sizeof(what), "mkfs: %u-byte blocks, %llu bytes, %u per group",
                 s->block_size, (unsigned long long)s->size, s->per_group);
        where = what;
        struct ext_mkfs p = { .block_size = s->block_size, .blocks_per_group = s->per_group,
                              .flex = s->flex, .flags = s->flags, .uid = 1000, .gid = 1000,
                              .now = clock_now };
        p.uuid[0] = (uint8_t)i;
        struct vol v;
        vol_new(&v, s->size);
        expect(vol_mkfs(&v, &p), OK, "mkfs");
        vol_mount(&v);
        struct inv inv;
        vol_check(&v, &inv);
        if (inv.dirs != 2 || !inv.exact)
            fail("a new volume: %u directories", inv.dirs);
        expect(make(&v, "d", EXT_S_IFDIR | 0755, NULL), OK, "mkdir");
        expect(make(&v, "d/f", EXT_S_IFREG | 0644, NULL), OK, "create");
        vol_remount(&v, &inv);
        if (inv.dirs != 3 || inv.names != 3)
            fail("after writing: %u directories, %u names", inv.dirs, inv.names);
        vol_free(&v);
    }
}

static void mkfs_refusals(void)
{
    where = "mkfs: refusals";
    struct vol v;
    vol_new(&v, 4u << 20);
    struct ext_mkfs p = { .block_size = 4096, .now = clock_now };
    expect(vol_mkfs(&v, &p), ERR_NO_SPACE, "1024 blocks");
    p.block_size = 8192;
    expect(vol_mkfs(&v, &p), ERR_INVALID_ARGS, "8 KiB blocks");
    p.block_size = 1024;
    p.flex = 3;
    expect(vol_mkfs(&v, &p), ERR_INVALID_ARGS, "flex 3");
    p.flex = 0;
    p.blocks_per_group = 100;
    expect(vol_mkfs(&v, &p), ERR_INVALID_ARGS, "100 blocks per group");
    p.blocks_per_group = 0;
    p.journal_blocks = 512;
    expect(vol_mkfs(&v, &p), ERR_INVALID_ARGS, "a 512-block journal");
    p.journal_blocks = 0;
    struct ext_dev dev = vol_dev(&v);
    uint8_t one[1024];
    expect(ext_mkfs(&dev, &p, one, sizeof(one)), ERR_NO_MEMORY, "one block of memory");
    expect(vol_mkfs(&v, &p), OK, "4 MiB of 1 KiB blocks");
    vol_free(&v);
}

void check_mkfs(const struct img *jam4k)
{
    mkfs_parity(jam4k);
    mkfs_shapes();
    mkfs_refusals();
}
