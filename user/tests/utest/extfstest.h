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
/* ... and the words in extra too. */
bool extfs_start_pager_words(struct fatrun *r, struct ramdisk *rd, handle_t pager,
                             const char *const *extra, unsigned n);
/* The disk as it is now, saved (sparse) as /data/extfs-e2fsck/<name>.sparse
 * when that directory exists (tools/extfs-svc-test.sh makes it, then runs
 * Linux's e2fsck on every image): only after extfs has stopped (the journal
 * closed). */
bool extfs_save(struct ramdisk *rd, const char *name);
/* extfs_restart.c's supervisor: it plays the spawner (the state VMO, the
 * keeper, the `fs` channel's server end) and starts extfs over rd again
 * whenever it ends, instance i with the test power dies[i] (n of them).
 * Its client's channel is extfs_sup_run()'s; extfs_sup_ended counts the
 * instances a test power ended; after extfs_sup_fresh the next instance is
 * handed a new state (it starts fresh, the keeper's handles refused). */
bool           extfs_sup_start(struct ramdisk *rd, const char *const *dies, unsigned n);
bool           extfs_sup_stop(void);
struct fatrun *extfs_sup_run(void);
unsigned       extfs_sup_ended(void);
void           extfs_sup_fresh(void);
/* Instances killed (a crash or extfs_sup_storm's kill). */
unsigned       extfs_sup_crashes(void);
/* Kill the instance every every_ns from now (0: stop), as WR's whostile
 * kills extfs-jam with initctl.kill; each is started again. */
void           extfs_sup_storm(uint64_t every_ns);
/* Kill the instance and give it up, as volumes does past a service's
 * restart limit: no next start, the keeper released (its clients' ends
 * closed). False if that didn't come by deadline. extfs_sup_stop after. */
bool           extfs_sup_give_up(uint64_t deadline);
/* fs.getattr's mode and link count (flags: FS_NOFOLLOW or 0). */
status_t xmode(handle_t fs, const char *path, uint32_t flags, uint32_t *mode, uint32_t *nlink);
