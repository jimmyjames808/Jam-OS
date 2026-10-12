/* extcheck's write tests (U2a: allocation and extents): tools/extwrite.c
 * (the device, the helpers, blocks and inodes) and tools/extfile.c
 * (extent trees, file writes and truncation). Each test mounts its own
 * copy of a writable fixture (jam4k, linux1k) on a device without an undo
 * log, changes it through the library, writes it home (ext_sync, as a
 * commit would) and checks the whole volume (extverify.c) as it goes. */
#pragma once

#include <ext_alloc.h>
#include <ext_names.h>
#include "extcheck.h"

/* A volume mounted read-write over a private copy of a fixture. */
struct wfs {
    const char *name;             /* the fixture's */
    uint8_t *bytes;
    uint64_t size;
    uint8_t *cache;
    struct ext_fs fs;
    uint64_t free0;               /* free blocks and inodes at the mount */
    uint32_t ifree0;
};

extern const char *save_dir;      /* --save: where the images e2fsck should see go (NULL: none) */

void wmount(struct wfs *w, const struct img *m);
/* Write every dirty block home (must succeed), no pins left. */
void wsync(struct wfs *w);
/* Mount the bytes again (after a wsync): what the disk holds. */
void wremount(struct wfs *w);
void wdone(struct wfs *w);
/* The free counts are back to what they were at the mount. */
void wcounts_back(struct wfs *w);

/* A new inode: a regular file (mode 0100644), owner 1000, links 1. */
void new_file(struct wfs *w, uint32_t gen, struct ext_inode *out);
/* Bytes of the test pattern: file `seed`'s byte at off. */
void pattern(uint32_t seed, uint64_t off, uint8_t *buf, size_t n);
/* Write n pattern bytes at off; must write all of them. */
void write_pat(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n);
/* Read [off, off + n) and compare with the pattern (or zeros). */
void expect_pat(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n);
void expect_zero(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n);
/* Truncate to size, step by step, syncing after each; the steps taken. */
unsigned truncate_all(struct wfs *w, struct ext_inode *in, uint64_t size);
/* The extent tree's depth and its extents. */
uint32_t tree_depth(const struct ext_inode *in);
uint32_t tree_extents(struct wfs *w, struct ext_inode *in);
/* Save a copy of the volume with files[i] linked as /emptydir/names[i]
 * (save_dir/<fixture>-<what>.img), for e2fsck -fn on the PC. */
void save_linked(struct wfs *w, const char *what, const uint32_t *files, const char *const *names,
                 unsigned n);

/* extverify.c */
void verify_volume(struct ext_fs *fs);

/* extfile.c */
void check_file_writes(const struct img *m);
/* extwrite.c: the entry point */
void check_writes(const struct img *m, bool writable);
