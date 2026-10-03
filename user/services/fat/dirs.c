/* fat: directory listing in linear time. The `fs` protocol has no
 * directory handle: a client lists a directory by asking for entry 0, 1,
 * 2, ... of its path (fs.readdir). Walking from the start for each index
 * makes a listing of n entries cost n * n / 2 entry reads, which the
 * music player's scan and jamjar's library pay for every folder.
 *
 * So fat keeps a few cursors: an open FatFs directory, the path it lists
 * and the index of the entry its next f_readdir returns. A request for the
 * index a cursor is at costs one f_readdir; a later index on the same path
 * skips forward from there; anything else (an earlier index, another
 * path) opens the least recently used cursor again from the start, which
 * is what every request cost before.
 *
 * The answers are the same as a walk from the start would give: every
 * change to what a directory holds (an entry made, removed or renamed:
 * fs.mkdir, fs.unlink, fs.rename, fs.open with FS_CREATE) closes every
 * cursor first, so a cursor never lists a directory that changed since it
 * was opened. Closing them first also gives back FatFs's lock on each
 * directory (FF_FS_LOCK), which would refuse to remove or rename one a
 * cursor holds open; FF_FS_LOCK counts the cursors in. A file's size and
 * date change its entry in place, not the order: writes leave cursors
 * alone.
 *
 * The cursors are fat's own memory, not its state (fat.h): a fat that
 * takes over from another starts with none, and a listing in progress goes
 * on from a fresh walk (fs.readdir takes an index). */
#include "fat.h"

#define READDIR_MAX 65536u   /* a FAT directory holds at most this many entries */

struct dir_cursor {
    bool     open;                /* dir is an open FatFs directory */
    DIR      dir;                 /* positioned before entry `next` */
    uint32_t next;                /* the index the next f_readdir returns */
    uint64_t used;                /* dirs_clock when last used: the LRU order */
    char     path[FS_PATH_MAX];   /* the resolved path it lists */
};

static struct dir_cursor cursors[FAT_DIR_CURSORS];
static uint64_t dirs_clock;       /* bumped on every use */
static uint64_t entries_read;     /* f_readdir calls, ever (fsctl.stats) */

static void forget(struct dir_cursor *c)
{
    if (c->open)
        (void)f_closedir(&c->dir);   /* it only gives FatFs's lock back */
    c->open = false;
}

void dirs_forget(void)
{
    for (unsigned i = 0; i < FAT_DIR_CURSORS; i++)
        forget(&cursors[i]);
}

uint64_t dirs_entries_read(void)
{
    return entries_read;
}

/* The cursor on `path` that can reach `index` going forward, or else the
 * least recently used one, closed. */
static struct dir_cursor *pick(const char *path, uint32_t index)
{
    struct dir_cursor *lru = &cursors[0];
    for (unsigned i = 0; i < FAT_DIR_CURSORS; i++) {
        struct dir_cursor *c = &cursors[i];
        if (c->open && c->next <= index && !strcmp(c->path, path))
            return c;
        if (c->used < lru->used)
            lru = c;
    }
    forget(lru);
    return lru;
}

/* Open c on `path` at entry 0. ERR_WRONG_TYPE: the path is a file. */
static status_t open_at_start(struct dir_cursor *c, const char *path)
{
    FILINFO fi;
    FRESULT fr = f_opendir(&c->dir, path);
    if (fr == FR_NO_PATH && f_stat(path, &fi) == FR_OK)
        return ERR_WRONG_TYPE;
    if (fr != FR_OK)
        return fr_status(fr);
    c->open = true;
    c->next = 0;
    memcpy(c->path, path, strnlen(path, FS_PATH_MAX - 1) + 1);
    return OK;
}

status_t dirs_read(const char *path, uint32_t index, FILINFO *fi)
{
    if (index >= READDIR_MAX)
        return ERR_NOT_FOUND;
    struct dir_cursor *c = pick(path, index);
    c->used = ++dirs_clock;
    status_t st = c->open ? OK : open_at_start(c, path);
    fi->fname[0] = '\0';
    while (st == OK && c->next <= index) {
        FRESULT fr = f_readdir(&c->dir, fi);
        entries_read++;
        if (fr != FR_OK) {
            st = fr_status(fr);
        } else if (fi->fname[0] == '\0') {
            st = ERR_NOT_FOUND;   /* the end: stays there for the next index */
        } else {
            c->next++;
        }
    }
    if (st != OK && st != ERR_NOT_FOUND)
        forget(c);   /* a disk error: start again next time */
    return st;
}
