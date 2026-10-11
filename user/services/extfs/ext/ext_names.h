/* ext: names and links, the library's write side for directories. New
 * files, directories and symbolic links; hard links and unlinks; rename
 * (replacing, without replacing, exchanging; moving a directory, never
 * into itself); the orphan list (inodes with no name left, kept while
 * something still has them open, freed after); and the in-memory hash of
 * directories that makes a lookup in a big directory one probe.
 *
 * The model, on top of <ext.h>'s:
 *   - Every call here is one step of a transaction: it changes blocks in
 *     the cache (dirty) and nothing on the disk. The caller commits
 *     (ext_sync, or the journal) or gives the transaction up
 *     (ext_discard). A call that fails before changing anything leaves
 *     nothing dirty (a name taken, missing, a directory not empty, a bad
 *     name); one that fails later (no space, damage) leaves the
 *     transaction to be given up.
 *   - The library has no clock: the caller passes the time every change
 *     is stamped with. It has no idea which files are open either: an
 *     inode whose last name goes is put on the orphan list (on the disk,
 *     so a cut frees it at the next mount) and handed back to the
 *     caller, which frees it at once (ext_orphan_drop) or at its last
 *     close.
 *   - Directories are lists of blocks. An entry never moves while it
 *     lives (readdir's cookie, its byte offset, stays good); a removed
 *     one becomes part of the one before it in its block. A directory
 *     Linux indexed (dir_index) is turned into a plain one by the first
 *     change to it: its index blocks become empty entry blocks and its
 *     index flag goes (e2fsck -D can index it again).
 *   - Link counts: at most EXT_LINK_MAX; a directory counts its "." and
 *     each subdirectory's "..". A directory another system left at 1
 *     (dir_nlink: "too many to count") stays at 1.
 *   - The directory hash (ext_names_init) is memory the caller gives,
 *     rebuilt as directories are looked into, never stored: every change
 *     here keeps it in step, and it starts over after an ext_discard. A
 *     lookup through it reads one directory block.
 *
 * Errors, on top of <ext.h>'s: ERR_INVALID_ARGS (a bad name: empty, "."
 * or "..", holding '/' or NUL; unknown flags), ERR_NAME_TOO_LONG,
 * ERR_ALREADY_EXISTS, ERR_NOT_FOUND, ERR_NOT_DIR, ERR_IS_DIR,
 * ERR_NOT_EMPTY, ERR_NO_SPACE, ERR_NO_RESOURCES (a link count at
 * EXT_LINK_MAX: POSIX's EMLINK), ERR_ACCESS_DENIED (the volume may not be
 * written). Permission bits are the caller's to check. */
#pragma once

#include "ext.h"

#define EXT_LINK_MAX 65000          /* the most links an inode may have */

/* A name in a directory: 1 to 255 bytes, no '/' or NUL, not "." or "..". */
struct ext_name {
    const char *s;
    uint32_t len;
};

/* What a new inode is made with. */
struct ext_attrs {
    uint16_t mode;                  /* its type (EXT_S_IF*) and permission bits */
    uint32_t uid, gid;
    uint32_t generation;            /* the caller's (random): seeds its checksums */
    struct ext_time now;            /* its times, and its directory's change */
};

/* ---- making and removing names (links.c) ------------------------------------------------ */

/* A new regular file (mode EXT_S_IFREG) or directory (EXT_S_IFDIR: "."
 * and ".." in its first block) named `name` in dir; *out the new inode.
 * dir is written back (its size, times, links) and stays current.
 * ERR_NOT_SUPPORTED for any other type. */
status_t ext_create(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    const struct ext_attrs *a, struct ext_inode *out);

/* A symbolic link to `target` (1 to 4095 bytes, no NUL, shorter than a
 * block): in the inode itself when under 60 bytes, else in a block of
 * its own. Its mode is EXT_S_IFLNK | 0777 (a's mode bits are not used). */
status_t ext_symlink(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                     const struct ext_name *target, const struct ext_attrs *a,
                     struct ext_inode *out);

/* Another name for `in` (not a directory: ERR_IS_DIR; one with no names
 * left: ERR_NOT_FOUND); in's links and change time are written back. */
status_t ext_link(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                  struct ext_inode *in, const struct ext_time *now);

#define EXT_UNLINK_DIR 0x1          /* rmdir: the name must be an empty directory */

/* Remove `name` from dir. A file: ERR_IS_DIR without EXT_UNLINK_DIR; a
 * directory: ERR_NOT_DIR with it unless one, ERR_NOT_EMPTY unless empty.
 * *out_orphan: the inode left with no name (on the orphan list, to be
 * dropped), else 0. */
status_t ext_unlink(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    uint32_t flags, const struct ext_time *now, uint32_t *out_orphan);

/* The entry `name` in dir, through the directory hash when there is one
 * (ext_dir_lookup's answers otherwise). ERR_NOT_FOUND; ERR_NOT_DIR. */
status_t ext_lookup(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    struct ext_dentry *out);

/* ---- rename (rename.c) ------------------------------------------------------------------ */

#define EXT_RENAME_NOREPLACE 0x1    /* ERR_ALREADY_EXISTS if the new name is there */
#define EXT_RENAME_EXCHANGE  0x2    /* swap the two names (both must be there) */

struct ext_rename {
    struct ext_inode *from_dir;     /* written back and kept current, as ... */
    struct ext_name from;
    struct ext_inode *to_dir;       /* ... this one (the same directory or not) */
    struct ext_name to;
    uint32_t flags;                 /* EXT_RENAME_* */
    struct ext_time now;
};

/* Rename r->from to r->to as one change: an inode keeps its number; a
 * name it replaces is gone in the same step (no moment without either);
 * renaming a name onto another name of the same inode does nothing. A
 * directory replaces only an empty directory (ERR_NOT_EMPTY), a file only
 * a file (ERR_IS_DIR, ERR_NOT_DIR); a directory moved below itself is
 * ERR_INVALID_ARGS. *out_orphan: the inode a replace left with no name,
 * else 0. */
status_t ext_rename(struct ext_fs *fs, const struct ext_rename *r, uint32_t *out_orphan);

/* ---- the orphan list (orphan.c) --------------------------------------------------------- */

/* Put `in` (written back) at the head of the orphan list. */
status_t ext_orphan_add(struct ext_fs *fs, struct ext_inode *in);
/* Take inode `ino` off the list. ERR_NOT_FOUND if it isn't on it. */
status_t ext_orphan_remove(struct ext_fs *fs, uint32_t ino);
/* Free an orphan with no links: its blocks freed, off the list, its
 * attribute block (shared ones counted down) released, its deletion time
 * `now`, its inode freed. ERR_BAD_STATE if it still has links;
 * ERR_SHOULD_WAIT when it holds more than a transaction should free
 * (part of it freed, still on the list: commit and call again);
 * ERR_NOT_SUPPORTED for a block-mapped file (it stays on the list). */
status_t ext_orphan_drop(struct ext_fs *fs, uint32_t ino, const struct ext_time *now);

/* Which orphans a successor keeps (still open in its state). */
typedef bool (*ext_keep_fn)(void *ctx, uint32_t ino);

/* At a mount: drop up to `max` orphans with no links that keep (NULL:
 * none kept) doesn't keep, and finish the truncation of those with links
 * (taken off the list); *out_done: how many were dropped or finished.
 * Call again (after committing) while *out_done == max. */
status_t ext_orphans_recover(struct ext_fs *fs, ext_keep_fn keep, void *ctx,
                             const struct ext_time *now, uint32_t max, uint32_t *out_done);

/* ---- the directory hash (names.c) ------------------------------------------------------- */

/* One directory's entries in the hash, while it is there. */
struct ext_names_dir {
    uint32_t ino;                   /* 0: a free slot */
    uint32_t gen;                   /* its inode's generation: a reused number is another */
    uint32_t first;                 /* its first entry, index + 1; 0: none */
    uint32_t count;                 /* entries it has here */
    uint64_t used;                  /* the clock at its last use: the oldest goes first */
    uint32_t room_from;             /* every block before this one ... */
    uint32_t room_need;             /* ... lacks a gap this many bytes long */
};

/* One name. */
struct ext_names_ent {
    uint64_t pos;                   /* the entry's byte offset in its directory */
    uint32_t hash;                  /* the name's hash */
    uint32_t next;                  /* the next in its bucket, index + 1 */
    uint32_t dnext, dprev;          /* its directory's list (dnext: the free list too) */
    uint32_t dir;                   /* its directory's slot */
};

struct ext_names {
    struct ext_names_dir *dirs;
    uint32_t ndirs;
    struct ext_names_ent *ents;
    uint32_t nents;
    uint32_t *heads;                /* the buckets: the first entry's index + 1 */
    uint32_t shift;                 /* 64 - log2(the number of buckets) */
    uint32_t free;                  /* the free entries, index + 1 */
    uint64_t clock;
    uint64_t discards;              /* fs->cache.discards it is in step with */
    uint64_t builds;                /* directories read in whole */
    uint64_t hits, misses;          /* lookups answered by a probe */
    uint64_t linear;                /* lookups by a walk (a directory too big for it) */
};

/* Bytes the hash needs for `nents` names. */
size_t ext_names_mem(uint32_t nents);
/* Make the hash in mem (as many names as fit; kept by the caller while
 * fs is used) and give it to fs (fs->names). ERR_NO_MEMORY if too small. */
status_t ext_names_init(struct ext_fs *fs, void *mem, size_t size);
/* Forget every directory in it. */
void ext_names_reset(struct ext_fs *fs);
