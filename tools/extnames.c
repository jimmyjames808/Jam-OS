/* extnames: extcheck's tests of the names' writes (user/services/extfs/ext:
 * dirent.c, names.c, links.c, rename.c, orphan.c), run on volumes held in
 * memory (tools/extvol.c): ones mkfs makes, and a copy of a fixture Linux
 * made. After each step the volume's invariants are checked
 * (tools/extinv.c), and often again after a remount (a fresh struct
 * ext_fs over the same bytes, as a successor would mount it). Each
 * refusal is checked to have changed nothing.
 *
 *   - renames of every kind: in one directory and across, replacing (the
 *     replaced inode an orphan), NOREPLACE, EXCHANGE of files, of a file
 *     and a directory, of two directories; directories moved (their ".."
 *     and both parents' link counts), over an empty directory, refused
 *     over a full one, a file, below themselves; two names of one inode;
 *   - hard links and link counts (to EXT_LINK_MAX), unlinks to an orphan;
 *   - symbolic links in the inode (59 bytes) and in a block (60 bytes to
 *     the longest a block holds), with 4 KiB and 1 KiB blocks;
 *   - directories made and removed, their link counts, the refusals;
 *   - bad names and good ones (255 bytes, Makefile beside makefile);
 *   - orphans across a remount: kept ones stay, the rest are freed, a
 *     truncation's orphan finished;
 *   - an indexed directory (linux1k's) changed: made plain, every name
 *     still there;
 *   - the hash after a given-up transaction, and the add's room hint
 *     after a removal (each a test that fails without its code);
 *   - and through check_names, mkfs (extmkfs.c), 10,000 entries in one
 *     directory and the storm (extbig.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extvol.h"

/* ---- renames ----------------------------------------------------------------------------- */

/* A plain move, a replace, the flags, exchanges. */
static void renames_files(struct vol *v)
{
    uint32_t f1 = ino_of(v, "a/f1"), f2 = ino_of(v, "d/f2"), o;
    expect(mv(v, "a/f1", "a/g1", 0, NULL), OK, "same directory");
    if (ino_of(v, "a/g1") != f1)
        fail("a rename changed the inode");
    gone(v, "a/f1");
    expect(mv(v, "a/g1", "d/g1", 0, NULL), OK, "across directories");
    expect(mv(v, "d/g1", "d/f2", 0, &o), OK, "replace");
    if (o != f2 || ino_of(v, "d/f2") != f1)
        fail("replace: orphan %u, d/f2 is %u", o, ino_of(v, "d/f2"));
    struct ext_time t = now();
    expect(ext_orphan_drop(&v->fs, o, &t), OK, "drop the replaced");
    vol_check(v, NULL);
    vol_sync(v);
    refused(v, mv(v, "d/f2", "d/f3", EXT_RENAME_NOREPLACE, NULL), ERR_ALREADY_EXISTS, "noreplace");
    refused(v, mv(v, "d/f2", "d/zz", EXT_RENAME_EXCHANGE, NULL), ERR_NOT_FOUND, "exchange, none");
    refused(v, mv(v, "d/nope", "d/zz", 0, NULL), ERR_NOT_FOUND, "from nothing");
    refused(v, mv(v, "d/f2", "d/zz", 4, NULL), ERR_INVALID_ARGS, "unknown flag");
    refused(v, mv(v, "d/f2", "d/zz", 3, NULL), ERR_INVALID_ARGS, "both flags");
    refused(v, mv(v, "d/f2", "d/.", 0, NULL), ERR_INVALID_ARGS, "to .");
    expect(mv(v, "d/f2", "d/f4", EXT_RENAME_NOREPLACE, NULL), OK, "noreplace, free");
    uint32_t f3 = ino_of(v, "d/f3");
    expect(mv(v, "d/f4", "d/f3", EXT_RENAME_EXCHANGE, NULL), OK, "exchange files");
    if (ino_of(v, "d/f3") != f1 || ino_of(v, "d/f4") != f3)
        fail("exchange: the inodes didn't swap");
    struct ext_inode dir, in;
    struct ext_name n = split(v, "d/f3", &dir);
    iget(v, f1, &in);
    struct ext_name h = nm("h");
    expect(ext_link(&v->fs, &dir, &h, &in, &t), OK, "link");
    (void)n;
    vol_sync(v);
    expect(mv(v, "d/f3", "d/h", 0, NULL), OK, "onto another name of itself");
    if (ino_of(v, "d/f3") != f1 || ino_of(v, "d/h") != f1)
        fail("a rename between two names of one inode changed something");
    vol_check(v, NULL);
}

/* Directories: moved, replacing, refused, never below themselves. */
static void renames_dirs(struct vol *v)
{
    uint32_t root = EXT_ROOT_INO, a = ino_of(v, "a"), d = ino_of(v, "d"), e = ino_of(v, "e");
    uint32_t sd = ino_of(v, "a/sd");
    expect(mv(v, "d/f4", "a/sd", EXT_RENAME_EXCHANGE, NULL), OK, "exchange a file and a directory");
    dir_is(v, "d/f4", 3, d);
    dir_is(v, "a", 2, root);
    dir_is(v, "d", 3, root);
    expect(mv(v, "d/f4", "d/sub", 0, NULL), OK, "directory, same parent");
    expect(mv(v, "d/sub", "e/sub", 0, NULL), OK, "directory, new parent");
    dir_is(v, "e/sub", 3, e);
    dir_is(v, "e/sub/inner", 2, sd);
    dir_is(v, "d", 2, root);
    dir_is(v, "e", 3, root);
    if (ino_of(v, "e/sub") != sd)
        fail("a moved directory changed its inode");
    vol_check(v, NULL);
    expect(make(v, "a/empty", EXT_S_IFDIR | 0755, NULL), OK, "mkdir");
    uint32_t o, empty = ino_of(v, "a/empty");
    expect(mv(v, "e/sub", "a/empty", 0, &o), OK, "a directory over an empty one");
    if (o != empty)
        fail("the replaced directory isn't the orphan");
    struct ext_time t = now();
    expect(ext_orphan_drop(&v->fs, o, &t), OK, "drop");
    dir_is(v, "a/empty", 3, a);
    dir_is(v, "a", 3, root);
    dir_is(v, "e", 2, root);
    vol_check(v, NULL);
    vol_sync(v);
    refused(v, mv(v, "e", "a", 0, NULL), ERR_NOT_EMPTY, "over a full directory");
    refused(v, mv(v, "e", "a/b", 0, NULL), ERR_NOT_DIR, "a directory over a file");
    refused(v, mv(v, "a/b", "e", 0, NULL), ERR_IS_DIR, "a file over a directory");
    refused(v, mv(v, "a", "a/empty/x", 0, NULL), ERR_INVALID_ARGS, "below itself");
    refused(v, mv(v, "a", "a/x", 0, NULL), ERR_INVALID_ARGS, "into itself");
    refused(v, mv(v, "a/empty", "a/empty/inner/x", 0, NULL), ERR_INVALID_ARGS, "deeper");
    refused(v, mv(v, "a/empty/inner", "a", EXT_RENAME_EXCHANGE, NULL), ERR_INVALID_ARGS,
            "exchange with an ancestor");
    expect(mv(v, "a", "a", 0, NULL), OK, "onto itself");
    expect(mv(v, "a/empty", "x", 0, NULL), OK, "up to the root");
    dir_is(v, "x", 3, root);
    expect(mv(v, "x", "e", EXT_RENAME_EXCHANGE, NULL), OK, "exchange two directories");
    dir_is(v, "e", 3, root);
    dir_is(v, "x", 2, root);
    expect(mv(v, "a", "e/inner/a", 0, NULL), OK, "down two levels");
    dir_is(v, "e/inner/a", 2, ino_of(v, "e/inner"));
    dir_is(v, "e/inner", 3, sd);
    vol_check(v, NULL);
}

static void renames(void)
{
    where = "renames";
    struct vol v;
    small_vol(&v, 4096, 16u << 20);
    static const char *dirs[] = { "a", "a/b", "d", "e", "a/sd", "a/sd/inner" };
    static const char *files[] = { "a/f1", "d/f2", "d/f3" };
    for (unsigned i = 0; i < 6; i++)
        expect(make(&v, dirs[i], EXT_S_IFDIR | 0755, NULL), OK, dirs[i]);
    expect(rm(&v, "a/b", EXT_UNLINK_DIR, NULL), OK, "rmdir a/b");
    expect(make(&v, "a/b", EXT_S_IFREG | 0644, NULL), OK, "a/b");
    for (unsigned i = 0; i < 3; i++)
        expect(make(&v, files[i], EXT_S_IFREG | 0644, NULL), OK, files[i]);
    renames_files(&v);
    renames_dirs(&v);
    vol_remount(&v, NULL);
    vol_save(&v, "renames", true);
    vol_free(&v);
}

/* ---- links, symbolic links, directories, names ------------------------------------------- */

static void links(void)
{
    where = "hard links";
    struct vol v;
    small_vol(&v, 4096, 16u << 20);
    expect(make(&v, "f", EXT_S_IFREG | 0644, NULL), OK, "f");
    expect(make(&v, "dir", EXT_S_IFDIR | 0755, NULL), OK, "dir");
    expect(link_to(&v, "f", "dir/g"), OK, "link");
    struct ext_inode in;
    uint32_t f = walk(&v, "f", &in);
    if (in.d.links_count != 2 || ino_of(&v, "dir/g") != f)
        fail("a link: %u links", in.d.links_count);
    vol_check(&v, NULL);
    vol_sync(&v);
    refused(&v, link_to(&v, "dir", "dir2"), ERR_IS_DIR, "a link to a directory");
    refused(&v, link_to(&v, "f", "dir/g"), ERR_ALREADY_EXISTS, "a link onto a name");
    set_links(&v, f, EXT_LINK_MAX - 1);
    expect(link_to(&v, "f", "h"), OK, "the last link");
    vol_sync(&v);
    refused(&v, link_to(&v, "f", "i"), ERR_NO_RESOURCES, "one too many");
    set_links(&v, f, 3);
    uint32_t o;
    expect(rm(&v, "h", 0, &o), OK, "unlink");
    expect(rm(&v, "dir/g", 0, &o), OK, "unlink");
    walk(&v, "f", &in);
    if (o || in.d.links_count != 1)
        fail("unlinking a second name: orphan %u, %u links", o, in.d.links_count);
    expect(rm(&v, "f", 0, &o), OK, "unlink the last name");
    if (o != f || v.fs.sb.last_orphan != f)
        fail("the last name: orphan %u, the list starts at %u", o, v.fs.sb.last_orphan);
    struct inv inv;
    vol_check(&v, &inv);
    if (inv.orphans != 1)
        fail("%u orphans", inv.orphans);
    struct ext_inode dir;
    iget(&v, f, &in);
    walk(&v, "dir", &dir);
    struct ext_name n = nm("again");
    struct ext_time t = now();
    expect(ext_link(&v.fs, &dir, &n, &in, &t), ERR_NOT_FOUND, "a link to an orphan");
    struct ext_inode stale = in;
    stale.d.generation++;
    expect(ext_link(&v.fs, &dir, &n, &stale, &t), ERR_NOT_FOUND, "a link to a stale inode");
    expect(ext_orphan_drop(&v.fs, f, &t), OK, "drop");
    vol_remount(&v, &inv);
    if (inv.orphans || inv.names != 2)
        fail("after the drop: %u orphans, %u names", inv.orphans, inv.names);
    vol_save(&v, "links", true);
    vol_free(&v);
}

/* path's link reads back as target, `fast` (in the inode) or not. */
static void link_reads(struct vol *v, const char *path, const char *target, uint32_t len,
                       bool fast)
{
    struct ext_inode in;
    char buf[4096];
    size_t n;
    if (!walk(v, path, &in))
        fail("%s is missing", path);
    expect(ext_readlink(&v->fs, &in, buf, sizeof(buf), &n), OK, "readlink");
    if (n != len || memcmp(buf, target, len) || in.d.mode != (EXT_S_IFLNK | 0777))
        fail("%s: the link reads %zu bytes, mode %o", path, n, in.d.mode);
    uint64_t sectors = fast ? 0 : v->fs.block_size / 512;
    if (fast != !(in.d.flags & EXT_EXTENTS_FL) || ext_iblocks512(&v->fs, &in) != sectors)
        fail("%s: %s link with %llu sectors", path, fast ? "a fast" : "a slow",
             (unsigned long long)ext_iblocks512(&v->fs, &in));
}

static void symlinks_on(uint32_t block_size)
{
    struct vol v;
    small_vol(&v, block_size, 16u << 20);
    static char t[8192];
    memset(t, 'x', sizeof(t));
    memcpy(t, "../a/b/", 7);
    struct ext_inode in;
    uint32_t max = block_size - 1 < 4095 ? block_size - 1 : 4095;
    expect(symlink_to(&v, "fast", t, 59, &in), OK, "59 bytes");
    expect(symlink_to(&v, "slow", t, 60, &in), OK, "60 bytes");
    expect(symlink_to(&v, "longest", t, max, &in), OK, "the longest");
    link_reads(&v, "fast", t, 59, true);
    link_reads(&v, "slow", t, 60, false);
    link_reads(&v, "longest", t, max, false);
    vol_check(&v, NULL);
    vol_sync(&v);
    refused(&v, symlink_to(&v, "x", t, max + 1, &in), ERR_NAME_TOO_LONG, "one byte more");
    refused(&v, symlink_to(&v, "x", t, 0, &in), ERR_INVALID_ARGS, "empty");
    refused(&v, symlink_to(&v, "x", "a\0b", 3, &in), ERR_INVALID_ARGS, "a NUL");
    refused(&v, symlink_to(&v, "fast", t, 3, &in), ERR_ALREADY_EXISTS, "a name taken");
    vol_remount(&v, NULL);
    link_reads(&v, "fast", t, 59, true);
    link_reads(&v, "longest", t, max, false);
    expect(rm(&v, "slow", 0, NULL), OK, "unlink a slow link");
    expect(rm(&v, "fast", 0, NULL), OK, "unlink a fast link");
    vol_remount(&v, NULL);
    char name[32];
    snprintf(name, sizeof(name), "symlinks-%u", block_size);
    vol_save(&v, name, true);
    vol_free(&v);
}

static void symlinks(void)
{
    where = "symbolic links, 4 KiB blocks";
    symlinks_on(4096);
    where = "symbolic links, 1 KiB blocks";
    symlinks_on(1024);
}

static void dirs(void)
{
    where = "directories";
    struct vol v;
    small_vol(&v, 1024, 16u << 20);
    expect(make(&v, "a", EXT_S_IFDIR | 0755, NULL), OK, "mkdir a");
    expect(make(&v, "a/b", EXT_S_IFDIR | 0700, NULL), OK, "mkdir a/b");
    expect(make(&v, "a/f", EXT_S_IFREG | 0644, NULL), OK, "a/f");
    dir_is(&v, "a", 3, EXT_ROOT_INO);
    dir_is(&v, "a/b", 2, ino_of(&v, "a"));
    dir_is(&v, "", 4, EXT_ROOT_INO);
    vol_sync(&v);
    refused(&v, make(&v, "a", EXT_S_IFDIR | 0755, NULL), ERR_ALREADY_EXISTS, "mkdir a again");
    refused(&v, make(&v, "p", EXT_S_IFIFO | 0644, NULL), ERR_NOT_SUPPORTED, "a FIFO");
    refused(&v, rm(&v, "a", EXT_UNLINK_DIR, NULL), ERR_NOT_EMPTY, "rmdir a full one");
    refused(&v, rm(&v, "a/b", 0, NULL), ERR_IS_DIR, "unlink a directory");
    refused(&v, rm(&v, "a/f", EXT_UNLINK_DIR, NULL), ERR_NOT_DIR, "rmdir a file");
    refused(&v, rm(&v, "a/nope", 0, NULL), ERR_NOT_FOUND, "unlink nothing");
    refused(&v, rm(&v, "a/f", 2, NULL), ERR_INVALID_ARGS, "unknown flag");
    refused(&v, make(&v, "a/f/x", EXT_S_IFREG | 0644, NULL), ERR_NOT_DIR, "in a file");
    uint32_t o;
    struct ext_inode b;
    walk(&v, "a/b", &b);
    expect(rm(&v, "a/b", EXT_UNLINK_DIR, &o), OK, "rmdir a/b");
    if (o != b.ino)
        fail("rmdir: orphan %u, not %u", o, b.ino);
    dir_is(&v, "a", 2, EXT_ROOT_INO);
    vol_check(&v, NULL);
    struct ext_name n = nm("x");
    struct ext_attrs at = attrs(EXT_S_IFREG | 0644);
    struct ext_inode in;
    iget(&v, b.ino, &b);
    vol_sync(&v);
    refused(&v, ext_create(&v.fs, &b, &n, &at, &in), ERR_NOT_FOUND, "in a removed directory");
    expect(ext_orphan_drop(&v.fs, o, &at.now), OK, "drop");
    set_links(&v, ino_of(&v, "a"), EXT_LINK_MAX);
    refused(&v, make(&v, "a/c", EXT_S_IFDIR | 0755, NULL), ERR_NO_RESOURCES,
            "too many subdirectories");
    expect(make(&v, "a/c", EXT_S_IFREG | 0644, NULL), OK, "a file there");
    set_links(&v, ino_of(&v, "a"), 2);
    vol_remount(&v, NULL);
    vol_save(&v, "dirs", true);
    vol_free(&v);
}

static void bad_names(void)
{
    where = "names";
    struct vol v;
    small_vol(&v, 4096, 16u << 20);
    static char n255[256], n256[257];
    memset(n255, 'n', 255);
    memset(n256, 'n', 256);
    vol_sync(&v);
    struct ext_inode root, in;
    iget(&v, EXT_ROOT_INO, &root);
    static const struct { const char *s; uint32_t len; status_t want; } cases[] = {
        { "", 0, ERR_INVALID_ARGS }, { ".", 1, ERR_INVALID_ARGS }, { "..", 2, ERR_INVALID_ARGS },
        { "a/b", 3, ERR_INVALID_ARGS }, { "a\0b", 3, ERR_INVALID_ARGS },
        { n256, 256, ERR_NAME_TOO_LONG },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct ext_name n = { .s = cases[i].s, .len = cases[i].len };
        struct ext_attrs a = attrs(EXT_S_IFREG | 0644);
        refused(&v, ext_create(&v.fs, &root, &n, &a, &in), cases[i].want, "a bad name");
    }
    static const char *good[] = { n255, "Makefile", "makefile", "caf\xc3\xa9", "a b", "...", ".x" };
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        expect(make(&v, good[i], EXT_S_IFREG | 0644, NULL), OK, "a good name");
    if (ino_of(&v, "Makefile") == ino_of(&v, "makefile"))
        fail("Makefile and makefile are one file");
    vol_remount(&v, NULL);
    vol_save(&v, "names", true);
    vol_free(&v);
}

/* ---- orphans across a remount ------------------------------------------------------------ */

struct keep {
    uint32_t inos[4];
    unsigned n;
};

static bool keep_some(void *ctx, uint32_t ino)
{
    const struct keep *k = ctx;
    for (unsigned i = 0; i < k->n; i++)
        if (k->inos[i] == ino)
            return true;
    return false;
}

static uint32_t list_len(struct vol *v)
{
    uint32_t list[64], n;
    expect(ext_orphans(&v->fs, list, 64, &n), OK, "the orphan list");
    return n;
}

static void orphans(void)
{
    where = "orphans across a remount";
    struct vol v;
    small_vol(&v, 4096, 16u << 20);
    struct keep k = { .n = 2 };
    char path[16];
    for (unsigned i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "o%u", i);
        expect(make(&v, path, i < 5 ? EXT_S_IFDIR | 0755 : EXT_S_IFREG | 0644, NULL), OK, path);
    }
    for (unsigned i = 0; i < 8; i++) {
        uint32_t o;
        snprintf(path, sizeof(path), "o%u", i);
        expect(rm(&v, path, i < 5 ? EXT_UNLINK_DIR : 0, &o), OK, path);
        if (i == 1 || i == 6)
            k.inos[i == 6] = o;
    }
    struct inv inv;
    vol_remount(&v, &inv);   /* as after a cut: eight orphans, none freed */
    if (inv.orphans != 8 || list_len(&v) != 8)
        fail("%u orphans after the remount", inv.orphans);
    struct ext_time t = now();
    uint32_t done;
    expect(ext_orphans_recover(&v.fs, keep_some, &k, &t, 3, &done), OK, "recover");
    if (done != 3)
        fail("recover: %u, not 3", done);
    vol_sync(&v);
    expect(ext_orphans_recover(&v.fs, keep_some, &k, &t, 3, &done), OK, "recover");
    expect(ext_orphans_recover(&v.fs, keep_some, &k, &t, 3, &done), OK, "recover");
    if (done || list_len(&v) != 2)
        fail("recover: %u left on the list, last pass %u", list_len(&v), done);
    vol_remount(&v, &inv);
    if (inv.orphans != 2)
        fail("the kept orphans: %u", inv.orphans);
    vol_sync(&v);
    expect(ext_orphan_drop(&v.fs, EXT_ROOT_INO, &t), ERR_BAD_STATE, "drop with links");
    expect(ext_orphan_remove(&v.fs, EXT_ROOT_INO), ERR_NOT_FOUND, "remove: not on the list");
    expect(make(&v, "trunc", EXT_S_IFREG | 0644, NULL), OK, "trunc");
    struct ext_inode in;
    walk(&v, "trunc", &in);
    expect(ext_orphan_add(&v.fs, &in), OK, "a truncation's orphan");
    expect(ext_orphan_add(&v.fs, &in), OK, "the same again");
    expect(make(&v, "trunc2", EXT_S_IFREG | 0644, NULL), OK, "trunc2");
    walk(&v, "trunc2", &in);
    expect(ext_orphan_add(&v.fs, &in), OK, "another truncation's orphan");
    uint32_t o;
    expect(rm(&v, "trunc2", 0, &o), OK, "its last name");
    if (list_len(&v) != 4)
        fail("%u on the list, not 4", list_len(&v));
    vol_remount(&v, &inv);
    expect(ext_orphans_recover(&v.fs, NULL, NULL, &t, 100, &done), OK, "recover all");
    if (done != 4 || list_len(&v) || !walk(&v, "trunc", &in) || in.d.links_count != 1)
        fail("recover all: %u, %u left", done, list_len(&v));
    vol_remount(&v, &inv);
    if (inv.orphans || inv.names != 2)
        fail("after recovery: %u orphans, %u names", inv.orphans, inv.names);
    vol_save(&v, "orphans", true);
    vol_free(&v);
}

/* ---- an indexed directory ---------------------------------------------------------------- */

static void indexed(const struct img *fx)
{
    where = "linux1k's indexed directory, written";
    struct vol v;
    vol_new(&v, fx->size);
    memcpy(v.bytes, fx->bytes, fx->size);
    vol_mount(&v);
    struct ext_inode many;
    walk(&v, "many", &many);
    if (!(many.d.flags & EXT_INDEX_FL))
        fail("many isn't indexed");
    expect(make(&v, "many/new-one", EXT_S_IFREG | 0644, NULL), OK, "an add");
    walk(&v, "many", &many);
    if (many.d.flags & EXT_INDEX_FL)
        fail("many is still indexed");
    struct ext_dentry d;
    unsigned n = 0;
    for (uint64_t pos = 0;; n++) {
        expect(ext_dir_next(&v.fs, &many, &pos, &d), OK, "readdir");
        if (!d.ino)
            break;
    }
    if (n != 150 + 3)
        fail("%u entries", n);
    vol_sync(&v);
    vol_mount_hash(&v, false);
    char name[64];
    for (unsigned i = 0; i < 150; i++) {
        snprintf(name, sizeof(name), "many/entry-%03u-with-a-longer-name", i);
        if (!walk(&v, name, NULL))
            fail("%s is gone", name);
    }
    expect(rm(&v, "many/entry-007-with-a-longer-name", 0, NULL), OK, "unlink");
    expect(mv(&v, "many/entry-008-with-a-longer-name", "many/entry-007-with-a-longer-name", 0,
              NULL), OK, "rename");
    vol_remount(&v, NULL);
    struct ext_time t = now();
    uint32_t done;
    expect(ext_orphans_recover(&v.fs, NULL, NULL, &t, 100, &done), OK, "linux1k's orphans");
    vol_remount(&v, NULL);
    vol_save(&v, "indexed", true);
    vol_free(&v);
}

/* ---- the hash after a given-up transaction, the room hint -------------------------------- */

static void discard(void)
{
    where = "the directory hash after ext_discard";
    struct vol v;
    small_vol(&v, 4096, 16u << 20);
    expect(make(&v, "x", EXT_S_IFREG | 0644, NULL), OK, "x");
    vol_sync(&v);
    struct ext_super sb = v.fs.sb;
    uint32_t ino = ino_of(&v, "x"), o;
    expect(rm(&v, "x", 0, &o), OK, "unlink x");
    gone(&v, "x");
    ext_discard(&v.fs);
    v.fs.sb = sb;   /* the transaction's superblock changes go with it */
    if (ino_of(&v, "x") != ino)
        fail("x lost by a transaction given up");
    vol_check(&v, NULL);
    vol_free(&v);
}

static void room_hint(void)
{
    where = "an add after a removal";
    struct vol v;
    small_vol(&v, 1024, 16u << 20);
    expect(make(&v, "h", EXT_S_IFDIR | 0755, NULL), OK, "h");
    char path[32];
    struct ext_inode dir;
    for (unsigned i = 0; walk(&v, "h", &dir) && ext_isize(&dir) < 3 * 1024; i++) {
        snprintf(path, sizeof(path), "h/n%04u", i);
        expect(make(&v, path, EXT_S_IFREG | 0644, NULL), OK, path);
    }
    expect(rm(&v, "h/n0003", 0, NULL), OK, "unlink in block 0");
    expect(make(&v, "h/m0003", EXT_S_IFREG | 0644, NULL), OK, "the same length again");
    struct ext_dentry d;
    struct ext_name n = nm("m0003");
    walk(&v, "h", &dir);
    expect(ext_lookup(&v.fs, &dir, &n, &d), OK, "lookup");
    if (d.pos >= 1024)
        fail("the new entry went to byte %llu, not the gap in block 0",
             (unsigned long long)d.pos);
    vol_remount(&v, NULL);
    vol_save(&v, "room", true);
    vol_free(&v);
}

void check_names(struct img *fixtures, unsigned n, unsigned storms)
{
    const struct img *jam = NULL, *linux1k = NULL;
    for (unsigned i = 0; i < n; i++) {
        if (!strcmp(fixtures[i].name, "jam4k"))
            jam = &fixtures[i];
        if (!strcmp(fixtures[i].name, "linux1k"))
            linux1k = &fixtures[i];
    }
    if (!jam || !linux1k)
        fail("names: the jam4k and linux1k fixtures are needed");
    check_mkfs(jam);
    bad_names();
    dirs();
    links();
    symlinks();
    renames();
    orphans();
    discard();
    room_hint();
    indexed(linux1k);
    check_big_dir();
    check_storm(storms);
}
