/* logd's own pieces: main.c (startup, following the log, when to sync and
 * when to try /data again), logfile.c (the boot log file: its name, opening
 * it, writing), store_ns.c and store_fs.c (the two ways to reach /data).
 *
 * A store is where the file lives. logd is written against <os.h>'s file
 * calls on the namespace it is started with (store_ns: what init gives
 * it). store_fs talks to one `fs` channel directly instead, for a starter
 * that hands logd /data's channel and no namespace: utest does, to test
 * logd against a fat service of its own. */
#pragma once

#include <os.h>

/* Startup handles that replace the defaults (utest/logd.c has the same
 * numbers):
 *   LOGD_SR_FS   an `fs` channel to use as /data (default: the namespace);
 *   LOGD_SR_LOG  a channel whose messages are the log's text (default: the
 *                kernel log, read with SR_RESOURCE). When its other end
 *                closes the log has ended: logd syncs and exits 0. */
#define LOGD_SR_FS  (SR_USER + 0)
#define LOGD_SR_LOG (SR_USER + 1)

/* One way to reach /data. It holds at most one open file: the log. Errors
 * are the filesystem's (fs.idl), ERR_NOT_FOUND also for "no /data". */
struct store {
    const char *root;   /* what /data is called through it: "/data", or "" */
    status_t (*mkdir)(const char *path);
    /* OK: the path exists. */
    status_t (*stat)(const char *path);
    /* flags: FS_*; *size: the file's size now. */
    status_t (*open)(const char *path, uint32_t flags, uint64_t *size);
    status_t (*write)(uint64_t offset, const void *data, uint32_t n);
    status_t (*sync)(void);
    void     (*close)(void);
};

extern const struct store store_ns;
/* The store on `fs`, a client end of an `fs` channel. */
const struct store *store_fs(handle_t fs);

/* logfile.c: this boot's log file, /data/logs/boot-NNNN.txt. */
/* Open it through s: the first time that makes /data/logs if needed and
 * takes the next free number; later (after /data went away) the same file
 * again, or a new number if it is gone. */
status_t logfile_open(const struct store *s);
/* Append n bytes. */
status_t logfile_write(const void *data, uint32_t n);
status_t logfile_sync(void);
void     logfile_close(void);
/* Its path, for messages ("" before the first open). */
const char *logfile_path(void);
