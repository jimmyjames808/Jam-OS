/* fat internals: one FAT volume served through the `fs` and `file`
 * protocols (abi/idl/fs.idl, file.idl), with ChaN's FatFs doing the FAT
 * work (third_party/fatfs, configured by ffport/ffconf.h).
 *
 * The startup handles and exit codes are in <fatsvc.h>.
 *
 * One thread, one port: the fs channel, every open file's channel and the
 * block channel's peer-closed signal all arrive on it (main.c). FatFs is
 * not re-entrant and needs no locks here: nothing else runs in fat.
 *
 * Files: main.c the startup, mount and event loop; disk.c FatFs's disk
 * callbacks over `block`, the volume's dirty flag and get_fattime;
 * fsops.c the `fs` methods; fileops.c the open-file table and the `file`
 * methods; path.c paths, names, times and the FRESULT -> ERR_* mapping. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <fatsvc.h>
#include <ff.h>
#include <fs_idl.h>
#include <os.h>

#define FAT_MAX_FILES  32          /* open files at once; FF_FS_LOCK is this + 2 */
#define FAT_FILE_BUF   (64u << 10) /* each open file's transfer buffer, bytes */
#define FAT_SECTOR     512u        /* the only sector size (FF_MAX_SS) */
#define FAT_FILE_MAX   0xffffffffull /* FAT's largest file, bytes */
/* The most one write or truncate may grow a file past its end (the gap is
 * filled with zeros, and nothing else is served meanwhile): grow in steps. */
#define FAT_GROW_MAX   (16u << 20)
#define FAT_BATCH      16          /* messages served per wakeup on one channel */

/* Port keys: the fs channel, the block channel, and open file `slot`
 * (FAT_KEY_FILE(slot, gen); gen tells a reused slot's packets apart). */
#define FAT_KEY_FS          1ull
#define FAT_KEY_BLOCK       2ull
#define FAT_KEY_CTL         3ull
#define FAT_KEY_FILE_BIT    (1ull << 62)
#define FAT_KEY_FILE(s, g)  (FAT_KEY_FILE_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define FAT_KEY_SLOT(k)     ((unsigned)((k) & 0xffff))
#define FAT_KEY_GEN(k)      ((uint32_t)((k) >> 16 & 0xffffffffu))

/* The volume: fat serves exactly one. */
struct fat_vol {
    const char *name;         /* for log lines: argv[1] or "fat" */
    handle_t    port;         /* every channel's events */
    handle_t    block;        /* the `block` channel */
    uint8_t    *bbuf;         /* its shared buffer, mapped */
    uint32_t    bbuf_size;    /* ... in bytes (whole pages) */
    uint64_t    blocks;       /* sectors in the partition */
    bool        read_only;    /* block.info said so: every write is refused */
    bool        may_format;   /* started with FAT_ARG_FORMAT: a blank partition is formatted */
    bool        disk_gone;    /* a block call saw ERR_PEER_CLOSED */
    handle_t    rtc_root;     /* SR_RESOURCE or HANDLE_INVALID */
    FATFS       fs;           /* FatFs's volume */

    /* The dirty flag (FAT[1]'s clean-shutdown bit, disk.c). */
    bool        track_dirty;  /* a writable FAT16/FAT32 volume */
    bool        clean_on_disk;/* what the bit on the medium says now */
    uint32_t    fat0[2];      /* first sector of each FAT copy */
    unsigned    nfats;        /* copies (1 or 2) */
};

extern struct fat_vol vol;

/* A file that is open, once however many fs.open calls share it. */
struct fat_open {
    unsigned refs;            /* slots of files[] that use it; 0: free */
    bool     unsynced;        /* written since its last f_sync */
    char     path[FS_PATH_MAX];/* its resolved path: what makes two opens one file */
    FIL      fil;             /* FatFs's file, opened to read (and to write, if its first
                               * open asked) */
};

/* One fs.open: a slot of the table in fileops.c. */
struct fat_file {
    bool     used;            /* the slot holds an open file */
    bool     armed;           /* ch is bound to the port (ONCE) */
    uint32_t gen;             /* bumped on every open of this slot */
    uint32_t flags;           /* FS_* it was opened with */
    handle_t ch;              /* our end of its `file` channel */
    handle_t vmo;             /* its transfer buffer, FAT_FILE_BUF bytes: never mapped here */
    struct fat_open *o;       /* the file */
};

/* ---- disk.c ---------------------------------------------------------------------- */

/* Take the block channel: its info (512-byte sectors only: ERR_NOT_SUPPORTED
 * otherwise) and its buffer, mapped. */
status_t disk_open(handle_t block);
/* Is the partition blank: no boot signature (0x55 0xAA) in its first
 * sector? */
status_t disk_is_blank(bool *out);
/* Around f_mkfs: keep its write of sector 0 (the boot sector) back, then
 * write it last, flushed before and after, with `label` (at most 11
 * characters, as f_setlabel will get) in its label field and in its
 * backup copy's. Until the commit the partition is still blank.
 * ERR_BAD_STATE: nothing wrote sector 0. */
void     disk_hold_boot(void);
status_t disk_commit_boot(const char *label);
/* The format failed: forget the held sector, write nothing. */
void     disk_drop_boot(void);
/* After a mount: find the FATs, log a volume found dirty, and (writable
 * FAT16/32) start keeping the dirty flag. */
void     disk_watch(void);
/* FatFs has written everything out (the caller synced the files): flush the
 * medium if it needs it and mark the volume clean. durable: flush the mark
 * too (fs.sync, fat's end); without it the mark goes out with the next
 * flush, and a power cut before that finds a volume that is whole and
 * called dirty. */
status_t disk_settle(bool durable);

/* ---- path.c ---------------------------------------------------------------------- */

/* The IDL's 256-byte path field -> "/a/b" ("/" for the root): at most 255
 * bytes and NUL-terminated, "" or starting with '/'; "." and ".." resolved
 * (".." at the root stays there); every name one FAT and Windows accept
 * as a long name (no control characters, none of " * : < > ? \ |, not
 * ending in '.' or ' '). ERR_INVALID_ARGS otherwise. */
status_t path_resolve(const uint8_t in[FS_PATH_MAX], char out[FS_PATH_MAX]);
bool     path_is_root(const char *p);
/* Is `p` below directory `dir` (compared without case, as FAT does)? */
bool     path_inside(const char *p, const char *dir);
/* Are two resolved paths the same, compared without case? */
bool     path_same(const char *a, const char *b);
/* The generic FRESULT -> ERR_* mapping (the methods refine FR_DENIED). */
status_t fr_status(FRESULT r);
/* A FAT date and time (the clock's own zone) as seconds since 1970 counted
 * as if it were UTC; 0 for no date. */
uint64_t fat_unix_time(WORD date, WORD time);

/* ---- fsops.c --------------------------------------------------------------------- */

extern const struct fs_ops fat_fs_ops;

/* ---- fileops.c ------------------------------------------------------------------- */

/* Open `path` (resolved) with FS_* flags: a new slot, its channel (bound to
 * the port) and buffer. *out_ch and *out_vmo are the client's ends. */
status_t files_open(const char *path, uint32_t flags, handle_t *out_ch, handle_t *out_vmo,
                    uint64_t *out_size);
/* The port said slot's channel has news (key's gen): serve it. */
void     files_event(uint64_t key);
/* Finish every file whose client has closed it (called before each fs
 * request, so a close followed by an fs call is seen in that order). */
void     files_reap(void);
/* f_sync every file written since its last sync. */
status_t files_sync_all(void);
/* Any file with unsynced writes? */
bool     files_unsynced(void);
/* Close every open file, flushing what it wrote. */
void     files_close_all(void);
