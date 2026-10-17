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
 * number and generation, flags), the views (flags, the root's inode and
 * place), the mapped files (inode, kind, key). The block cache and the
 * directory hash are rebuilt.
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
 * subviews; pager.c mapped files and the mount's pager; test.c the test
 * powers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <idl/fsctl.h>
#include <os.h>
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
#define XF_FILE_BUF    (64u << 10) /* each open file's transfer buffer, bytes */
#define XF_ROOT_MAX    1024u       /* a subview's root's place, bytes with its NUL */
#define XF_LINKS_MAX   40u         /* links one path may follow (POSIX's SYMLOOP_MAX) */
#define XF_MAGIC       0xef53u     /* statfs's magic: Linux's EXT4_SUPER_MAGIC */
#define XF_NAME_MAX    255u        /* statfs's longest name */
#define XF_FEATURES    (FS_FEAT_LINKS | FS_FEAT_MODES | FS_FEAT_CASE | FS_FEAT_BIG | \
                        FS_FEAT_MULTI_WRITER)
#define XF_DEFER_MAX   64u         /* orphans and truncations left for after an answer */
#define XF_MAX_MAPS    256u        /* mapped files at once (file.map's VMOs: pager.c) */

/* A mapped file's key: a sequence number and its slot of maps[] (pager.c). */
#define XF_MAP_KEY(seq, i) ((uint64_t)(seq) << 8 | (uint64_t)(i))
#define XF_MAP_SLOT(key)   ((unsigned)((key) & 0xff))
_Static_assert(XF_MAX_MAPS <= 256, "a slot fits a key's low byte");

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
#define XF_KEY_SLOT(k)     ((unsigned)((k) & 0xffff))
#define XF_KEY_GEN(k)      ((uint32_t)((k) >> 16 & 0xffffffffu))

#define XF_CHAN_FS         1u
#define XF_CHAN_CTL        2u
#define XF_CHAN_OWN        3u
#define XF_CHAN_FILE(s, g) (1u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
#define XF_CHAN_VIEW(s, g) (2u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
_Static_assert(XF_MAX_FILES <= 256 && XF_VIEWS <= 256, "a slot fits a channel id's low byte");

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
    bool        ram;        /* the disk is a VMO (EXTFS_SR_RAM): no block channel */
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
#define XF_STATE_LAYOUT 3u            /* bump on any change to struct xf_state or the
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
};

/* One view (fs.view, fs.subview; its channel is views.c's). */
struct xf_view {
    bool     used;
    uint32_t flags;           /* FS_VIEW_* */
    uint32_t gen;
    uint32_t root_ino;        /* its root directory (EXT_ROOT_INO: the volume's) ... */
    uint32_t root_igen;       /* ... and its generation */
    uint32_t root_len;        /* the root's place from the volume's root ("" for it) */
    char     root[XF_ROOT_MAX];
};

/* One VMO file.map made (pager.c): the file it shows, and how. */
struct xf_map {
    bool     used;
    uint8_t  kind;            /* FILE_MAP_READ (the file's shared VMO) or FILE_MAP_EXEC */
    uint32_t ino;             /* the file: its inode ... */
    uint32_t igen;            /* ... and that inode's generation */
    uint64_t key;             /* its VMO's key: XF_MAP_KEY(seq, its slot) */
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
 * reply never went out, and drops it if the request runs again). A map
 * that shared a slot made before (XF_MADE_MAP_SHARED) is remade but never
 * dropped: other mappers have it. */
#define XF_MADE_FILE       1u
#define XF_MADE_VIEW       2u
#define XF_MADE_MAP        3u
#define XF_MADE_MAP_SHARED 4u
struct xf_made {
    uint64_t seq;
    uint32_t kind;
    uint32_t index;
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

enum xf_proto { XF_PROTO_FS, XF_PROTO_FILE, XF_PROTO_CTL };

/* The context of an fs request: the channel's view (the mount's own
 * channels: the whole volume, no flags). */
struct xf_ctx {
    uint32_t    flags;        /* FS_VIEW_*: judged where paths resolve (walk.c) */
    uint32_t    root_ino, root_igen;
    const char *root;         /* the root's place from the volume's root */
    uint32_t    root_len;
    int         view;         /* its slot of views[], or -1 */
};

struct xf_chan {
    handle_t        ch;
    uint32_t        id;       /* XF_CHAN_* */
    enum xf_proto   proto;
    struct xf_ctx   ctx;      /* XF_PROTO_FS */
    struct xf_file *file;     /* XF_PROTO_FILE */
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

/* After the answer: drop orphan `ino` (unless it is open), or finish
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

struct xf_walk {
    bool     found;           /* the path names something: `in` */
    bool     has_name;        /* it ends in a name (not the view's root, ".", ".."): `dir`
                               * holds it, and `name` is it */
    bool     must_dir;        /* it ended in '/' */
    struct ext_inode in;
    struct ext_inode dir;
    struct ext_name name;
    char     namebuf[EXT_NAME_MAX + 1];
    char     phys[FS_PATH_LEN + 1];   /* where it lies, from the volume's root (no leading '/') */
    uint32_t phys_len;
    /* ERR_LINK_OUT: where it went on (fs.resolve's answers) */
    uint8_t  kind;
    uint32_t used;
    char     next[FS_PATH_LEN + 1];
    uint32_t next_len;
};

/* Walk path (len bytes, as the protocol carried it) in view c: every name,
 * ".." physically, links inside the view followed (up to XF_LINKS_MAX:
 * ERR_LINK_LOOP past it), the search bit of every directory looked into
 * checked. The last name need not be there (found false). Errors:
 * ERR_LINK_OUT (an absolute link, or ".." above the view's root: w->kind,
 * used, next); ERR_NOT_FOUND, ERR_NOT_DIR on the way; ERR_ACCESS_DENIED
 * (a search bit, or a directory the view hides wholly); ERR_NAME_TOO_LONG;
 * ERR_INVALID_ARGS (a name ext can't hold); ERR_IO. */
status_t walk(const struct xf_ctx *c, const char *path, uint32_t len, uint32_t flags,
              struct xf_walk *w);
/* fs.resolve's answer for an INSIDE path: the place relative to the view's root. */
void     walk_inside(const struct xf_ctx *c, const struct xf_walk *w, char *out,
                     uint32_t *out_len);
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

/* Open inode in (checked by fsops.c) with FS_* flags: a new slot. */
status_t files_open(const struct ext_inode *in, uint32_t flags, bool nochange,
                    handle_t *out_ch, handle_t *out_vmo, uint64_t *out_size);
/* Set in's size: shrinking is a first step here, with the inode on the
 * orphan list, the rest after the answer (defer_trunc); its times. */
status_t files_truncate(struct ext_inode *in, uint64_t size);
/* Is inode ino open (a slot names it)? */
bool     files_has(uint32_t ino);
/* ... open for writing (FS_WRITE or FS_APPEND)? */
bool     files_writing(uint32_t ino);
/* ... open or mapped: its inode lives, if it has no name, on the orphan
 * list until neither (adopt.c keeps such orphans across a restart). */
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

/* ---- views.c ----------------------------------------------------------------- */

/* fs.view's add (fs_view_dispatch): host is the parent's struct xf_ctx. */
status_t views_add(void *host, handle_t ch, uint32_t flags);
/* fs.subview: a view rooted at directory w (walked in parent). */
status_t views_sub(const struct xf_ctx *parent, const struct xf_walk *w, uint32_t flags,
                   handle_t *out);
/* The flags fs_view_dispatch is given for a channel: a subview rooted
 * outside `etc` can't reach it, so the lexical etc checks (made on paths
 * relative to its root) are left to walk.c's. */
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

/* ---- pager.c ----------------------------------------------------------------- */

/* file.map of inode ino (its generation igen, fsize bytes long) with
 * flags FILE_MAP_READ or FILE_MAP_EXEC (files.c checked them, the open's
 * read right and, to run, that nobody writes it): a VMO into *out.
 * ERR_NOT_SUPPORTED with no pager; ERR_NO_RESOURCES: XF_MAX_MAPS. */
status_t maps_make(uint32_t ino, uint32_t igen, uint32_t flags, uint64_t fsize, handle_t *out);
/* Is inode ino mapped (a slot names it)? ... mapped to run? */
bool     maps_has(uint32_t ino);
bool     maps_exec(uint32_t ino);
/* Its last fs.open closed: its shared VMO is its mappers' alone now. */
void     maps_let_go(uint32_t ino);
/* Inode ino changed, inside the request that changed it: bytes [lo, hi)
 * written (or cut: lo == hi), its size now `size`. Its mappings see it. */
void     maps_changed(uint32_t ino, uint64_t lo, uint64_t hi, uint64_t size);
/* The running request ended: done, its change committed; not, given up
 * (the mapped files' sizes set again from their inodes). */
void     maps_settle(bool done);
/* A map the running request made goes (its reply failed). */
void     maps_unmake(uint32_t i);
/* A successor's: the keeper's handle of slot i; the table checked; its
 * handles made to agree with the open files; a map's reply made again. */
bool     maps_take(uint32_t i, const handle_t *hs, unsigned n);
const char *maps_check(void);
void     maps_adopt(void);
void     maps_drop_unknown(void);
status_t maps_remake(uint32_t i, handle_t *out);
/* After the start: the pager waited on, and what the instance before
 * took and never answered served. */
void     pager_start(void);
/* The pager's binding fired: one request served. */
void     pager_event(void);
/* What is queued, served (before each fs request). */
void     pager_reap(void);

/* ---- keeper.c ---------------------------------------------------------------- */

#define XF_KEEP_FILE(i) ((uint32_t)(i))
#define XF_KEEP_VIEW(i) ((uint32_t)(XF_MAX_FILES + (i)))
#define XF_KEEP_MAP(i)  ((uint32_t)(XF_MAX_FILES + XF_VIEWS + (i)))
#define XF_KEEP_SLOTS   (XF_MAX_FILES + XF_VIEWS + XF_MAX_MAPS)

status_t kept_put(uint32_t slot, const handle_t *hs, unsigned n);
void     kept_drop(uint32_t slot);

/* ---- adopt.c ----------------------------------------------------------------- */

/* Mount (the recovery), then carry on from a dead instance's state if
 * there is one. *no_volume: nothing extfs can serve is there. */
status_t adopt(bool adopted, bool *no_volume);
/* main.c's: mount the volume (formatting a blank one if asked). */
status_t mount(bool *no_volume);
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
    XF_DIE_COUNT,
};
bool     test_word(const char *w);
void     test_die(enum xf_die point);
void     test_crash(const char *path, uint32_t len);
bool     test_fixed_time(void);
bool     test_lazy_tail(void);
bool     test_quick_retry(void);
