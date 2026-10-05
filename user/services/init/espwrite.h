/* init's stick write, as its two parts share it: the build's
 * (espwrite.c, which also makes the ESP writable and runs the steps) and
 * the boot menu's (espmenu.c). A struct writer is one write under way,
 * on update.c's worker thread; each call below waits at most until
 * esp_until(), so the write's one deadline covers every file call. Paths
 * are the ESP's as fat takes them (its root is "/"), each with a suffix
 * (".new", "" for none). */
#pragma once

#include <fs_idl.h>
#include <update.h>
#include "init.h"

#define ESP_CHUNK     (1u << 20)        /* bytes read, then written, at a time: as much as
                                         * fat holds of an FS_GATHER file (its hold.c) */
#define RECOVER_LIMIT (60 * NS_PER_S)   /* after a failure: put back, leftovers removed */

/* A write under way. */
struct writer {
    struct esp_write *job;
    handle_t fs;                          /* the ESP's writable `fs` channel */
    uint8_t *buf;                         /* ESP_CHUNK bytes */
    uint64_t old_size[UPDATE_FILES];      /* the stick's build: its files' sizes */
    uint8_t  old_sha[UPDATE_FILES][SHA256_BYTES];   /* ... and SHA-256s, as read */
    uint64_t said;                        /* uptime ns of the last progress line */
    uint64_t deadline;                    /* no call may wait past this (uptime ns) */
    bool     already;                     /* the stick has the new build: nothing to write */
    uint8_t  old_at[UPDATE_FILES];        /* AT_*: where the old build's files are */
    uint32_t ops;                         /* changes of names the swap made (swap_op) */
    bool     stopped;                     /* a test's stop came: no clean-up, no sync */
};

/* Where esp_write_file's bytes come from: next(w, ctx, off, buf, n) fills buf. */
typedef status_t (*source_t)(struct writer *, void *, uint64_t, uint8_t *, size_t);

/* A call's deadline (uptime ns): FS_CALL_TIMEOUT from now, never past the
 * write's (w->deadline). */
uint64_t      esp_until(const struct writer *w);
/* A progress line, "init: update: write: ...". */
void          esp_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
unsigned long esp_ms_since(uint64_t t0);
/* The test's injected failure (UPDATE_OFFER_FAIL): ERR_IO once, at that step. */
status_t      esp_inject(struct writer *w, uint32_t step);
/* path + suffix removed (not there: OK). */
status_t      esp_unlink(struct writer *w, const char *path, const char *suffix);
/* path + suffix renamed to + to_suffix (logged). */
status_t      esp_rename(struct writer *w, const char *path, const char *suffix, const char *to,
                         const char *to_suffix);
/* The ESP's fat synced (logged). */
status_t      esp_sync(struct writer *w);
/* path + suffix opened with flags into *f. */
status_t      esp_open(struct writer *w, const char *path, const char *suffix, uint32_t flags,
                       struct jfile *f);
/* Exactly n bytes of f at off into dst (ERR_IO: the file is shorter). */
status_t      esp_read_at(struct writer *w, struct jfile *f, uint64_t off, uint8_t *dst,
                          size_t n);
/* path + suffix, which must be exactly size bytes: its SHA-256 (logged). */
status_t      esp_hash(struct writer *w, const char *path, const char *suffix, uint64_t size,
                       uint8_t digest[SHA256_BYTES]);
/* The bytes `next` gives, size of them, written to path + suffix (created
 * or emptied first, FS_GATHER) and synced (logged); a test's failure at
 * `step` half way (esp_inject). */
status_t      esp_write_file(struct writer *w, const char *path, const char *suffix,
                             uint64_t size, uint32_t step, source_t next, void *ctx);
/* esp_write_file's source: a VMO (ctx: its handle). */
status_t      esp_from_vmo(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n);
/* The size of the file at path + suffix (ERR_NOT_FOUND: none;
 * ERR_WRONG_TYPE: a directory). */
status_t      esp_size_of(struct writer *w, const char *path, const char *suffix,
                          uint64_t *size);
bool          esp_exists(struct writer *w, const char *path, const char *suffix);

/* ---- espmenu.c: the boot menu --------------------------------------------------- */

/* What an earlier menu write that stopped part way left, settled (the
 * stick's menu back in its place if it isn't, the temporary files
 * removed); part of the build's ROOM step, whether or not this write
 * carries a menu. */
status_t esp_menu_settle(struct writer *w);
/* The job's boot menu (w->job->menu_vmo; none: UPDATE_MENU_NONE) checked
 * and written, once the build is: the job's menu, menu_status and
 * menu_why say what became of it. Never changes the job's st. */
void     esp_menu_write(struct writer *w);
