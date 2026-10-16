/* jamext: Jam OS's ext volumes on the Mac (and any Unix), built from the
 * same library the extfs service runs (user/services/extfs/ext), so there
 * is no e2fsprogs to install. See main.c for the commands.
 *
 * A volume is a file (an image), or a part of one: --offset and --size
 * pick it out of a bigger file, --part N finds partition N of the file's
 * GPT or MBR. Writes go through the library's journal as extfs's requests
 * do, so a tool that dies mid-way leaves a volume the next mount (this
 * tool's or Jam OS's) replays; a volume that needs it is replayed when it
 * is opened for writing. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ext.h>
#include <ext_alloc.h>
#include <ext_check.h>
#include <ext_jnl.h>
#include <ext_mkfs.h>
#include <ext_names.h>

#define JX_USER      1000u          /* the one user: every file belongs to it */
#define JX_CHUNK     (1u << 20)     /* file data moves a megabyte at a time */

struct jx_opts {
    const char *image;
    uint64_t    offset;             /* the volume's place in the file */
    uint64_t    size;               /* 0: the file's rest (or the superblock's say) */
    int         part;               /* 0: none; else the partition table's Nth */
    int64_t     now;                /* the time every change is stamped with */
    uint64_t    seed;               /* 0: random */
    bool        keep_modes;         /* import: the host's permission bits as they are */
    bool        keep_times;         /* import: the host's modification times */
    bool        replay;             /* read-only commands: replay the journal first */
    bool        quiet;
};

struct jx {
    struct jx_opts o;
    int            fd;
    uint64_t       base;            /* the volume's first byte in the file */
    uint64_t       bytes;           /* its size */
    bool           write;           /* opened for writing: the journal is open */
    struct ext_fs  fs;
    struct ext_jnl jnl;
    void          *cache, *jmem, *names;
    size_t         cache_size, jsize;
    uint64_t       rng;
};

/* vol.c */
/* Open the volume (--offset, --size, --part applied). For writing: mount it
 * with its journal (replayed if need be), finish the orphans a cut left. For
 * reading: mount read-only. Failures are said on stderr; false. */
bool jx_open(struct jx *x, const struct jx_opts *o, bool write);
/* Close: the journal marked empty, the file synced and closed. */
bool jx_close(struct jx *x);
/* A volume made on the file: its size, label and the rest of ext_mkfs's
 * parameters (p; uuid and now filled in here when zero). A new file is
 * created. */
bool jx_mkfs(const struct jx_opts *o, uint64_t size, struct ext_mkfs *p);
/* The transaction a change runs in: begin, then commit or abort. */
bool jx_begin(struct jx *x);
bool jx_commit(struct jx *x);
void jx_abort(struct jx *x);
/* A random 32-bit generation number for a new inode. */
uint32_t jx_random(struct jx *x);
/* What a status means, in words. */
const char *jx_why(status_t st);
/* Say "jamext: <what>: <why>" on stderr. */
void jx_fail(const char *what, status_t st);

/* path.c */
struct jx_loc {
    struct ext_inode dir;           /* the directory that holds (or would hold) the name */
    char             name[EXT_NAME_MAX + 1];
    uint32_t         name_len;      /* 0: the path was the root */
    struct ext_inode in;            /* the name's inode, when found */
    bool             found;
};
/* Walk path from the volume's root. Links in the middle of a path are
 * followed (an absolute target from the root); the last name's, only
 * when follow_last. OK: found says whether the last name exists (its
 * directory does). ERR_NOT_FOUND: a directory on the way is missing;
 * ERR_NOT_DIR; ERR_LINK_LOOP; ERR_NAME_TOO_LONG. */
status_t jx_locate(struct jx *x, const char *path, bool follow_last, struct jx_loc *loc);

/* ops.c */
struct ext_attrs jx_attrs(struct jx *x, uint16_t mode);
struct ext_time  jx_time(const struct jx *x);
/* The directories of path made (like mkdir -p) and the directory itself. */
status_t jx_mkdir(struct jx *x, const char *path, uint16_t mode, bool parents);
/* The host file (a path, or "-" for stdin) as the file at path (parents
 * made; a file already there is replaced). mode: the permission bits. */
status_t jx_put(struct jx *x, const char *host, const char *path, uint16_t mode);
/* The file at path to the host file (a path, or "-" for stdout). */
status_t jx_get(struct jx *x, const char *path, const char *host);
/* A symbolic link at path to target, or a hard link to the file at target. */
status_t jx_ln(struct jx *x, const char *target, const char *path, bool symbolic);
status_t jx_chmod(struct jx *x, const char *path, uint16_t mode);
status_t jx_rm(struct jx *x, const char *path, bool recursive);
/* Print the volume's names (long: mode, links, owner, size, time, links'
 * targets); recursive lists the directories under path too. */
status_t jx_ls(struct jx *x, const char *path, bool longf, bool recursive);
/* Drop an inode left with no name (an unlink or a replace made it). */
status_t jx_drop(struct jx *x, uint32_t ino);
/* A file's data from the host's descriptor, in a transaction a megabyte;
 * the inode is the file's, size bytes of it. */
status_t jx_write_fd(struct jx *x, struct ext_inode *in, int fd, uint64_t size);

/* import.c */
/* The host directory host and everything under it, as the directory at
 * path of the volume (made): files, directories, links, hard links. Said
 * on stdout unless quiet; the number of names made in *count. */
status_t jx_import(struct jx *x, const char *host, const char *path, uint64_t *count);
/* The bytes a volume needs to hold the host directory's tree (data,
 * metadata and the journal, with room to spare) and the inodes it needs. */
bool jx_size_for(const char *host, uint64_t *out, uint32_t *inodes);
