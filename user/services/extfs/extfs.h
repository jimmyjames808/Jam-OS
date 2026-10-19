/* extfs internals: one ext volume served through the `fs` and `file`
 * protocols (abi/idl/fs.idl, file.idl), on the ext library beside it
 * (ext/: the format, allocation, names and links, the journal;
 * docs/M13.6-PLAN.md, "The design").
 *
 * The startup handles and exit codes are in <extfssvc.h>.
 *
 * One thread, one port, as fat: the fs channel and its views, every open
 * file's channel and the block channel's peer-closed signal all arrive on
 * it (main.c). One request at a time.
 *
 * The disk is the state (the plan's "The disk is the state"): every
 * request that changes the volume is one journal transaction, committed on
 * the disk before its answer, so the disk is always whole and a
 * successor's start is the recovery a power cut needs (the journal's
 * replay, then the orphan list). What the state VMO keeps is small: the
 * request slots and each one's transaction id, the open files (inode
 * number and generation, flags), the views (flags, the root's inode), the
 * mapped files (inode, kind, key), the watches (watch.c's table: the
 * watcher channels and their views, the watched inodes), the record locks
 * (locks.c's table and lock owners), the directory channels (the
 * directory's inode and generation, the view's flags and root, a removed
 * one's parent). The block
 * cache, the directory hash, where each directory lies (walk.c's places),
 * the watchers' queued events and the locks' waits are not kept (a
 * successor's queues start with IN_Q_OVERFLOW; a waiter asks again; a
 * place is found again by climbing).
 *
 * Files: main.c the startup, the mount (and the format), the event loop;
 * disk.c the ext library's device over `block`; state.c the state VMO;
 * request.c each request read into a slot and run as one transaction
 * (begin, run, commit in the journal, commit in the slot, answer), and
 * the work left after an answer (orphans dropped, truncations finished);
 * adopt.c a successor's start; keeper.c the keep channel; walk.c paths:
 * names, links followed inside the view, `..`, the permission bits, what
 * a view hides judged where a path resolves; fsops.c the `fs` methods;
 * files.c the open files and the `file` methods; views.c views and
 * subviews; pager.c mapped files and the mount's pager; wback.c their
 * stores written back; watch.c file watching (fs.watch's channels and
 * the events, docs/INOTIFY-PLAN.md); locks.c record locks (file.lock,
 * <rlock.h>); dirs.c directory channels (fs.opendir,
 * docs/DIRHANDLE-PLAN.md); test.c the test powers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <fswatch.h>
#include <idl/fsctl.h>
#include <os.h>
#include <rlock.h>
#include <svcstate.h>
#include "ext/ext.h"
#include "ext/ext_alloc.h"
#include "ext/ext_jnl.h"
#include "ext/ext_mkfs.h"
#include "ext/ext_names.h"

/* Open files, views and mapped files at once, the whole volume's: on a
 * drive boot one extfs serves /data, /jam and init's /tmp, /usr, /var,
 * /home and /pkg views of it, and every program's views of those, so these
 * are the system's (docs/M13.6-REVIEW.md's R3; 32 open files was too few
 * for 64 threads' temporary files). Each costs handles here and with the
 * keeper (keeper.c's asserts), and a file 64 KiB of buffer while open:
 * volumes' ext_limits give the job room for all of them. */
#define XF_MAX_FILES   256         /* open files at once */
#define XF_VIEWS       128         /* views and subviews served at once */
#define XF_DIRS        256u        /* directory channels at once (fs.opendir: dirs.c) ... */
#define XF_DIRS_PROC   64u         /* ... from one process (the opendir request's writer) */
#define XF_FILE_BUF    (64u << 10) /* each open file's transfer buffer, bytes */
#define XF_LINKS_MAX   40u         /* links one path may follow (POSIX's SYMLOOP_MAX) */
#define XF_MAGIC       0xef53u     /* statfs's magic: Linux's EXT4_SUPER_MAGIC */
#define XF_NAME_MAX    255u        /* statfs's longest name */
#define XF_FEATURES    (FS_FEAT_LINKS | FS_FEAT_MODES | FS_FEAT_CASE | FS_FEAT_BIG | \
                        FS_FEAT_MULTI_WRITER | FS_FEAT_DIRS)
#define XF_DEFER_MAX   64u         /* orphans and truncations left for after an answer */
#define XF_MAX_MAPS    256u        /* mapped files at once (file.map's VMOs: pager.c) */
#define XF_LOCK_OWNERS 64u         /* lockowner's owners at once (locks.c: OFD and flock) */

/* The key of a VMO mapped to run: a sequence number and its slot of
 * maps[] (pager.c). The sequence stays below 2^55 (nanoseconds since
 * boot, over a year), so the top bit is clear. */
#define XF_MAP_KEY(seq, i) ((uint64_t)(seq) << 8 | (uint64_t)(i))
#define XF_MAP_SLOT(key)   ((unsigned)((key) & 0xff))
_Static_assert(XF_MAX_MAPS <= 256, "a slot fits a key's low byte");
/* The key of a file's shared VMO: the file's, its inode's number and
 * generation under the top bit, so the kernel's keys are the index of
 * the live ones (pager.c). An inode number past XF_SHARED_INO_MAX has
 * none (a volume of 2^31 inodes). */
#define XF_SHARED_BIT          (1ull << 63)
#define XF_SHARED_INO_MAX      0x7fffffffu
#define XF_SHARED_KEY(ino, ig) (XF_SHARED_BIT | (uint64_t)(ino) << 32 | (uint64_t)(ig))

/* Port keys and channel ids: fat's (fat.h), the same scheme. */
#define XF_KEY_FS          1ull
#define XF_KEY_BLOCK       2ull
#define XF_KEY_CTL         3ull
#define XF_KEY_OWN         4ull
#define XF_KEY_PAGER       5ull
#define XF_KEY_FILE_BIT    (1ull << 62)
#define XF_KEY_FILE(s, g)  (XF_KEY_FILE_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define XF_KEY_VIEW_BIT    (1ull << 61)
#define XF_KEY_VIEW(s, g)  (XF_KEY_VIEW_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define XF_KEY_WATCH_BIT   (1ull << 60)   /* a watcher channel: its number is the library's */
#define XF_KEY_WATCH(s, g) (XF_KEY_WATCH_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define XF_KEY_LOCK_BIT    (1ull << 59)   /* a lock owner's token (locks.c): its slot */
#define XF_KEY_LOCK(i)     (XF_KEY_LOCK_BIT | (uint64_t)(i))
#define XF_KEY_DIR_BIT     (1ull << 58)   /* a directory channel (dirs.c) */
#define XF_KEY_DIR(s, g)   (XF_KEY_DIR_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define XF_KEY_SLOT(k)     ((unsigned)((k) & 0xffff))
#define XF_KEY_GEN(k)      ((uint32_t)((k) >> 16 & 0xffffffffu))

/* A channel's id in a request slot: its kind in the top two bits (0: the
 * mount's own channels, numbered, and the directory channels, bit 24),
 * its generation's low 16 bits and its slot. */
#define XF_CHAN_FS          1u
#define XF_CHAN_CTL         2u
#define XF_CHAN_OWN         3u
#define XF_CHAN_FILE(s, g)  (1u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
#define XF_CHAN_VIEW(s, g)  (2u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
#define XF_CHAN_WATCH(s, g) (3u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
#define XF_CHAN_DIR_BIT     (1u << 24)
#define XF_CHAN_DIR(s, g)   (XF_CHAN_DIR_BIT | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
_Static_assert(XF_MAX_FILES <= 256 && XF_VIEWS <= 256 && FSW_CHANNELS <= 256 && XF_DIRS <= 256,
               "a slot fits a channel id's low byte");

/* The steps of a start, timed for a restart's log line (adopt.c). */
enum xf_at { XF_AT_MAIN, XF_AT_DISK, XF_AT_STATE, XF_AT_MOUNT, XF_AT_HANDLES, XF_AT_COUNT };

/* The volume and this instance's handles (its own memory: rebuilt by a
 * successor). */
struct xf_vol {
    const char *name;         /* for log lines: argv[1] or "extfs" */
    handle_t    port;
    handle_t    block;        /* the `block` channel */
    uint8_t    *bbuf;         /* its shared buffer, mapped */
    uint32_t    bbuf_size;    /* ... bytes */
    uint32_t    sector;       /* the channel's block size, bytes */
    uint64_t    sectors;      /* ... and its blocks */
    uint32_t    max_sectors;  /* the most one block call moves */
    bool        read_only;    /* block.info said so */
    bool        in_order;     /* ... and a queue depth of 1: answers in order (disk.c's retries) */
    bool        ram;          /* the disk is a VMO (EXTFS_SR_RAM): no block channel */
    bool        may_format;   /* EXTFS_ARG_FORMAT */
    bool        fua;          /* EXTFS_ARG_FUA: an answered write is on the medium */
    bool        unflushed;    /* written since the last block.sync (no fua) */
    bool        disk_gone;    /* a block call saw ERR_PEER_CLOSED */
    handle_t    keep, serve, own, ctl, pager;
    struct idl_held held;     /* channels held back by the unread bound (fat's rule) */
    const char *ended;        /* EXTFS_ARG_KILLED, EXTFS_ARG_CRASHED, or NULL */
    uint64_t    at[XF_AT_COUNT];
    /* The ext library's volume: the cache, the journal, the directory hash. */
    struct ext_fs  fs;
    struct ext_jnl jnl;
    bool        mounted;
    bool        marked;       /* the error state was written (once) */
};

extern struct xf_vol vol;

/* ---- the state (state.c) --------------------------------------------------------- */

#define XF_STATE_KIND   0x73667865u   /* "exfs": svcstate's kind */
#define XF_STATE_LAYOUT 12u           /* bump on any change to struct xf_state or the
                                       * requests its slots replay */
/* A slot's request area: the request (fs.rename's two paths), then from
 * XF_SLOT_DATA its data: the bounce buffer a write's bytes are copied
 * into before ext sees them, so a re-run writes the same bytes. */
#define XF_SLOT_DATA    (12u << 10)

/* One fs.open (its channel and buffer are files.c's, by the same index). */
struct xf_file {
    bool     used;
    uint32_t gen;             /* bumped on every open of the slot: its port key */
    uint32_t flags;           /* FS_* it was opened with */
    uint32_t ino;             /* the file: its inode ... */
    uint32_t igen;            /* ... and that inode's generation */
    bool     nochange;        /* its view may not change it (file.setattr refused) */
    uint32_t wb_seq;          /* the file's last write-back error it heard of (an xf_map's
                               * error_seq: wback_sample at the open, wback_file) */
};

/* One view (fs.view, fs.subview; its channel is views.c's, and where its
 * root lies, found by climbing). */
struct xf_view {
    bool     used;
    uint32_t flags;           /* FS_VIEW_* */
    uint32_t gen;
    uint32_t root_ino;        /* its root directory (EXT_ROOT_INO: the volume's) ... */
    uint32_t root_igen;       /* ... and its generation */
};

/* One directory channel (fs.opendir; its channel is dirs.c's). Its view
 * is a copy of the one it was opened through (that channel may close
 * first). A directory removed while a channel holds it stays, with no
 * name, on the orphan list until its last channel closes; its ".." is the
 * parent it had then (docs/DIRHANDLE-PLAN.md 3.3). */
struct xf_dir {
    bool     used;
    uint32_t gen;             /* bumped on every opendir of the slot: its port key */
    uint32_t ino;             /* the directory: its inode ... */
    uint32_t igen;            /* ... and that inode's generation */
    uint32_t flags;           /* its view's FS_VIEW_* ... */
    uint32_t root_ino;        /* ... root ... */
    uint32_t root_igen;       /* ... and the root's generation, */
    int32_t  view;            /* checked unless this is -1 (the mount's own channel), else 0 */
    uint32_t pid;             /* the process that opened it (XF_DIRS_PROC) */
    uint32_t up_ino;          /* removed: the directory it was removed from (set before the
                               * removal commits; read only once its link count is 0) ... */
    uint32_t up_igen;         /* ... and that one's generation */
};

/* One VMO file.map made (pager.c): the file it shows, and how. */
struct xf_map {
    bool     used;
    uint8_t  kind;            /* FILE_MAP_READ (the file's shared VMO) or FILE_MAP_EXEC */
    bool     written;         /* shared: handed out writable since it was made (until its
                               * GONE: the file isn't mapped to run, Q7) */
    bool     made_write;      /* shared: the last map that answered with it was FILE_MAP_WRITE
                               * (adopt.c makes that answer again) */
    uint32_t ino;             /* the file: its inode ... */
    uint32_t igen;            /* ... and that inode's generation */
    status_t error;           /* shared: the first write-back error (ERR_IO, ERR_NO_SPACE)
                               * since an open's file.sync last heard of one, or OK ... */
    uint32_t error_seq;       /* ... its number (xf_state's err_seq; 0: none), which each
                               * open's file.sync reports once (wback.c: Linux's errseq) */
    bool     error_seen;      /* ... and an open has heard of it */
    uint64_t key;             /* its VMO's key: XF_SHARED_KEY, or XF_MAP_KEY(seq, its slot) */
};

/* What a slot's request did to the journal: set before its commit
 * (on_commit), so a successor that finds the slot not committed knows
 * whether the journal has it (then its reply, already in the slot, is
 * the answer) or not (run it again). */
struct xf_slot {
    uint64_t seq;             /* the request (the slot's seq) the rest is about; 0: none */
    uint32_t rn;              /* its reply's length */
    uint32_t tid;             /* its transaction ... */
    uint32_t has_tid;         /* ... set (released) just before the commit block goes */
};

/* The slot of files[], views[] or maps[] the request numbered `seq` made,
 * which its reply hands out (adopt.c makes the client's end again if the
 * reply never went out, and drops it if the request runs again). A map to
 * run is XF_MADE_MAP; one of a file's shared VMO (XF_MADE_MAP_SHARED) is
 * remade but never dropped: other mappers may have it, the request run
 * again finds it by the file's key, and its slot goes at its GONE. */
#define XF_MADE_FILE       1u
#define XF_MADE_VIEW       2u
#define XF_MADE_MAP        3u
#define XF_MADE_MAP_SHARED 4u
#define XF_MADE_WATCH      5u   /* fs.watch's watcher channel (the library's number) */
#define XF_MADE_DIR        6u   /* fs.opendir's directory channel */
struct xf_made {
    uint64_t seq;
    uint32_t kind;
    uint32_t index;
};

/* The last file.sync whose open heard of a write-back error (wback.c),
 * recorded before the mark: a successor that runs that request again (its
 * slot not committed, so its answer never went out) puts the mark back
 * first (adopt.c), and the run again reports the error as the first run
 * did. */
struct xf_heard {
    uint64_t seq;             /* the request (the slot's seq); 0: none */
    uint32_t file;            /* files[] whose wb_seq it moved ... */
    uint32_t was;             /* ... from this ... */
    uint32_t heard;           /* ... to this (maps[map]'s error_seq) */
    uint32_t map;             /* maps[] whose error_seen it set ... */
    bool     seen;            /* ... from this */
};

/* A watcher channel's view (watch.c), beside the library's record of it
 * (struct fsw_chan_rec: the view's flags and root inode): the rest of the
 * struct xf_ctx its adds are walked in. */
struct xf_wview {
    uint32_t gen;             /* bumped at each fs.watch of the slot: its port key */
    int32_t  view;            /* -1: the mount's own channel (the root's generation not
                               * checked), else 0 */
};

/* A rename's reach check (watch.c's `moved`), held to its request's
 * commit: inode x (a directory if xdir) moved from directory `from` into
 * `to` (inode numbers and generations). */
struct xf_wmove {
    uint32_t x, x_gen, from, from_gen, to, to_gen;
    uint32_t xdir;
};

/* File watching's part (watch.c, <fswatch.h>): the library's table (the
 * watcher channels' records and the watches), each channel's view, each
 * watched file's directory (inode and generation; 0 for a directory's
 * watch), where the reach check (docs/INOTIFY-PLAN.md 2.6) places it, and
 * the running request's reach checks not yet run: a successor that finds
 * the request in the journal runs them (watch_moves_again, IR's 1). */
struct xf_watches {
    struct fsw_table table;
    struct xf_wview  views[FSW_CHANNELS];
    uint32_t         dir[FSW_WATCHES][2];
    uint32_t         nmoves;
    struct xf_wmove  moves[2];    /* a rename's one, or an exchange's two */
};

/* Record locks' part (locks.c): the table (<rlock.h>: changed by one
 * store) and each lockowner's number (0: the slot is free). */
struct xf_locks {
    struct rl_table table;
    uint64_t        owners[XF_LOCK_OWNERS];
};

struct xf_state {
    uint8_t         uuid[16];      /* the volume's: a state is for one volume */
    bool            mounted;       /* a volume was served from it */
    struct xf_file  files[XF_MAX_FILES];
    struct xf_view  views[XF_VIEWS];
    struct xf_slot  slot[2];
    struct xf_made  made;
    uint64_t        data_seq[2];   /* slot i's data holds request data_seq[i]'s bytes */
    struct xf_map   maps[XF_MAX_MAPS];
    uint64_t        map_seq;       /* the last key's sequence number (pager.c) */
    uint32_t        err_seq;       /* the last write-back error's number (wback.c) */
    struct xf_heard heard;         /* ... and the last file.sync that reported one */
    struct xf_locks locks;         /* locks.c's */
    struct xf_dir   dirs[XF_DIRS]; /* dirs.c's */
    struct xf_watches watch;       /* watch.c's */
};

extern struct xf_state *kept;

/* Map the state VMO (SR_STATE, or one made now); *adopted: a dead
 * instance's that svcstate accepted. Set up empty otherwise. */
status_t state_open(bool *adopted);
/* Set it up empty (keep_views: but for the views' table). */
void     state_reset(bool keep_views);
struct svcstate *state_slots(void);

/* ---- disk.c ------------------------------------------------------------------- */

/* Take the block channel: its info, its buffer mapped; vol.fs's device. */
status_t disk_open(handle_t block, struct ext_dev *dev);
/* ... or a VMO as the disk (EXTFS_SR_RAM): mapped whole; vol.ram. */
status_t disk_open_ram(handle_t vmo, struct ext_dev *dev);
/* The partition's first 4 KiB are all zeros (ext's mkfs writes them last,
 * the superblock with them, so a format cut short is blank still). */
status_t disk_is_blank(bool *out);
/* Device writes so far (test.c's write point counts them). */
uint64_t disk_writes(void);
/* Every write so far on the medium: a block.sync if the disk has a cache
 * (no EXTFS_ARG_FUA) and anything was written since the last one; OK at
 * once otherwise. The journal calls it through the device's flush. */
status_t disk_flush(void);

/* ---- request.c ---------------------------------------------------------------- */

enum xf_proto { XF_PROTO_FS, XF_PROTO_FILE, XF_PROTO_CTL, XF_PROTO_WATCH };

/* Where a directory lies (walk.c's walk_place): its region, found by
 * climbing its ".." entries to the volume's root, and kept until a rename
 * moves a directory (the epoch, docs/DIRHANDLE-PLAN.md 3.2). Memory only:
 * a successor climbs again. */
struct xf_place {
    uint64_t         epoch;   /* walk.c's epoch when found (0: never) */
    status_t         st;      /* OK, or why not (ERR_NOT_FOUND: gone, or out of reach) */
    struct fs_region region;
};

/* The context of an fs request: the channel's view (the mount's own
 * channels: the whole volume, no flags), and a directory channel's
 * directory, where its relative paths start. */
struct xf_ctx {
    uint32_t         flags;   /* FS_VIEW_*: judged where paths resolve (walk.c) */
    uint32_t         root_ino, root_igen;
    struct xf_place *root_at; /* where the root lies (NULL: found at each walk) */
    int              view;    /* its slot of views[] (or 0: a copy's, the root's generation
                               * checked), or -1 */
    uint32_t         dir_ino; /* a directory channel's directory (0: none) ... */
    uint32_t         dir_igen;
    struct xf_place *dir_at;  /* ... and where it lies (NULL: found at each walk) */
};

struct xf_chan {
    handle_t        ch;
    uint32_t        id;       /* XF_CHAN_* */
    enum xf_proto   proto;
    struct xf_ctx   ctx;      /* XF_PROTO_FS (a view's, a directory channel's) */
    struct xf_file *file;     /* XF_PROTO_FILE */
    void           *watch;    /* XF_PROTO_WATCH: watch.c's record of the channel */
};

status_t serve_one(const struct xf_chan *c);
void     serve_slot(const struct xf_chan *c, unsigned slot);
void     answer(const struct xf_chan *c, unsigned slot, uint32_t rn, handle_t *rhs,
                uint32_t rhn);
void     answer_status(const struct xf_chan *c, unsigned slot, status_t status);
void     reply_flush(void);
status_t xf_wait(uint64_t deadline, struct port_packet *pkt);
/* The running request's bounce buffer (XF_FILE_BUF bytes, in its slot), and
 * whether its bytes are in it already (a re-run). */
uint8_t *op_bounce(void);
bool     op_data_in(void);
void     op_data_copied(void);
/* The running request made slot `index` (XF_MADE_*). */
void     op_made(uint32_t kind, uint32_t index);
/* The process that wrote the running request (fs and file requests: the
 * kernel's CHANNEL_READ_PID, kept in its slot; 0 for the others). */
uint32_t op_pid(void);
/* A request runs (its transaction open): work after an answer waits for it. */
bool     op_running(void);
/* The running request's number (its slot's seq), or 0. */
uint64_t op_seq(void);

/* The volume may be changed: mounted writable, its journal open. */
bool     writable(void);
/* A transaction outside a request (a close's orphan, the recovery, the
 * work after an answer): begin, then commit (or abort on an error). */
status_t tx_begin(void);
status_t tx_commit(void);
void     tx_abort(void);
/* A metadata error (ERR_IO with the volume damaged): the volume goes
 * read-only, its superblock marked as having errors, the log told. */
void     went_bad(const char *what);

/* After the answer: drop orphan `ino` (unless it is held: files_held), or finish
 * shortening file `ino` (on the orphan list since the request; `own`:
 * its entry on the list is the truncation's, taken off at the end). */
void     defer_drop(uint32_t ino);
void     defer_trunc(uint32_t ino, bool own);
/* Do what was deferred (main.c's loop, after each answer). */
void     defer_run(void);
/* The journal's hook (on_commit), after each ext_jnl_init. */
void     request_init(void);

/* The time now (the clock's; test.c's fixed-time power: 2026-01-01). */
void     time_now(struct ext_time *t);
/* A new inode's generation. */
uint32_t new_generation(void);

/* ---- walk.c ------------------------------------------------------------------ */

#define W_NOFOLLOW 0x1        /* a link as the last name is the answer, not followed */
#define W_STAT     0x2        /* fs.stat: `etc` itself shows unless all of etc is hidden */
#define W_GONE     0x4        /* a removed directory may be the answer (getattr, stat, sync,
                               * statfs, opendir: docs/DIRHANDLE-PLAN.md 3.3) */

struct xf_walk {
    bool     found;           /* the path names something: `in` */
    bool     gone;            /* ... a removed directory a channel holds (W_GONE): it has no
                               * place, so no view hides it */
    bool     has_name;        /* it ends in a name (not the view's root, ".", ".."): `dir`
                               * holds it, and `name` is it */
    bool     must_dir;        /* it ended in '/' */
    struct ext_inode in;
    struct ext_inode dir;
    struct ext_name name;
    char     namebuf[EXT_NAME_MAX + 1];
    struct fs_region region;  /* where it lies, for the view's checks (<fsview.h>) */
    /* The names walked below `base` (the view's root, or the directory
     * channel's directory, or one its ".." climbed to), as text:
     * fs.resolve's answer only (walk_inside). */
    uint32_t base;
    char     rel[FS_PATH_LEN + 1];
    uint32_t rel_len;
    bool     rel_long;        /* ... they passed FS_PATH_LEN bytes: no answer */
    /* ERR_LINK_OUT: where it went on (fs.resolve's answers) */
    uint8_t  kind;
    uint32_t used;
    char     next[FS_PATH_LEN + 1];
    uint32_t next_len;
};

/* Walk path (len bytes, as the protocol carried it) in view c, from the
 * view's root if it starts with '/' or c is no directory channel, else
 * from c's directory: every name, ".." physically (from a directory
 * channel's directory, its own ".." entry, up to the view's root), links
 * inside the view followed (up to XF_LINKS_MAX: ERR_LINK_LOOP past it),
 * the search bit of every directory looked into checked. The last name
 * need not be there (found false). A directory channel's directory removed
 * (held: dirs.c): "" and "." name it (w->gone; with W_GONE only), ".." the
 * parent it had (dirs_up), where the rest is walked from; every other
 * name, and a '/' path, ERR_NOT_FOUND. Errors: ERR_LINK_OUT (an absolute
 * link, or ".." above the view's root: w->kind, used, next);
 * ERR_NOT_FOUND, ERR_NOT_DIR on the way (ERR_NOT_FOUND too for a
 * directory channel whose directory is gone, or lies where its view
 * doesn't reach or hides: docs/DIRHANDLE-PLAN.md's Q2); ERR_ACCESS_DENIED
 * (a search bit, or a directory the view hides wholly); ERR_NAME_TOO_LONG;
 * ERR_INVALID_ARGS (a name ext can't hold); ERR_IO. */
status_t walk(const struct xf_ctx *c, const char *path, uint32_t len, uint32_t flags,
              struct xf_walk *w);
/* fs.resolve's answer for an INSIDE path: the place relative to the
 * view's root (a directory channel's own place found by climbing and
 * naming each directory). ERR_NAME_TOO_LONG past FS_PATH_LEN bytes. */
status_t walk_inside(const struct xf_ctx *c, const struct xf_walk *w, char *out,
                     uint32_t *out_len);
/* Where directory ino lies (generation gen; 0: not checked), into *p: as
 * found before, unless a directory moved since (walk_moved), else found
 * now by climbing ".." entries to the volume's root. stop (0: none): the
 * view's root (generation stop_gen, 0: not checked), which the climb must
 * pass (else ERR_NOT_FOUND: it lies outside the view). Also ERR_NOT_FOUND
 * for a directory gone, its inode used again or not a directory; ERR_IO.
 * Returns p->st. */
status_t walk_place(struct xf_place *p, uint32_t ino, uint32_t gen, uint32_t stop,
                    uint32_t stop_gen);
/* Where w's answer lies, as found now (a new view's root, a new directory
 * channel's directory). */
struct xf_place walk_place_of(const struct xf_walk *w);
/* A rename moved a directory to another parent, or renamed one in the
 * volume's top two levels: every place found before is found again. */
void     walk_moved(void);
/* Does the view hide what w names (reads and changes alike)? stat: fs.stat. */
bool     walk_hidden(const struct xf_ctx *c, const struct xf_walk *w, bool stat);
/* May the view change what w names (FS_VIEW_GUARD_ETC and FS_VIEW_GUARD_PKG
 * where it resolves)? */
bool     walk_may_change(const struct xf_ctx *c, const struct xf_walk *w);
/* The permission bits for the one user: want's bits (4 read, 2 write, 1
 * search) all in the owner's (uid FS_ONE_USER) or the others' bits. */
bool     may(const struct ext_inode *in, unsigned want);
/* An inode's attributes, as fs.getattr gives them. */
struct xf_attr {
    uint32_t mode, nlink, uid, gid;
    uint64_t size, blocks, id;
    int64_t  atime_ns, mtime_ns, ctime_ns;
};
void     attr_of(const struct ext_inode *in, struct xf_attr *a);
/* fs.setattr and file.setattr on in (written back with its change time). */
status_t attr_set(struct ext_inode *in, uint32_t mask, uint32_t mode, uint32_t uid,
                  uint32_t gid, int64_t atime_ns, int64_t mtime_ns);

/* ---- fsops.c ----------------------------------------------------------------- */

extern const struct fs_ops xf_fs_ops;      /* ctx: the channel's struct xf_ctx */
extern const struct fsctl_ops xf_ctl_ops;
bool     ctl_stopping(void);

/* ---- files.c ----------------------------------------------------------------- */

extern const struct file_ops xf_file_ops;  /* ctx: the file's struct xf_file */

/* Open inode in (checked by fsops.c) with FS_* flags: a new slot. w: the
 * walk that reached it (its directory and name, for the file's events). */
status_t files_open(const struct ext_inode *in, uint32_t flags, bool nochange,
                    const struct xf_walk *w, handle_t *out_ch, handle_t *out_vmo,
                    uint64_t *out_size);
/* A rename (fsops.c, done): the open files named `from` in from_dir are
 * named `to` in to_dir now; those named `to` were renamed over (no
 * directory now), or with exchange are named `from`. */
void     files_renamed(const struct fsw_key *from_dir, const struct ext_name *from,
                       const struct fsw_key *to_dir, const struct ext_name *to, bool exchange);
/* An unlink of `name` in dir: the open files named so are unlinked (their
 * events stay out of the directory's IN_EXCL_UNLINK watches). */
void     files_unlinked(const struct fsw_key *dir, const struct ext_name *name);
/* The order rule (main.c): is the file of port key `key` closed by its
 * client? Its leftover requests and its close, now. Every held file's
 * (the unread bound: its close made no packet) too. */
bool     files_closed(uint64_t key);
void     files_close_now(uint64_t key);
void     files_held_closes(void);
/* Set in's size: shrinking is a first step here, with the inode on the
 * orphan list, the rest after the answer (defer_trunc); its times. */
status_t files_truncate(struct ext_inode *in, uint64_t size);
/* Is inode ino open (a slot names it)? */
bool     files_has(uint32_t ino);
/* ... open for writing (FS_WRITE or FS_APPEND)? */
bool     files_writing(uint32_t ino);
/* ... open, mapped or a directory channel's (dirs_has): its inode lives,
 * if it has no name, on the orphan list until none (adopt.c keeps such
 * orphans across a restart). */
bool     files_held(uint32_t ino);
void     files_event(uint64_t key);
void     files_unheld(void);
void     files_reap(void);
void     files_close_all(void);
void     files_drop_unknown(void);
void     files_release(uint32_t i);
bool     files_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned files_adopt(void);
status_t files_remake(uint32_t i, handle_t *out_ch, handle_t *out_vmo);
bool     files_chan(uint32_t i, uint32_t gen16, struct xf_chan *out);
/* An adopted table, checked against the volume: NULL, or why not. */
const char *files_check(void);
/* Open file f as a lock's `via` (locks.c): its slot and generation; and
 * is the open a via names gone (closed here, or its client's end closed)? */
uint32_t files_via(const struct xf_file *f);
bool     files_via_gone(uint32_t via);

/* ---- views.c ----------------------------------------------------------------- */

/* fs.view's add (fs_view_dispatch): host is the parent's struct xf_ctx. */
status_t views_add(void *host, handle_t ch, uint32_t flags);
/* fs.subview: a view rooted at directory w (walked in parent). */
status_t views_sub(const struct xf_ctx *parent, const struct xf_walk *w, uint32_t flags,
                   handle_t *out);
/* The flags fs_view_dispatch is given for a channel: a subview rooted
 * outside `etc` can't reach it, and a directory channel's relative `etc`
 * is not the volume's, so the lexical etc checks (made on paths as if from
 * the volume's root) are left to walk.c's there. */
uint32_t views_lexical(const struct xf_ctx *c);
void     views_event(uint64_t key);
void     views_unheld(void);
void     views_drop_unknown(void);
void     views_drop(uint32_t i);
bool     views_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned views_adopt(void);
status_t views_remake(uint32_t i, handle_t *out);
bool     views_chan(uint32_t i, uint32_t gen16, struct xf_chan *out);
const char *views_check(void);
/* The whole volume, as the mount's own channels see it. */
void     views_whole(struct xf_ctx *c);

/* ---- dirs.c ------------------------------------------------------------------ */

/* fs.opendir through c of directory w->in (walked in c, checked by
 * fsops.c): a directory channel keeping c's view, its client end *out.
 * ERR_NO_RESOURCES: XF_DIRS, or the writer's XF_DIRS_PROC. */
status_t dirs_open(const struct xf_ctx *c, const struct xf_walk *w, handle_t *out);
/* rename_at's and link_at's to_dir: directory `id` placed in c's view (by
 * climbing: the view's root met on the way), as a directory channel's
 * context *out (its place in *at). ERR_NOT_FOUND: no such directory, or
 * the view doesn't reach it. */
status_t dirs_of_id(const struct xf_ctx *c, uint64_t id, struct xf_ctx *out, struct xf_place *at);
/* The volume's number in opendir's reply (its UUID folded). */
uint64_t dirs_volume(void);
/* fs.view's add on a directory channel: ERR_NOT_SUPPORTED. */
status_t dirs_no_view(void *host, handle_t ch, uint32_t flags);
/* Directory `in` was removed from `parent` (its last name: on the orphan
 * list), inside the running request: every channel holding it remembers
 * parent as its "..". */
void     dirs_removed(const struct ext_inode *in, const struct ext_inode *parent);
/* The parent removed directory ino (generation gen) had, as its channels
 * remember it: false if no channel holds it. */
bool     dirs_up(uint32_t ino, uint32_t gen, uint32_t *up, uint32_t *up_gen);
/* Does a directory channel hold inode ino? */
bool     dirs_has(uint32_t ino);
/* Every directory channel closed (the service stops): a removed directory
 * goes with its last (defer_drop). */
void     dirs_close_all(void);
void     dirs_event(uint64_t key);
void     dirs_unheld(void);
void     dirs_drop(uint32_t i);
void     dirs_drop_unknown(void);
bool     dirs_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned dirs_adopt(void);
status_t dirs_remake(uint32_t i, handle_t *out);
bool     dirs_chan(uint32_t i, uint32_t gen16, struct xf_chan *out);
const char *dirs_check(void);

/* ---- pager.c ----------------------------------------------------------------- */

/* file.map of inode ino (its generation igen, fsize bytes long) with
 * flags FILE_MAP_READ, FILE_MAP_WRITE or FILE_MAP_EXEC (files.c checked
 * them: the open's rights, and Q7's rule): a VMO into *out.
 * ERR_NOT_SUPPORTED with no pager; ERR_NO_RESOURCES: XF_MAX_MAPS. */
status_t maps_make(uint32_t ino, uint32_t igen, uint32_t flags, uint64_t fsize, handle_t *out);
/* Is inode ino mapped (a slot names it)? ... mapped to run? ... is its
 * shared VMO one handed out writable that still lives? */
bool     maps_has(uint32_t ino);
bool     maps_exec(uint32_t ino);
bool     maps_written(uint32_t ino);
/* Its last fs.open closed: its shared VMO is its mappers' alone now. */
void     maps_let_go(uint32_t ino);
/* Inode ino changed, inside the request that changed it, before its
 * commit (pager.c's "The ordering"); its mappings see it. A write of n
 * bytes of `data` at off, its size old before and `size` now ... */
void     maps_wrote(uint32_t ino, uint64_t off, const void *data, uint64_t n, uint64_t old,
                    uint64_t size);
/* ... its size set from old to `size` (a truncate) ... */
void     maps_resized(uint32_t ino, uint64_t old, uint64_t size);
/* ... or a write or truncate that failed: bytes [lo, hi) may have
 * changed on the disk, its size now `size`. */
void     maps_changed(uint32_t ino, uint64_t lo, uint64_t hi, uint64_t size);
/* A read of inode ino: the n bytes at off that came from the disk, with
 * its mappings' stores the disk hasn't yet laid over them. */
status_t maps_peek(uint32_t ino, uint64_t off, uint8_t *data, uint64_t n);
/* The running request ended: done, its change committed; not, given up
 * (the mapped files' sizes set again from their inodes). */
void     maps_settle(bool done);
/* A map the running request made goes (its reply failed). */
void     maps_unmake(uint32_t i);
/* The slot of key's VMO, or -1. */
int      maps_slot_of(uint64_t key);
/* The inode of key's VMO's file now, and its slot (*slot), made from the
 * key if the state doesn't know it (a fresh start's: the inode the key
 * names, if it has a name; *slot -1 if no slot is free). ERR_NOT_FOUND
 * with *slot -1: no such file (gone, or a key to run). */
status_t maps_key(uint64_t key, struct ext_inode *in, int *slot);
/* key's VMO cut off for good (its mappers fail as for a pulled stick). */
void     maps_detach(uint64_t key);
/* A successor's: the keeper's handle of slot i; the table checked; its
 * handles made to agree with the open files; a map's reply made again. */
bool     maps_take(uint32_t i, const handle_t *hs, unsigned n);
const char *maps_check(void);
void     maps_adopt(void);
void     maps_drop_unknown(void);
status_t maps_remake(uint32_t i, handle_t *out);
/* What the instance before took and never answered queued again, its
 * writing pages dirty again (PAGER_NEXT_AGAIN), once: at pager_start, or
 * before, by a write-back in a request adopt.c runs again (a file.sync
 * whose write-back a death cut off must find those pages dirty). */
void     maps_again(void);
/* After the start: the pager waited on, and what the instance before
 * took and never answered served. */
void     pager_start(void);
/* The pager's binding fired: one request served. */
void     pager_event(void);
/* What is queued, served (before each fs request). */
void     pager_reap(void);

/* ---- wback.c ----------------------------------------------------------------- */

/* pager_start's: the age setting read (files.writeback, the volume's own
 * etc/settings). */
void     wback_start(void);
/* A PAGER_REQ_WRITEBACK: the dirty pages of its range written back. */
void     wback_request(const struct pager_request *r);
/* When the age check is next due (DEADLINE_NEVER: never), and the check
 * itself, run if due (main.c's loop: at most once a second, idle or not). */
uint64_t wback_due(void);
void     wback_tick(void);
/* file.sync of open f: its file's shared VMO written back whole; the
 * error the file keeps if it came after f's sample (reported once to each
 * open, Linux's errseq: the sample then catches up, kept->heard recording
 * it for a run again), else OK. */
status_t wback_file(struct xf_file *f);
/* Request seq runs again (adopt.c: not committed): what its file.sync's
 * open heard of put back, so it hears of it again. */
void     wback_unhear(uint64_t seq);
/* A new open's sample of inode ino's write-back errors: the last error's
 * number, or 0 while no open has heard of it (the new open does) or the
 * file has none kept. */
uint32_t wback_sample(uint32_t ino, uint32_t igen);
/* fs.sync: every page dirty now, of every VMO of the pager, written back:
 * the first error, else OK. */
status_t wback_all(void);
/* An orderly stop: as wback_all, for a few seconds at most. */
void     wback_stop(void);

/* ---- watch.c ----------------------------------------------------------------- */

/* An inode as watches name it: its number and generation. */
static inline struct fsw_key xf_key(const struct ext_inode *in)
{
    return (struct fsw_key){ .ino = in->ino, .gen = in->d.generation };
}

/* The state set up empty (state.c's reset): every watcher channel this
 * instance holds ended first, its bell rung (the client's IN_UNMOUNT and
 * IN_IGNORED), then nothing watched. */
void     watch_reset(void);
/* A successor, before its mount: the dead instance's watches taken over,
 * each channel's queue starting with IN_Q_OVERFLOW; a part that fails its
 * checks: set up empty (the keeper's channels are refused, so they close). */
void     watch_adopt_table(void);
/* adopt.c's take of the keeper's slot XF_KEEP_WATCH(i): the channel (and
 * its bell, if given) back, or false if the state doesn't know it. */
bool     watch_take(uint32_t i, const handle_t *hs, unsigned n);
/* A slot of a watcher channel the state doesn't know (a fresh start's):
 * its bell rung before the keeper's restore closes it. */
void     watch_refused(const handle_t *hs, unsigned n);
/* After the restore: every kept channel waited on again, one whose
 * handles didn't come back ended. How many are served. */
unsigned watch_adopt(void);
/* Watcher channel i, generation gen16 (a slot's XF_CHAN_WATCH), as adopt.c
 * finishes its request. */
bool     watch_chan(uint32_t i, uint32_t gen16, struct xf_chan *out);
/* fs.watch's reply that died unsent: the client's end made again. */
status_t watch_remake(uint32_t i, handle_t *out);
/* fs.watch not committed (it runs again): the channel it made forgotten. */
void     watch_unmake(uint32_t i);
/* The service stops for good: every watcher channel ended, its bell rung. */
void     watch_stop(void);
/* After the request in progress at a death was finished: if it was in the
 * journal (answered from its slot, not run again), the reach checks it
 * held for its commit run now, on the disk as it is; else they are
 * dropped (it ran again and made its own). */
void     watch_moves_again(bool committed);
/* Is anything watched on the volume? (A change's one test.) */
bool     watching(void);
/* fs.watch through the view c: a watcher channel, its client end *out. */
status_t watch_new(const struct xf_ctx *c, handle_t *out);
/* A watcher channel's binding fired: its bell taken, or one request served. */
void     watch_event(uint64_t key);
void     watch_unheld(void);
extern const struct fswatch_ops xf_watch_ops;   /* ctx: the channel's record */

/* An event (docs/INOTIFY-PLAN.md 2.2): to directory dir's watches about
 * its entry `name` (len bytes; dir NULL: none), then to inode self's own
 * (NULL: none), mask the same for both; flags FSW_EV_*; cookie a move's
 * (dir alone). Inside a request it waits for the request's commit
 * (ev_settle): a change given up makes none. */
void     ev(const struct fsw_key *dir, const char *name, uint32_t len, const struct fsw_key *self,
            uint32_t mask, uint32_t flags, uint32_t cookie);
/* Inode self goes (its last name and its last open: defer_run): its
 * watches' IN_DELETE_SELF and IN_IGNORED. */
void     ev_gone(const struct fsw_key *self);
/* A rename moved inode `in` from directory `from` into `to` (after its
 * IN_MOVE_SELF): a watch through a narrower view that can no longer reach
 * it, or what is under it, ends (docs/INOTIFY-PLAN.md Q2). Waits for the
 * commit as ev's. */
void     watch_moved(const struct ext_inode *in, const struct ext_inode *from,
                     const struct ext_inode *to);
/* The next move's cookie. */
uint32_t ev_cookie(void);
/* The event of a setattr of `mask` (FS_ATTR_*), as Linux's
 * fsnotify_change makes it: 0 for none. */
uint32_t ev_attr_mask(uint32_t mask);
/* request.c: a request begins; it ended (done: committed): its events
 * queued, else dropped. */
void     ev_begin(void);
void     ev_settle(bool done);

/* ---- locks.c ----------------------------------------------------------------- */

/* file.lock, file.getlock, file.lockowner (xf_file_ops'; ctx: the file). */
status_t locks_op_lock(void *ctx, uint8_t type, uint8_t flags, uint64_t start, uint64_t end,
                       uint64_t owner, uint8_t *out_result, handle_t *out_wake,
                       uint32_t *out_wake_len);
status_t locks_op_getlock(void *ctx, uint8_t type, uint8_t flags, uint64_t start, uint64_t end,
                          uint64_t owner, uint8_t *out_held, uint64_t *out_held_start,
                          uint64_t *out_held_end, uint32_t *out_pid);
status_t locks_op_lockowner(void *ctx, uint64_t *out_owner, handle_t *out_token,
                            uint32_t *out_token_len);
/* Open file f closes (files.c, before its slot goes): the POSIX locks set
 * through it go, each such process's on the file. */
void     locks_closed(const struct xf_file *f);
/* A lock owner's token's binding fired: its last handle closed, so its
 * locks go. */
void     locks_event(uint64_t key);
/* The state set up empty (state.c's reset): the tokens and waits this
 * instance holds closed. */
void     locks_reset(void);
/* A successor's: the table checked (NULL, or what is wrong), the keeper's
 * token of owner i back, and, after files_adopt, the owners whose token
 * didn't come back or closed dropped and the rest waited on; how many. */
const char *locks_check(void);
bool     locks_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned locks_adopt(void);

/* ---- keeper.c ---------------------------------------------------------------- */

#define XF_KEEP_FILE(i) ((uint32_t)(i))
#define XF_KEEP_VIEW(i) ((uint32_t)(XF_MAX_FILES + (i)))
#define XF_KEEP_MAP(i)  ((uint32_t)(XF_MAX_FILES + XF_VIEWS + (i)))
#define XF_KEEP_WATCH(i) ((uint32_t)(XF_MAX_FILES + XF_VIEWS + XF_MAX_MAPS + (i)))
#define XF_KEEP_LOCK(i) ((uint32_t)(XF_MAX_FILES + XF_VIEWS + XF_MAX_MAPS + FSW_CHANNELS + (i)))
#define XF_KEEP_DIR(i)  ((uint32_t)(XF_KEEP_LOCK(XF_LOCK_OWNERS) + (i)))
#define XF_KEEP_SLOTS   (XF_MAX_FILES + XF_VIEWS + XF_MAX_MAPS + FSW_CHANNELS + XF_LOCK_OWNERS + \
                         XF_DIRS)

status_t kept_put(uint32_t slot, const handle_t *hs, unsigned n);
/* ... with a ring the keeper sounds on hs[1] when it closes the slot
 * itself (<keep.h>: a watcher's bell, so a dead extfs given up wakes its
 * pollers, IR's 7). */
status_t kept_put_ring(uint32_t slot, signals_t ring, const handle_t *hs, unsigned n);
void     kept_drop(uint32_t slot);

/* ---- adopt.c ----------------------------------------------------------------- */

/* Mount (the recovery), then carry on from a dead instance's state if
 * there is one. *no_volume: nothing extfs can serve is there. */
status_t adopt(bool adopted, bool *no_volume);
/* main.c's: mount the volume (formatting a blank one if asked). */
status_t mount(bool *no_volume);
/* main.c's order rule (docs/INOTIFY-PLAN.md 2.3): every close already made
 * handled now, before the request about to be served. */
void     order_closes(void);
/* The orphans the state doesn't hold open, dropped (at a mount). */
void     recover_orphans(void);

/* ---- test.c ------------------------------------------------------------------ */

enum xf_die {
    XF_DIE_WRITE,             /* a device write landed (data, log, commit or home) */
    XF_DIE_JOURNAL,           /* the request's transaction committed, the slot not yet */
    XF_DIE_COMMIT,            /* the slot committed, not answered */
    XF_DIE_ANSWERED,          /* answered: its reply waits for the next system call */
    XF_DIE_MAP,               /* a file.map made its VMO, not answered */
    XF_DIE_SUPPLY,            /* a page request read, its pages not supplied */
    XF_DIE_WBACK,             /* a write-back's step: a run begun, its data written, the
                               * batch committed, a run answered (wback.c) */
    XF_DIE_REMOVED,           /* a held directory's removal recorded (dirs.c: its ".."
                               * remembered), not committed */
    XF_DIE_HEARD,             /* a file.sync's open heard of a write-back error (the mark
                               * in the state, wback.c), the answer not committed */
    XF_DIE_COUNT,
};
bool     test_word(const char *w);
void     test_die(enum xf_die point);
/* The hang power: extfs stops answering (sleeps until it is killed) the
 * n-th time it reaches point, as one stuck in its disk would. */
void     test_hang(enum xf_die point);
void     test_crash(const char *path, uint32_t len);
bool     test_fixed_time(void);
bool     test_lazy_tail(void);
bool     test_quick_retry(void);
bool     test_quick_wait(void);
/* A file grown from old to size bytes (files.c's truncate): with the
 * unwritten-grow power, the new blocks allocated as unwritten extents (as
 * fallocate would), else nothing. */
status_t test_unwritten(struct ext_inode *in, uint64_t old, uint64_t size);
