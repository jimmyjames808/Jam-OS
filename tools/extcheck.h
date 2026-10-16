/* extcheck's files: tools/extcheck.c (the device, the fixtures'
 * listings, the main program), tools/extbreak.c (broken checksums, the
 * feature table, writes of what was read), tools/extmutate.c (the
 * mutations and the fuzz pass), tools/extinv.c (a volume's invariants)
 * and the writing tests (tools/extvol.h). The model is in extcheck.c's
 * header. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ext.h>

#define CACHE_BYTES (2u << 20)   /* the cache region of a mount: 512 blocks of 4 KiB */

/* A fixture image, held in memory, as the library's device. */
struct img {
    const char *name;
    uint8_t *bytes;
    uint64_t size;
    uint64_t dev_bytes;      /* the size the device says (a mutation may shrink it) */
    uint64_t reads;          /* device reads since reset */
    uint64_t read_limit;     /* more is a failure (a walk that doesn't end) */
    uint64_t fail_at;        /* the read that fails with ERR_IO (0: none) */
    bool     same_writes;    /* a write must leave the bytes as they are */
    uint8_t *cache;          /* CACHE_BYTES for its mounts */
    /* The undo log: every byte changed by poke, its old value. */
    uint64_t *undo_off;
    uint8_t  *undo_old;
    size_t   nundo, capundo;
};

extern uint64_t rng;
extern const char *where;    /* what is being checked, for failures */

void fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
uint64_t rnd(void);
uint64_t below(uint64_t n);

/* Change n bytes of the image at off, remembering the old ones; undo puts
 * every changed byte back. */
void poke(struct img *m, uint64_t off, const void *p, size_t n);
void undo(struct img *m);

/* Mount m (a cache of `bytes` bytes of m->cache), with fresh read counts. */
status_t mount_img(struct img *m, struct ext_fs *fs, size_t bytes, uint32_t flags);
/* No buffer may be pinned between calls. */
void check_pins(const struct ext_fs *fs);

/* A volume's listing: one line per name, as tools/extfix/listing.py
 * writes them (extcheck.c has the format). */
#define MAX_LINES 4096
struct lines {
    char *v[MAX_LINES];
    unsigned n;
    uint32_t orphans[16];
    unsigned norphans;
    unsigned xattr_blocks;        /* attribute blocks read (listing) */
};
/* Every name under the root, sorted (the library's walk, every file read). */
void list_volume(struct ext_fs *fs, struct lines *l);
/* <dir>/<name>.list, sorted; and two listings that must be the same. */
void read_list(const char *dir, const char *name, struct lines *l);
void compare_lines(const struct lines *want, const struct lines *got);
void free_lines(struct lines *l);
/* Every group's bitmaps read and checked. */
void check_bitmaps(struct ext_fs *fs);

/* The sparse hash of a regular file (tools/extfix/listing.py's), as hex. */
void sparse_hash(struct ext_fs *fs, struct ext_inode *in, char out[65]);
/* The inode of /name (failing the run if there is none). */
void lookup(struct ext_fs *fs, const char *name, struct ext_inode *out);

/* extbreak.c: each fails the run at the first mistake. */
void check_breaks(struct img *m);
void check_features(struct img *m);
void check_roundtrip(struct img *m);

/* The fuzz pass over one fixture: n runs (extmutate.c). */
void fuzz(struct img *m, unsigned n);

/* extinv.c: what a check of the whole volume found (it fails the run at
 * the first broken invariant). */
struct inv {
    uint32_t dirs, names, inodes, orphans;
    uint32_t free_inodes;
    uint64_t free_blocks;
    bool exact;              /* every block's owner was known */
};
void check_invariants(struct ext_fs *fs, struct inv *out);

/* extnames.c: the names' writes (directories, links, rename, orphans, the
 * directory hash) and mkfs, on images it makes and on the fixtures; a
 * storm of `storms` random steps. */
void check_names(struct img *fixtures, unsigned n, unsigned storms);
/* extdetect.c: the library's checker (ext_check) finds what is spoilt, one way
 * at a time, and nothing in a good volume. */
void check_detects(void);
/* extcheck.c: the volume listed as tools/extfix/listing.py lists it with
 * Linux's debugfs (at most 4096 names), into path. */
void write_listing(struct ext_fs *fs, const char *path);
