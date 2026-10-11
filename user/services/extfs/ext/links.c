/* ext: making and removing names: new files, directories and symbolic
 * links, hard links, unlinks; and the link counts they keep.
 *
 * Each call checks everything it can before it changes anything (the
 * name, the types, the name taken or missing, the link count's room, a
 * directory's emptiness), so its common refusals leave the transaction
 * clean. Then it changes, in order: the new inode (written whole: its
 * table slot zeroed first, so no old attribute bytes survive), the
 * directory entry, the counts, the directory's times.
 *
 * A new inode: the generation one past the slot's last (so blocks left
 * from the inode's previous life never pass its checksums), the large
 * part's 32 bytes (nanosecond times and the creation time), an empty
 * extent tree for a file or directory, the four times the caller gave.
 * A directory's first block holds "." and ".."; it counts 2 links, and
 * its parent one more. A symbolic link under 60 bytes lives in i_block
 * (no extent tree: the bytes are the target), a longer one in a block. */
#include "ext_alloc.h"
#include "names_int.h"

#define FAST_LINK_MAX ((uint32_t)sizeof(((struct ext_disk_inode *)0)->block))
#define TARGET_MAX    4095u

static bool many(const struct ext_inode *in)
{
    return ext_itype(in) == EXT_FT_DIR && in->d.links_count == 1;
}

status_t ext_nlink_room(const struct ext_inode *in)
{
    return many(in) || in->d.links_count < EXT_LINK_MAX ? OK : ERR_NO_RESOURCES;
}

void ext_nlink_inc(struct ext_inode *in)
{
    if (!many(in))
        in->d.links_count++;
}

void ext_nlink_dec(struct ext_inode *in)
{
    if (in->d.links_count > 2)
        in->d.links_count--;
}

status_t ext_touch(struct ext_inode *in, const struct ext_time *now, bool mtime)
{
    status_t st = ext_itime_set(in, EXT_CTIME, now);
    if (st == OK && mtime)
        st = ext_itime_set(in, EXT_MTIME, now);
    return st;
}

/* Can dir take a new name? (A directory removed while open can't.) */
static status_t dir_ok(const struct ext_inode *dir)
{
    if (ext_itype(dir) != EXT_FT_DIR)
        return ERR_NOT_DIR;
    return dir->d.links_count ? OK : ERR_NOT_FOUND;
}

/* Is `name` free in dir? */
static status_t name_free(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name)
{
    struct ext_dentry d;
    status_t st = ext_name_check(name);
    if (st == OK)
        st = dir_ok(dir);
    if (st != OK)
        return st;
    st = ext_lookup(fs, dir, name, &d);
    if (st == OK)
        return ERR_ALREADY_EXISTS;
    return st == ERR_NOT_FOUND ? OK : st;
}

/* Write inode ino whole for the first time: *out ready to fill in. */
static status_t inode_new(struct ext_fs *fs, uint32_t ino, uint16_t mode,
                          const struct ext_attrs *a, struct ext_inode *out)
{
    uint64_t blk;
    uint32_t off;
    struct ext_buf *b;
    status_t st = ext_iloc(fs, ino, &blk, &off);
    if (st == OK)
        st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    uint32_t gen = ((const struct ext_disk_inode *)(b->data + off))->generation + 1;
    __builtin_memset(b->data + off, 0, fs->inode_size);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    __builtin_memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->seed = ext_inode_seed(fs, ino, gen);
    out->d.mode = mode;
    out->d.uid = (uint16_t)a->uid;
    out->d.uid_high = (uint16_t)(a->uid >> 16);
    out->d.gid = (uint16_t)a->gid;
    out->d.gid_high = (uint16_t)(a->gid >> 16);
    out->d.links_count = 1;
    out->d.generation = gen;
    out->d.extra_isize = fs->inode_size > EXT_GOOD_OLD_INODE ? 32 : 0;
    static const enum ext_time_which all[] = { EXT_ATIME, EXT_CTIME, EXT_MTIME, EXT_CRTIME };
    for (uint32_t i = 0; i < 4 && st == OK; i++)
        if (all[i] != EXT_CRTIME || out->d.extra_isize)
            st = ext_itime_set(out, all[i], &a->now);
    return st;
}

/* An empty extent tree in the inode. */
static void extent_root(struct ext_inode *in)
{
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    in->d.flags |= EXT_EXTENTS_FL;
    *h = (struct ext_extent_header){ .magic = EXT_EXTENT_MAGIC, .max = 4 };
}

/* A new directory's first block: "." and "..". */
static status_t dir_first_block(struct ext_fs *fs, struct ext_inode *in, uint32_t parent)
{
    uint64_t pblk;
    struct ext_buf *b;
    status_t st = ext_alloc_lblk(fs, in, 0, &pblk);
    if (st == OK)
        st = ext_bnew(fs, pblk, &b);
    if (st != OK)
        return st;
    struct ext_dirent *dot = (struct ext_dirent *)b->data;
    struct ext_dirent *dotdot = (struct ext_dirent *)(b->data + 12);
    *dot = (struct ext_dirent){ .inode = in->ino, .rec_len = 12, .name_len = 1,
                                .file_type = fs->filetype ? EXT_FT_DIR : 0 };
    dot->name[0] = '.';
    *dotdot = (struct ext_dirent){ .inode = parent, .name_len = 2,
                                   .rec_len = ext_rec_store(fs, ext_leaf_limit(fs) - 12),
                                   .file_type = fs->filetype ? EXT_FT_DIR : 0 };
    dotdot->name[0] = dotdot->name[1] = '.';
    ext_leaf_seal(fs, in, b);
    ext_brelse(fs, b);
    in->d.size_lo = fs->block_size;
    in->d.links_count = 2;
    return OK;
}

/* Name the finished inode `in` in dir and stamp dir. */
static status_t name_it(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                        struct ext_inode *in, const struct ext_time *now)
{
    status_t st = ext_iput(fs, in);
    if (st == OK)
        st = ext_dirent_add(fs, dir, name, in->ino, (uint8_t)ext_itype(in));
    if (st == OK && ext_itype(in) == EXT_FT_DIR)
        ext_nlink_inc(dir);
    if (st == OK)
        st = ext_touch(dir, now, true);
    if (st == OK)
        st = ext_iput(fs, dir);
    return st;
}

status_t ext_create(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    const struct ext_attrs *a, struct ext_inode *out)
{
    uint16_t type = a->mode & EXT_S_IFMT;
    bool isdir = type == EXT_S_IFDIR;
    if (type != EXT_S_IFREG && !isdir)
        return ERR_NOT_SUPPORTED;
    status_t st = name_free(fs, dir, name);
    if (st == OK && isdir)
        st = ext_nlink_room(dir);
    uint32_t ino;
    if (st == OK)
        st = ext_ialloc(fs, dir->ino, isdir, &ino);
    struct ext_inode in;
    if (st == OK)
        st = inode_new(fs, ino, a->mode, a, &in);
    if (st != OK)
        return st;
    extent_root(&in);
    if (isdir)
        st = dir_first_block(fs, &in, dir->ino);
    if (st == OK)
        st = name_it(fs, dir, name, &in, &a->now);
    if (st == OK)
        *out = in;
    return st;
}

/* Is target a symbolic link's target this volume can hold? */
static status_t target_check(const struct ext_fs *fs, const struct ext_name *t)
{
    if (!t->len)
        return ERR_INVALID_ARGS;
    if (t->len > TARGET_MAX || t->len >= fs->block_size)
        return ERR_NAME_TOO_LONG;
    for (uint32_t i = 0; i < t->len; i++)
        if (t->s[i] == '\0')
            return ERR_INVALID_ARGS;
    return OK;
}

/* A slow link's target into its block. */
static status_t target_block(struct ext_fs *fs, struct ext_inode *in, const struct ext_name *t)
{
    uint64_t pblk;
    struct ext_buf *b;
    extent_root(in);
    status_t st = ext_alloc_lblk(fs, in, 0, &pblk);
    if (st == OK)
        st = ext_bnew(fs, pblk, &b);
    if (st != OK)
        return st;
    __builtin_memcpy(b->data, t->s, t->len);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    return OK;
}

status_t ext_symlink(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                     const struct ext_name *target, const struct ext_attrs *a,
                     struct ext_inode *out)
{
    status_t st = target_check(fs, target);
    if (st == OK)
        st = name_free(fs, dir, name);
    uint32_t ino;
    if (st == OK)
        st = ext_ialloc(fs, dir->ino, false, &ino);
    struct ext_inode in;
    if (st == OK)
        st = inode_new(fs, ino, EXT_S_IFLNK | 0777, a, &in);
    if (st != OK)
        return st;
    if (target->len < FAST_LINK_MAX)
        __builtin_memcpy(in.d.block, target->s, target->len);
    else
        st = target_block(fs, &in, target);
    in.d.size_lo = target->len;
    if (st == OK)
        st = name_it(fs, dir, name, &in, &a->now);
    if (st == OK)
        *out = in;
    return st;
}

status_t ext_link(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                  struct ext_inode *in, const struct ext_time *now)
{
    struct ext_inode cur;
    status_t st = ext_iget(fs, in->ino, EXT_IGET_UNLINKED, &cur);
    if (st == OK && (cur.d.generation != in->d.generation || !cur.d.links_count))
        st = ERR_NOT_FOUND;
    if (st == OK && ext_itype(&cur) == EXT_FT_DIR)
        st = ERR_IS_DIR;
    if (st == OK)
        st = ext_nlink_room(&cur);
    if (st == OK)
        st = name_free(fs, dir, name);
    if (st == OK)
        st = ext_dirent_add(fs, dir, name, cur.ino, (uint8_t)ext_itype(&cur));
    if (st != OK)
        return st;
    ext_nlink_inc(&cur);
    st = ext_touch(&cur, now, false);
    if (st == OK)
        st = ext_iput(fs, &cur);
    if (st == OK)
        st = ext_touch(dir, now, true);
    if (st == OK)
        st = ext_iput(fs, dir);
    if (st == OK)
        *in = cur;
    return st;
}

/* The entry `name` in dir and its inode, checked against flags. */
static status_t unlink_target(struct ext_fs *fs, struct ext_inode *dir,
                              const struct ext_name *name, uint32_t flags, struct ext_dentry *e,
                              struct ext_inode *in)
{
    status_t st = ext_name_check(name);
    if (st == OK)
        st = ext_itype(dir) == EXT_FT_DIR ? OK : ERR_NOT_DIR;
    if (st == OK)
        st = ext_lookup(fs, dir, name, e);
    if (st == OK)
        st = ext_iget(fs, e->ino, 0, in);
    if (st == ERR_NOT_FOUND && e->ino)
        return ext_damaged(fs, "directory entry: a free inode", dir->ino, 0);
    if (st != OK)
        return st;
    bool isdir = ext_itype(in) == EXT_FT_DIR, empty = false;
    if (!(flags & EXT_UNLINK_DIR))
        return isdir ? ERR_IS_DIR : OK;
    if (!isdir)
        return ERR_NOT_DIR;
    st = ext_dir_empty(fs, in, &empty);
    if (st == OK && !empty)
        st = ERR_NOT_EMPTY;
    return st;
}

status_t ext_unlinked(struct ext_fs *fs, struct ext_inode *in, const struct ext_time *now,
                      uint32_t *out_orphan)
{
    if (ext_itype(in) == EXT_FT_DIR) {
        in->d.links_count = 0;
        ext_names_forget(fs, in->ino);
    } else if (in->d.links_count) {
        in->d.links_count--;
    }
    status_t st = ext_touch(in, now, false);
    if (st != OK)
        return st;
    *out_orphan = in->d.links_count ? 0 : in->ino;
    return in->d.links_count ? ext_iput(fs, in) : ext_orphan_add(fs, in);
}

status_t ext_unlink(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    uint32_t flags, const struct ext_time *now, uint32_t *out_orphan)
{
    if (flags & ~(uint32_t)EXT_UNLINK_DIR)
        return ERR_INVALID_ARGS;
    struct ext_dentry e = { .ino = 0 };
    struct ext_inode in;
    status_t st = unlink_target(fs, dir, name, flags, &e, &in);
    if (st == OK)
        st = ext_dirent_remove(fs, dir, e.pos, e.ino);
    if (st == OK)
        st = ext_unlinked(fs, &in, now, out_orphan);
    if (st != OK)
        return st;
    if (ext_itype(&in) == EXT_FT_DIR)
        ext_nlink_dec(dir);
    st = ext_touch(dir, now, true);
    if (st == OK)
        st = ext_iput(fs, dir);
    return st;
}
