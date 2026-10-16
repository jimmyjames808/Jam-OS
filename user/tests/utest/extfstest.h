/* utest: what the extfs tests share (extfs.c): an extfs process
 * (bin/extfs, user/services/extfs) over a RAM disk, run as fat's tests run
 * fat (fattest.h: the same struct fatrun and the same client calls, which
 * speak the protocols, not FAT). */
#pragma once

#include "fattest.h"

#define EXT_DISK_SECTORS (32u * MIB_SECTORS)   /* the tests' disks: 32 MiB */

/* Start extfs over rd with EXTFS_ARG_FORMAT (a blank disk is formatted)
 * and the words in extra (n of them: test powers). */
bool extfs_start(struct fatrun *r, struct ramdisk *rd, bool read_only, const char *const *extra,
                 unsigned n);
/* The same, writable, handed a copy of pager (EXTFS_SR_PAGER, with the
 * rights volumes gives): its files can be mapped. */
bool extfs_start_pager(struct fatrun *r, struct ramdisk *rd, handle_t pager);
/* The disk as it is now, saved (sparse) as /data/extfs-e2fsck/<name>.sparse
 * when that directory exists (tools/extfs-svc-test.sh makes it, then runs
 * Linux's e2fsck on every image): only after extfs has stopped (the journal
 * closed). */
bool extfs_save(struct ramdisk *rd, const char *name);
/* fs.getattr's mode and link count (flags: FS_NOFOLLOW or 0). */
status_t xmode(handle_t fs, const char *path, uint32_t flags, uint32_t *mode, uint32_t *nlink);
