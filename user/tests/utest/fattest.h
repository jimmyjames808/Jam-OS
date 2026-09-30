/* utest: what the fat tests share (fat_run.c): a fat service process
 * (bin/fat) running over a RAM disk (ramdisk.h), and a small client of its
 * `fs` and `file` channels. The client calls the protocols directly, not
 * through libos's namespace, so the tests need nothing but fat. Every call
 * has a deadline (FAT_CALL_NS), so a fat that hangs fails a test instead of
 * hanging utest. */
#pragma once

#include <fs_idl.h>
#include <os.h>
#include "ramdisk.h"

#define FAT_CALL_NS (20 * NS_PER_S)
#define FAT_BUF     (64u << 10)   /* a file's transfer buffer (file.idl) */
#define MIB_SECTORS 2048u   /* sectors per MiB */

/* One fat process and the disk under it. */
struct fatrun {
    struct ramdisk *rd;     /* the disk (not owned) */
    handle_t        job;    /* the job fat runs in, alone */
    handle_t        proc;   /* fat */
    handle_t        fs;     /* the client end of its `fs` channel */
};

/* One open file. */
struct tfile {
    handle_t ch;            /* its `file` channel */
    uint8_t *buf;           /* its transfer buffer, mapped */
    uint64_t size;          /* the size fs.open reported */
};

/* Start fat over rd (a session of the RAM disk, read-only or not). */
bool fat_start(struct fatrun *r, struct ramdisk *rd, bool read_only);
/* fat must end by itself with exit code `code` within FAT_CALL_NS and
 * leave its job empty. */
bool fat_wait(struct fatrun *r, int code);
/* Close the fs channel: fat must exit 0, and the RAM disk's session end. */
bool fat_stop(struct fatrun *r);

/* The protocol calls, with C strings for paths. */
status_t t_open(const struct fatrun *r, const char *path, uint32_t flags, struct tfile *f);
void     t_close(struct tfile *f);
status_t t_read(struct tfile *f, uint64_t off, void *dst, uint32_t n, uint32_t *done);
status_t t_write(struct tfile *f, uint64_t off, const void *src, uint32_t n, uint32_t *done);
status_t t_stat(const struct fatrun *r, const char *path, uint64_t *size, bool *is_dir,
                uint64_t *mtime);
/* name: FS_PATH_MAX bytes. */
status_t t_readdir(const struct fatrun *r, const char *path, uint32_t index, char *name,
                   bool *is_dir);
status_t t_mkdir(const struct fatrun *r, const char *path);
status_t t_unlink(const struct fatrun *r, const char *path);
status_t t_rename(const struct fatrun *r, const char *from, const char *to);
status_t t_sync(const struct fatrun *r);
status_t t_free(const struct fatrun *r, uint64_t *total, uint64_t *free_bytes);

/* Create (or empty) `path` and write the string into it. */
bool put_file(const struct fatrun *r, const char *path, const char *text);
/* `path` holds exactly the string. */
bool file_is(const struct fatrun *r, const char *path, const char *text);
/* Entries in directory `path`; *found (may be NULL): whether one is `name`
 * exactly, byte for byte. */
bool dir_count(const struct fatrun *r, const char *path, const char *name, unsigned *count,
               bool *found);
