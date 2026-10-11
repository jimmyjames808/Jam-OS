/* extvol: the writing tests' volumes in memory and their steps by path
 * (tools/extvol.h has the model). The device checks every read and write
 * is whole kibibytes inside the volume; a mount must be writable (the
 * fixtures' journal replay is not this part's: its "needs recovery" mark
 * is ignored here); every step checks no buffer is left pinned. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extvol.h"
#include "extwrite.h"

int64_t clock_now = 1800000000;

static void check_io(struct vol *v, uint64_t off, uint32_t len)
{
    if (!len || off % 1024 || len % 1024 || off > v->size || len > v->size - off)
        fail("device: %u bytes at %llu", len, (unsigned long long)off);
}

static status_t vread(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    struct vol *v = ctx;
    check_io(v, off, len);
    memcpy(buf, v->bytes + off, len);
    return OK;
}

static status_t vwrite(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    struct vol *v = ctx;
    check_io(v, off, len);
    memcpy(v->bytes + off, buf, len);
    v->writes++;
    return OK;
}

struct ext_dev vol_dev(struct vol *v)
{
    return (struct ext_dev){ .ctx = v, .bytes = v->size, .read = vread, .write = vwrite };
}

void vol_new(struct vol *v, uint64_t size)
{
    memset(v, 0, sizeof(*v));
    v->size = size;
    v->bytes = calloc(1, size);
    v->cache = malloc(VCACHE_BYTES);
    v->names_bytes = ext_names_mem(VNAMES);
    v->names = malloc(v->names_bytes);
    if (!v->bytes || !v->cache || !v->names)
        fail("out of memory");
}

void vol_free(struct vol *v)
{
    free(v->bytes);
    free(v->cache);
    free(v->names);
}

status_t vol_mkfs(struct vol *v, const struct ext_mkfs *p)
{
    static uint8_t mem[MKFS_MEM];
    struct ext_dev dev = vol_dev(v);
    return ext_mkfs(&dev, p, mem, sizeof(mem));
}

/* Mount v writable, with the directory hash (or without: hash false). */
void vol_mount_hash(struct vol *v, bool hash)
{
    struct ext_dev dev = vol_dev(v);
    status_t st = ext_mount(&v->fs, &dev, v->cache, VCACHE_BYTES, 0);
    if (st != OK)
        fail("mount: %d (%s)", st, v->fs.why ? v->fs.why : "");
    v->fs.ro &= ~(uint32_t)EXT_RO_RECOVER;   /* the fixtures need no replay here */
    if (v->fs.ro)
        fail("mounted read-only: %#x", v->fs.ro);
    if (ext_count_free(&v->fs) != OK)   /* a writer's mount: the allocator's rule */
        fail("count free");
    if (hash && ext_names_init(&v->fs, v->names, v->names_bytes) != OK)
        fail("names init");
}

void vol_mount(struct vol *v)
{
    vol_mount_hash(v, true);
}

void vol_sync(struct vol *v)
{
    check_pins(&v->fs);
    status_t st = ext_sync(&v->fs);
    if (st != OK)
        fail("sync: %d", st);
}

void vol_save(struct vol *v, const char *what, bool list)
{
    char path[1024];
    if (!save_dir)
        return;
    vol_sync(v);
    snprintf(path, sizeof(path), "%s/names-%s.img", save_dir, what);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(v->bytes, 1, v->size, f) != v->size || fclose(f))
        fail("%s: can't write it", path);
    if (!list)
        return;
    snprintf(path, sizeof(path), "%s/names-%s.list", save_dir, what);
    write_listing(&v->fs, path);
}

void vol_check(struct vol *v, struct inv *out)
{
    struct inv dummy;
    check_invariants(&v->fs, out ? out : &dummy);
    verify_volume(&v->fs);   /* the allocator's checker too: two views of the blocks */
}

/* Sync, mount again from the bytes, check. */
void vol_remount(struct vol *v, struct inv *out)
{
    vol_sync(v);
    vol_mount(v);
    vol_check(v, out);
}

struct ext_name nm(const char *s)
{
    return (struct ext_name){ .s = s, .len = (uint32_t)strlen(s) };
}

struct ext_time now(void)
{
    return (struct ext_time){ .sec = clock_now++, .nsec = 123456789 };
}

struct ext_attrs attrs(uint16_t mode)
{
    return (struct ext_attrs){ .mode = mode, .uid = 1000, .gid = 1000,
                               .generation = (uint32_t)rnd(), .now = now() };
}

void expect(status_t got, status_t want, const char *what)
{
    if (got != want)
        fail("%s: %d, not %d", what, got, want);
}

void iget(struct vol *v, uint32_t ino, struct ext_inode *out)
{
    status_t st = ext_iget(&v->fs, ino, EXT_IGET_UNLINKED, out);
    if (st != OK)
        fail("inode %u: %d (%s)", ino, st, v->fs.why ? v->fs.why : "");
}

/* The inode at path (from the root, names split by '/'); 0 if missing. */
uint32_t walk(struct vol *v, const char *path, struct ext_inode *out)
{
    struct ext_inode in;
    iget(v, EXT_ROOT_INO, &in);
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *save = NULL, *p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        struct ext_dentry d;
        struct ext_name n = nm(p);
        status_t st = ext_lookup(&v->fs, &in, &n, &d);
        if (st == ERR_NOT_FOUND)
            return 0;
        if (st != OK)
            fail("%s: lookup %s: %d", path, p, st);
        iget(v, d.ino, &in);
    }
    if (out)
        *out = in;
    check_pins(&v->fs);
    return in.ino;
}

/* Split path into its directory (*dir, read) and its last name. */
struct ext_name split(struct vol *v, const char *path, struct ext_inode *dir)
{
    const char *slash = strrchr(path, '/');
    char parent[1024] = "";
    if (slash)
        snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
    if (!walk(v, parent, dir))
        fail("%s: no parent", path);
    return nm(slash ? slash + 1 : path);
}

status_t make(struct vol *v, const char *path, uint16_t mode, struct ext_inode *out)
{
    struct ext_inode dir, in;
    struct ext_name n = split(v, path, &dir);
    struct ext_attrs a = attrs(mode);
    status_t st = ext_create(&v->fs, &dir, &n, &a, out ? out : &in);
    check_pins(&v->fs);
    return st;
}

status_t rm(struct vol *v, const char *path, uint32_t flags, uint32_t *orphan)
{
    struct ext_inode dir;
    struct ext_name n = split(v, path, &dir);
    struct ext_time t = now();
    uint32_t o = 0;
    status_t st = ext_unlink(&v->fs, &dir, &n, flags, &t, &o);
    check_pins(&v->fs);
    if (orphan)
        *orphan = o;
    else if (st == OK && o)
        expect(ext_orphan_drop(&v->fs, o, &t), OK, "drop");
    return st;
}

status_t mv(struct vol *v, const char *from, const char *to, uint32_t flags, uint32_t *orphan)
{
    struct ext_inode fd, td;
    struct ext_rename r = { .from_dir = &fd, .to_dir = &td, .flags = flags, .now = now() };
    r.from = split(v, from, &fd);
    r.to = split(v, to, &td);
    uint32_t o = 0;
    status_t st = ext_rename(&v->fs, &r, &o);
    check_pins(&v->fs);
    if (orphan)
        *orphan = o;
    else if (st == OK && o)
        expect(ext_orphan_drop(&v->fs, o, &r.now), OK, "drop");
    return st;
}

/* A refusal: the status, and nothing changed. */
void refused(struct vol *v, status_t got, status_t want, const char *what)
{
    expect(got, want, what);
    if (v->fs.cache.ndirty)
        fail("%s: refused, but %u blocks changed", what, v->fs.cache.ndirty);
}

void small_vol(struct vol *v, uint32_t block_size, uint64_t size)
{
    struct ext_mkfs p = { .block_size = block_size, .uid = 1000, .gid = 1000, .now = clock_now,
                          .label = "names" };
    memset(p.uuid, 0x5a, sizeof(p.uuid));
    vol_new(v, size);
    expect(vol_mkfs(v, &p), OK, "mkfs");
    vol_mount(v);
}

uint32_t ino_of(struct vol *v, const char *path)
{
    uint32_t ino = walk(v, path, NULL);
    if (!ino)
        fail("%s is missing", path);
    return ino;
}

void gone(struct vol *v, const char *path)
{
    if (walk(v, path, NULL))
        fail("%s is still there", path);
}

/* The links count and ".." of directory path. */
void dir_is(struct vol *v, const char *path, uint32_t links, uint32_t parent)
{
    struct ext_inode in;
    struct ext_dentry d;
    if (!walk(v, path, &in))
        fail("%s is missing", path);
    expect(ext_dir_lookup(&v->fs, &in, "..", 2, &d), OK, "..");
    if (in.d.links_count != links || d.ino != parent)
        fail("%s: %u links, .. %u; not %u, %u", path, in.d.links_count, d.ino, links, parent);
}

void set_links(struct vol *v, uint32_t ino, uint16_t links)
{
    struct ext_inode in;
    iget(v, ino, &in);
    in.d.links_count = links;
    expect(ext_iput(&v->fs, &in), OK, "iput");
    vol_sync(v);
}

status_t link_to(struct vol *v, const char *from, const char *to)
{
    struct ext_inode in, dir;
    struct ext_time t = now();
    if (!walk(v, from, &in))
        fail("%s is missing", from);
    struct ext_name n = split(v, to, &dir);
    status_t st = ext_link(&v->fs, &dir, &n, &in, &t);
    check_pins(&v->fs);
    return st;
}

status_t symlink_to(struct vol *v, const char *path, const char *target, uint32_t len,
                    struct ext_inode *out)
{
    struct ext_inode dir;
    struct ext_name n = split(v, path, &dir);
    struct ext_name t = { .s = target, .len = len };
    struct ext_attrs a = attrs(EXT_S_IFREG | 0600);
    status_t st = ext_symlink(&v->fs, &dir, &n, &t, &a, out);
    check_pins(&v->fs);
    return st;
}

