/* extvol: the writing tests' volumes (tools/extvol.c): an ext volume held
 * in memory as the library's device, mounted writable with the directory
 * hash, and the steps the tests take on it by path ("a/b/c", from the
 * root; "" is the root). Every helper fails the run (fail()) when the
 * library does something it must not: a read or write outside the
 * volume, a buffer left pinned, a mount refused. Used by extmkfs.c,
 * extnames.c and extbig.c. */
#pragma once

#include <ext_mkfs.h>
#include <ext_names.h>
#include "extcheck.h"

#define VCACHE_BYTES (4u << 20)        /* a mount's cache: 1024 blocks of 4 KiB */
#define VNAMES       32768u            /* names the directory hash holds */
#define MKFS_MEM     (256u << 10)      /* mkfs's scratch memory */

/* A volume in memory. */
struct vol {
    uint8_t *bytes;
    uint64_t size;
    uint64_t writes;                   /* device writes so far */
    uint8_t *cache;                    /* VCACHE_BYTES for its mounts */
    uint8_t *names;                    /* the directory hash's memory */
    size_t names_bytes;
    struct ext_fs fs;                  /* its mount */
};

extern int64_t clock_now;              /* the next time a change is stamped with */

/* The volume: made (zeros), freed, formatted, mounted (writable: a
 * read-only verdict fails the run), synced, checked (extinv.c), synced
 * and mounted again from its bytes and checked (a successor's view). */
void vol_new(struct vol *v, uint64_t size);
struct ext_dev vol_dev(struct vol *v);
void vol_free(struct vol *v);
status_t vol_mkfs(struct vol *v, const struct ext_mkfs *p);
void vol_mount_hash(struct vol *v, bool hash);
void vol_mount(struct vol *v);
void vol_sync(struct vol *v);
void vol_check(struct vol *v, struct inv *out);
void vol_remount(struct vol *v, struct inv *out);
/* With --save: sync, then write the volume to <save_dir>/names-<what>.img
 * (for Linux's e2fsck -fn) and, with `list`, the library's listing of it
 * beside it (.list, for debugfs's to match: tools/ext-e2fsck.sh). */
void vol_save(struct vol *v, const char *what, bool list);
/* A volume of `size` bytes mkfs made, mounted. */
void small_vol(struct vol *v, uint32_t block_size, uint64_t size);

struct ext_name nm(const char *s);
struct ext_time now(void);
/* A new inode's attributes: the mode, the one user, the next time. */
struct ext_attrs attrs(uint16_t mode);
void expect(status_t got, status_t want, const char *what);
/* A refusal: the status, and no block changed. */
void refused(struct vol *v, status_t got, status_t want, const char *what);

/* Inode ino (unlinked ones too). */
void iget(struct vol *v, uint32_t ino, struct ext_inode *out);
/* The inode at path (*out when not NULL); 0 if a name is missing. */
uint32_t walk(struct vol *v, const char *path, struct ext_inode *out);
/* The directory holding path's last name (*dir), and that name. */
struct ext_name split(struct vol *v, const char *path, struct ext_inode *dir);
uint32_t ino_of(struct vol *v, const char *path);   /* missing: the run fails */
void gone(struct vol *v, const char *path);         /* there: the run fails */
/* Directory path has `links` links and its ".." is `parent`. */
void dir_is(struct vol *v, const char *path, uint32_t links, uint32_t parent);
/* Write a link count straight into an inode (and sync). */
void set_links(struct vol *v, uint32_t ino, uint16_t links);

/* The library's calls by path. rm and mv drop the orphan they make in the
 * same transaction unless `orphan` asks for it instead. */
status_t make(struct vol *v, const char *path, uint16_t mode, struct ext_inode *out);
status_t rm(struct vol *v, const char *path, uint32_t flags, uint32_t *orphan);
status_t mv(struct vol *v, const char *from, const char *to, uint32_t flags, uint32_t *orphan);
status_t link_to(struct vol *v, const char *from, const char *to);
status_t symlink_to(struct vol *v, const char *path, const char *target, uint32_t len,
                    struct ext_inode *out);

/* extmkfs.c, extbig.c: the parts check_names (extnames.c) runs. */
void check_mkfs(const struct img *jam4k);
void check_big_dir(void);
void check_storm(unsigned steps);
