/* logd: this boot's log file, /data/logs/boot-NNNN.txt (NNNN from 0001),
 * and the copy of a panicked boot's log the boot after it saves,
 * boot-NNNN-crash.txt.
 *
 * A number is taken when either file has it: the log of a panicked boot
 * that had no file of its own (no /data then) gets the next free number,
 * and this boot's own log the one after it.
 *
 * The number is the first free one after the files already there, found
 * with a handful of stat calls instead of a walk through the directory
 * (which on FAT costs a read of the whole directory per entry asked for):
 * double the number until a file is missing, then halve the gap between
 * the last one found and the first one missing. With the files numbered
 * without holes, as logd leaves them, that is the highest number plus one.
 * Where files in the middle were deleted it may be a hole's number instead.
 * With all 9999 taken there is no file: ERR_NO_SPACE.
 *
 * The number is chosen once per boot. When /data comes back after it went
 * away the same file is opened again and appended to; only if it is gone
 * (another stick, a reformatted one) is a new number taken. */
#include <os.h>
#include "logd.h"

#define LOG_DIR  "/logs"
#define LOG_LAST 9999u

static const struct store *store;    /* where the file is */
static char     path[64];            /* the file, once a number is chosen */
static bool     is_open;
static uint64_t offset;              /* where the next write goes: its end */

static char base[16];   /* the file's name without ".txt": "boot-0042" */

static void name_of(char *out, size_t size, unsigned number, const char *kind)
{
    snprintf(out, size, "%s" LOG_DIR "/boot-%04u%s.txt", store->root, number, kind);
}

/* Does file `number` (or its crash log) exist? Anything but a clear yes
 * or no is *st. */
static bool taken(unsigned number, status_t *st)
{
    static const char *const kinds[] = { "", "-crash" };
    for (unsigned k = 0; k < 2; k++) {
        char name[64];
        name_of(name, sizeof(name), number, kinds[k]);
        status_t s = store->stat(name);
        if (s != OK && s != ERR_NOT_FOUND)
            *st = s;
        if (s != ERR_NOT_FOUND)
            return s == OK;
    }
    return false;
}

/* The first free number (see the top of the file). */
static status_t next_number(unsigned *out)
{
    status_t st = OK;
    unsigned have = 0, free_at = 1;   /* `have` is taken (or 0), free_at is not (or unknown) */
    while (st == OK && free_at <= LOG_LAST && taken(free_at, &st)) {
        have = free_at;
        free_at *= 2;
    }
    if (free_at > LOG_LAST)
        free_at = LOG_LAST + 1;       /* past the last: free by definition */
    while (st == OK && free_at - have > 1) {
        unsigned mid = have + (free_at - have) / 2;
        if (taken(mid, &st))
            have = mid;
        else
            free_at = mid;
    }
    if (st != OK)
        return st;
    if (free_at > LOG_LAST)
        return ERR_NO_SPACE;
    *out = free_at;
    return OK;
}

/* /data/logs, made if it isn't there. */
static status_t make_dir(void)
{
    char dir[32];
    snprintf(dir, sizeof(dir), "%s" LOG_DIR, store->root);
    status_t st = store->mkdir(dir);
    return st == ERR_ALREADY_EXISTS ? OK : st;
}

status_t logfile_open(const struct store *s)
{
    uint64_t size = 0;
    store = s;
    status_t st = make_dir();
    if (st != OK)
        return st;
    st = ERR_NOT_FOUND;
    if (path[0])   /* the file of this boot, if it is still there */
        st = store->open(path, FS_WRITE | FS_APPEND, &size);
    if (st == ERR_NOT_FOUND) {
        unsigned number;
        st = next_number(&number);
        if (st != OK)
            return st;
        name_of(path, sizeof(path), number, "");
        snprintf(base, sizeof(base), "boot-%04u", number);
        st = store->open(path, FS_WRITE | FS_CREATE | FS_APPEND, &size);
    }
    if (st != OK)
        return st;
    is_open = true;
    offset = size;
    return OK;
}

status_t logfile_write(const void *data, uint32_t n)
{
    status_t st = store->write(offset, data, n);
    if (st == OK)
        offset += n;
    return st;
}

status_t logfile_sync(void)
{
    return store->sync();
}

void logfile_close(void)
{
    if (is_open)
        store->close();
    is_open = false;
}

const char *logfile_path(void)
{
    return path;
}

const char *logfile_name(void)
{
    return base;
}

status_t logfile_crash_path(const struct store *s, const char *name, char *out, size_t size)
{
    store = s;
    status_t st = make_dir();
    if (st != OK)
        return st;
    if (name[0]) {
        snprintf(out, size, "%s" LOG_DIR "/%s-crash.txt", store->root, name);
        return OK;
    }
    unsigned number;
    st = next_number(&number);
    if (st == OK)
        name_of(out, size, number, "-crash");
    return st;
}
