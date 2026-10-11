/* ext: what the names' files (dirent.c, names.c, links.c, rename.c,
 * orphan.c) share and no one else uses. */
#pragma once

#include "ext_alloc.h"
#include "ext_int.h"
#include "ext_names.h"

/* dirent.c: entries in a directory's blocks. Each changes blocks in the
 * cache and keeps the directory hash in step; dir (its size, its extent
 * root) is the caller's to write back. */

/* ERR_INVALID_ARGS or ERR_NAME_TOO_LONG for a name no entry may have. */
status_t ext_name_check(const struct ext_name *n);
/* Add an entry name -> ino of type `type` (enum ext_ftype), in the first
 * gap that holds it, or in a new block at the directory's end. */
status_t ext_dirent_add(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *n,
                        uint32_t ino, uint8_t type);
/* Remove the entry at byte pos (it must name ino). */
status_t ext_dirent_remove(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos, uint32_t ino);
/* Point the entry at pos (naming old) at ino, of type `type`. */
status_t ext_dirent_set(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos, uint32_t old,
                        uint32_t ino, uint8_t type);
/* Turn an indexed directory into a plain one (nothing if it isn't). */
status_t ext_dir_plain(struct ext_fs *fs, struct ext_inode *dir);
/* Does dir hold nothing but "." and ".."? */
status_t ext_dir_empty(struct ext_fs *fs, struct ext_inode *dir, bool *out);
/* The bytes of a leaf block entries may use (the tail after them). */
uint32_t ext_leaf_limit(const struct ext_fs *fs);
/* rec_len in bytes, as stored. */
uint16_t ext_rec_store(const struct ext_fs *fs, uint32_t len);
/* A leaf block changed: its checksum set, dirty. */
void ext_leaf_seal(struct ext_fs *fs, const struct ext_inode *dir, struct ext_buf *b);

/* names.c: the hash kept in step (each does nothing when dir isn't in it). */
void ext_names_added(struct ext_fs *fs, const struct ext_inode *dir, const struct ext_name *n,
                     uint64_t pos);
void ext_names_removed(struct ext_fs *fs, const struct ext_inode *dir, const char *name,
                       uint32_t len, uint64_t pos);
void ext_names_forget(struct ext_fs *fs, uint32_t ino);
/* The first block an entry of `need` bytes may fit in, as far as the hash knows. */
uint32_t ext_names_room_from(struct ext_fs *fs, const struct ext_inode *dir, uint32_t need);
/* Blocks before lblk had no gap of `need` bytes (an add found its gap in lblk). */
void ext_names_room_found(struct ext_fs *fs, const struct ext_inode *dir, uint32_t lblk,
                          uint32_t need);
/* A gap opened in block lblk. */
void ext_names_room_freed(struct ext_fs *fs, const struct ext_inode *dir, uint32_t lblk);

/* links.c: a block of its own for logical block lblk of in (a hole),
 * mapped and counted (in written back); its bytes are the caller's to
 * write (ext_bnew). */
status_t ext_block_new(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                       uint64_t *out_pblk);
/* Link counts. Can `in` take one more link? */
status_t ext_nlink_room(const struct ext_inode *in);
void ext_nlink_inc(struct ext_inode *in);
/* One less: a directory's count of subdirectories ("many" stays). */
void ext_nlink_dec(struct ext_inode *in);
/* in lost a name: its count down (a directory's to nothing), on the
 * orphan list when nothing is left (written back either way);
 * *out_orphan: in's number then, else 0. */
status_t ext_unlinked(struct ext_fs *fs, struct ext_inode *in, const struct ext_time *now,
                      uint32_t *out_orphan);
/* Set the change time (and with `mtime` the modification time). */
status_t ext_touch(struct ext_inode *in, const struct ext_time *now, bool mtime);
