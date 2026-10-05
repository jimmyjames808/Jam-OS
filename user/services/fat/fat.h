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
 * Files: main.c the startup, mount and event loop; state.c the state
 * VMO that holds struct fat_state (what fat knows); request.c each request
 * read into a slot of it and run as one operation (begin, run, commit,
 * send, answer); undo.c the copy that puts the state back as it was before
 * an operation; disk.c FatFs's disk callbacks over `block`, the volume's
 * dirty flag and get_fattime; hold.c the writes held back until their
 * request commits (and an FS_GATHER file's, until its sync or close);
 * cache.c the write-through block cache under them;
 * fsops.c the `fs` methods; dirs.c the cursors that make listing a
 * directory linear; fileops.c the open-file table and the `file`
 * methods; views.c the narrower `fs` channels (fs.view); path.c paths,
 * names, times and the FRESULT -> ERR_* mapping. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <fatsvc.h>
#include <ff.h>
#include <fs_idl.h>
#include <idl/fsctl.h>
#include <os.h>
#include <svcstate.h>

#define FAT_MAX_FILES  32          /* open files at once */
#define FAT_DIR_CURSORS 8          /* directories being listed at once (dirs.c) */
#define FAT_FILE_BUF   (64u << 10) /* each open file's transfer buffer, bytes */
#define FAT_SECTOR     512u        /* the only sector size (FF_MAX_SS) */
#define FAT_FILE_MAX   0xffffffffull /* FAT's largest file, bytes */
/* The most one write or truncate may grow a file past its end (the gap is
 * filled with zeros, and nothing else is served meanwhile): grow in steps. */
#define FAT_GROW_MAX   (16u << 20)
#define FAT_VIEWS      32          /* views (fs.view) served at once */

/* Port keys: the fs channel, the block channel, open file `slot`
 * (FAT_KEY_FILE(slot, gen); gen tells a reused slot's packets apart) and
 * view `slot` (FAT_KEY_VIEW(slot, gen), the same way). */
#define FAT_KEY_FS          1ull
#define FAT_KEY_BLOCK       2ull
#define FAT_KEY_CTL         3ull
#define FAT_KEY_FILE_BIT    (1ull << 62)
#define FAT_KEY_FILE(s, g)  (FAT_KEY_FILE_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define FAT_KEY_VIEW_BIT    (1ull << 61)
#define FAT_KEY_VIEW(s, g)  (FAT_KEY_VIEW_BIT | (uint64_t)(g) << 16 | (uint64_t)(s))
#define FAT_KEY_SLOT(k)     ((unsigned)((k) & 0xffff))
#define FAT_KEY_GEN(k)      ((uint32_t)((k) >> 16 & 0xffffffffu))

/* Which channel a request was read from, as its slot records it (32 bits:
 * svcstate's `channel`): the fs channel, fsctl, open file `slot` or view
 * `slot` (with the low 16 bits of its generation). */
#define FAT_CHAN_FS         1u
#define FAT_CHAN_CTL        2u
#define FAT_CHAN_FILE(s, g) (1u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))
#define FAT_CHAN_VIEW(s, g) (2u << 30 | ((uint32_t)(g) & 0xffffu) << 8 | (uint32_t)(s))


/* The steps of a start, timed for a restart's log line (adopt.c). */
enum fat_at {
    FAT_AT_MAIN,              /* main ran */
    FAT_AT_DISK,              /* the block channel taken (disk_open) */
    FAT_AT_STATE,             /* the state mapped and its header checked */
    FAT_AT_HANDLES,           /* the state checked, the kept handles taken back */
    FAT_AT_COUNT,
};

/* The volume: fat serves exactly one. What this instance has of it: its
 * handles, its mapping of the block buffer, what block.info and argv
 * said. What fat knows of it that must outlive the process is in the
 * state (struct fat_state, below). */
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
    handle_t    keep;         /* SR_KEEP: the keep channel (keeper.c), or HANDLE_INVALID */
    handle_t    serve;        /* the fs channel (FAT_SR_SERVE) */
    handle_t    ctl;          /* this instance's fsctl channel, or HANDLE_INVALID */
    /* How the last instance ended, from argv (FAT_ARG_KILLED, FAT_ARG_CRASHED):
     * NULL for a first start. Only a crash counts against the request in
     * progress (adopt.c: the bad-request rule). */
    const char *ended;
    uint64_t    at[FAT_AT_COUNT];   /* when each step of the start was done (uptime ns) */
};

extern struct fat_vol vol;

/* ---- the state (state.c) --------------------------------------------------------- */

/* What fat knows of the volume and its clients that a successor must find
 * again (docs/M11.6-PLAN.md, "Where each service's state lives"): FatFs's
 * volume and every open file's FIL, fat's tables of open files and views,
 * the dirty flag, and the held writes. It is one struct, the service's
 * own area of a state VMO (<svcstate.h>), mapped at a fixed address so
 * that the pointers FatFs keeps inside it (each FIL's volume, its
 * directory entry in the volume's window) stay valid from one instance to
 * the next. Nothing in it points outside it but FATFS's lfnbuf (FatFs's
 * static name buffer, in fat's own image).
 *
 * Handles can't live in a VMO: each table's handles are kept apart, in
 * fat's own memory, by the same index (fileops.c, views.c). What fat
 * rebuilds instead (the block cache, the directory cursors, the block
 * buffer's mapping) is in fat's own memory too.
 *
 * Every request is run as one operation on it (request.c): an undo copy
 * first (undo.c), every disk write held (hold.c), then the commit word.
 * devmgr makes the VMO and keeps it across fat's deaths (SR_STATE); a fat
 * started with a used one carries on from it (adopt.c). Without SR_STATE
 * (a test's RAM disk) fat makes one of its own and always starts fresh. */

#define FAT_STATE_KIND 0x20746166u   /* "fat ": svcstate's kind */
#define FAT_STATE_LAYOUT 3u          /* bump on any change to struct fat_state */
#define FAT_HOLD_MAX  2304u   /* sectors held at most: a MiB of a file, the FAT sectors
                               * that chain it (on one-sector clusters, 16 per copy) */
#define FAT_HOLD_RUNS 255u    /* runs of consecutive sectors held at most (run_of is a byte) */
#define FAT_HOLD_CHUNK 128u   /* sectors whose pages are committed at a time (64 KiB) */
/* What one request may hold at most, but for the corner hold.c's header
 * names: a 64 KiB write or a new cluster of up to 64 KiB, the FAT sectors
 * that chain it in each copy, a directory sector; fs.sync's every open file
 * (its sector and its entry). A request starts with this much room. */
#define FAT_OP_ROOM   288u    /* sectors */
#define FAT_OP_RUNS   96u     /* runs */
#define FAT_UNDO_BYTES (32u << 10)   /* the undo copy's log: fs.sync over every open file */
#define FAT_UNDO_SECTORS 32u  /* held sectors a request may overwrite in place, saved */
/* A request slot's request area: the request message, then from
 * FAT_SLOT_DATA on its data (request.c: the bounce buffer). */
#define FAT_SLOT_DATA 4096u

/* The dirty flag (FAT[1]'s clean-shutdown bit) and what is on the medium:
 * disk.c's. */
struct fat_disk {
    bool     track_dirty;     /* a writable FAT16/FAT32 volume */
    bool     clean_on_disk;   /* what the bit on the medium says now */
    bool     unflushed;       /* a sector was written since the last block.sync */
    /* A held write (FS_GATHER) failed to reach the disk: nothing more is
     * written (FatFs's state is ahead of the disk) until fat starts
     * afresh, and an FS_GATHER file's sync fails (ERR_IO). */
    bool     hold_failed;
    uint8_t  clean_mask;      /* the bit in FAT sector 0 ... */
    uint32_t clean_off;       /* ... in this byte */
    uint32_t fat0[2];         /* first sector of each FAT copy */
    uint32_t nfats;           /* copies (1 or 2) */
};

/* A file that is open, once however many fs.open calls share it. */
struct fat_open {
    uint32_t refs;            /* slots of files[] that use it; 0: free */
    bool     unsynced;        /* written since its last f_sync */
    char     path[FS_PATH_MAX];/* its resolved path: what makes two opens one file */
    FIL      fil;             /* FatFs's file, opened to read (and to write, if its first
                               * open asked) */
};

/* One fs.open: a slot of the table in fileops.c (its channel and buffer
 * are there, by the same index). */
struct fat_file {
    bool     used;            /* the slot holds an open file */
    uint32_t gen;             /* bumped on every open of this slot */
    uint32_t flags;           /* FS_* it was opened with */
    uint32_t open;            /* its file: an index into opens[] */
};

/* One view (fs.view; its channel is in views.c, by the same index). */
struct fat_view {
    bool     used;            /* the slot holds a view */
    uint32_t flags;           /* FS_VIEW_* */
    uint32_t gen;             /* bumped on every use of the slot: its port key */
};

/* The held writes (hold.c's header says what they are and when they go). */
struct fat_hold {
    uint32_t ready;           /* sectors of data whose pages are committed (FAT_HOLD_CHUNKs) */
    uint32_t held;            /* sectors held */
    uint32_t runs;            /* runs begun, in order */
    uint64_t out_sectors;     /* sectors that went out held (fat's last line) */
    uint64_t out_writes;      /* ... in this many block writes */
    uint64_t run_first[FAT_HOLD_RUNS];   /* run r holds run_first[r] .. + run_len[r] - 1 */
    uint32_t run_len[FAT_HOLD_RUNS];
    uint64_t lba[FAT_HOLD_MAX];          /* data's sector i is for this sector */
    uint8_t  run_of[FAT_HOLD_MAX];       /* ... and belongs to this run */
    _Alignas(PAGE_SIZE) uint8_t data[FAT_HOLD_MAX * FAT_SECTOR];   /* committed as used */
};

/* What a request asks of the disk after its commit (request.c's send): a
 * successor that finds it committed and not sent does it again. */
#define FAT_SETTLE      1u    /* mark the volume clean (disk_settle(false)) */
#define FAT_SETTLE_SYNC 2u    /* ... and flush the mark too (disk_settle(true)) */
struct fat_post {
    bool     release;         /* what is held goes out: the request held writes that may
                               * not stay held (they aren't an FS_GATHER file's) */
    bool     flush;           /* then block.sync (FatFs asked: CTRL_SYNC) */
    uint8_t  settle;          /* then 0, FAT_SETTLE or FAT_SETTLE_SYNC */
};

/* The undo copy (undo.c's header): what the operation in progress may
 * change, as it was before. Valid while `op` is kept->ops_done + 1 and,
 * for a request, its slot isn't committed (undo_pending). */
struct fat_undo {
    uint64_t op;              /* the operation it undoes (set last, released); 0: none */
    uint64_t seq;             /* its request's slot number; 0: a close, which has no slot */
    uint32_t closing;         /* a close's: the slot of files[] it closes */
    uint32_t used;            /* bytes of log in use (each record, then this, released) */
    uint32_t opens;           /* bit i: opens[i] is in the log */
    uint32_t held;            /* the hold's sectors when it began ... */
    uint32_t runs;            /* ... and its runs */
    uint32_t nsect;           /* held sectors saved before being overwritten in place */
    bool     spent;           /* part of it went out early (the hold was full), or the log
                               * was: it can't be undone */
    uint32_t sect_at[FAT_UNDO_SECTORS];   /* the hold index each saved sector was at */
    _Alignas(8) uint8_t log[FAT_UNDO_BYTES];   /* records: where (from kept), length, bytes */
    _Alignas(PAGE_SIZE) uint8_t sect[FAT_UNDO_SECTORS][FAT_SECTOR];
};

/* What the request numbered `seq` made that its reply hands out (fs.open's
 * file, fs.view's view): a successor that finds the request committed
 * tells by that slot's channel whether the reply arrived (adopt.c). */
#define FAT_MADE_FILE 1u
#define FAT_MADE_VIEW 2u
struct fat_made {
    uint64_t seq;             /* the request (a slot's seq); 0: none */
    uint32_t kind;            /* FAT_MADE_FILE or FAT_MADE_VIEW */
    uint32_t index;           /* its slot of files[] or views[] */
};

struct fat_state {
    FATFS           fs;                    /* FatFs's volume */
    struct fat_disk disk;
    bool            mounted;               /* fs is a mounted volume, its dirty flag
                                            * found (disk_watch): what a successor adopts */
    uint32_t        close_crashes;         /* crashes while operation ops_done + 1, a
                                            * close, was in progress (adopt.c) */
    struct fat_open opens[FAT_MAX_FILES];  /* at most one per slot of files[] */
    struct fat_file files[FAT_MAX_FILES];
    struct fat_view views[FAT_VIEWS];
    uint64_t        ops_done;              /* operations committed: a close's commit word */
    struct fat_post post;                  /* the committed request's send */
    struct fat_made made;                  /* what the last request that made a slot made */
    uint64_t        data_seq[2];           /* slot i's data holds request data_seq[i]'s bytes
                                            * (a file.write's, all of them: set after the copy) */
    _Alignas(PAGE_SIZE) struct fat_undo undo;
    struct fat_hold hold;                  /* last: most of the state, untouched until used */
};

/* The state, mapped (state_open); NULL before. */
extern struct fat_state *kept;

/* Map the state VMO (SR_STATE, or one made now) at SVCSTATE_ADDR, after
 * disk_open (the state is bound to vol.name and vol.blocks). *adopted: a
 * dead instance's state that svcstate accepted (adopt.c checks the rest);
 * otherwise it is set up empty. Errors as svcstate_create's and
 * svcstate_open's. */
status_t state_open(bool *adopted);
/* Set the state up empty, as a first start has it: everything but the
 * views' table if keep_views (adopt.c: their channels outlive a volume
 * that is read again from the disk). */
void     state_reset(bool keep_views);
/* Commit the state's pages under [p, p + len), inside *kept: so that
 * running out of memory fails now instead of faulting later. vmo_commit's
 * errors (ERR_NO_MEMORY). */
status_t state_commit(const void *p, size_t len);
/* The request slots (svcstate's mapping of the state). */
struct svcstate *state_slots(void);

/* ---- request.c -------------------------------------------------------------------- */

/* The protocol a channel speaks. */
enum fat_proto {
    FAT_PROTO_FS,             /* fs (the mount's channel, or a view) */
    FAT_PROTO_FILE,           /* file (an open file) */
    FAT_PROTO_CTL,            /* fsctl */
};

/* One channel fat serves, as serve_one takes it. */
struct fat_chan {
    handle_t        ch;       /* fat's end */
    uint32_t        id;       /* FAT_CHAN_*: what the slot records */
    enum fat_proto  proto;
    uint32_t        flags;    /* FAT_PROTO_FS: the view's FS_VIEW_* (0: the whole volume) */
    struct fat_file *file;    /* FAT_PROTO_FILE: its slot */
};

/* What an operation may change (undo.c saves that much). */
enum fat_op {
    FAT_OP_FILE,              /* a file request: FatFs's volume and that file's open */
    FAT_OP_FS,                /* an fs or fsctl request: the volume, the tables, every open
                               * it touches (undo_open) */
    FAT_OP_CLOSE,             /* the close of a file whose client has gone */
};

/* Read one request from c->ch into a slot and run it as an operation:
 * undo copy, run with the hold on, commit, send what it held, answer. OK
 * once a message was handled (answered, or thrown away); otherwise the
 * read's status (ERR_SHOULD_WAIT: nothing queued; ERR_PEER_CLOSED: the
 * client is gone). */
status_t serve_one(const struct fat_chan *c);
/* Around the close of file f as an operation of its own (fileops.c's
 * close_op): begin (the undo copy), then, once closed, the commit and the
 * send. */
void     op_close_begin(const struct fat_file *f);
void     op_close_end(void);
/* Built with FAT_RERUN_CHECK only (request.c): the close just run is
 * undone and run again (close(f)), and must do the same. */
void     op_close_again(struct fat_file *f, void (*close)(struct fat_file *f));
/* Is an operation running (begun, not yet committed)? Then disk writes are
 * held, and a flush or the clean mark waits for the send (disk.c). */
bool     op_running(void);
/* The running operation's writes may stay held after it (an FS_GATHER
 * file's write, or a read made while such writes are held). */
void     op_gather(void);
/* The running request's data buffer (FAT_FILE_BUF bytes, in its slot). */
uint8_t *op_bounce(void);
/* Does the bounce hold the running request's data already (it is run
 * again: its bytes were copied in by its first run)? */
bool     op_data_in(void);
/* The running request's data is all in the bounce now. */
void     op_data_copied(void);
/* The first hold index the running operation wrote: sectors held before
 * it are committed (hold.c saves them before overwriting them). */
uint32_t op_hold_first(void);
/* The running operation held a sector (hold.c). */
void     op_wrote(void);
/* The hold filled up in the middle of the running operation: what it holds
 * goes out now, in steps (hold.c's header): logged once, and the undo copy
 * spent. Called before that release. */
void     op_steps(void);
/* The running request made slot `index` of files[] or views[]
 * (FAT_MADE_*), which its reply hands out: into kept->made. */
void     op_made(uint32_t kind, uint32_t index);
/* Run the request in `slot` (taken already) as serve_one does: for a
 * successor's request in progress (adopt.c). */
void     serve_slot(const struct fat_chan *c, unsigned slot);
/* Commit and answer the request in `slot` with a bare status, on c (NULL:
 * its caller is gone: only marked answered). */
void     answer_status(const struct fat_chan *c, unsigned slot, status_t status);
/* The committed reply in `slot` (rn bytes), with handles rhs[0..rhn)
 * (moved; closed if the client is gone): it waits for fat's next take or
 * port wait, which sends it and marks the slot's reply out. */
void     answer(const struct fat_chan *c, unsigned slot, uint32_t rn, handle_t *rhs,
                uint32_t rhn);
/* The send of a committed operation (kept->post), done again (adopt.c). */
status_t op_resend(void);
/* The reply waiting for fat's next system call (answer's) out now: before
 * fat ends, or before it does something long with nothing to carry it. */
void     reply_flush(void);
/* fat's port wait, which sends the reply waiting first (main.c's loop). */
status_t fat_wait(struct port_packet *pkt);

/* ---- adopt.c ---------------------------------------------------------------------- */

/* Carry on from a dead instance's state (state_open said adopted): check
 * it, FatFs told the volume is mounted, the kept handles taken back, the
 * request or close in progress undone or finished, the restart logged.
 * When the state can't be carried on from, the volume is mounted afresh
 * from the disk (views kept if their table can be trusted), as main's
 * mount would. *no_volume as main.c's mount. */
status_t adopt(bool adopted, bool *no_volume);
/* Mount from the disk (main.c): a first start, or a state not adopted. */
status_t mount(bool *no_volume);

/* ---- keeper.c --------------------------------------------------------------------- */

#define FAT_KEEP_FILE(i) ((uint32_t)(i))                  /* files[i]'s keep slot */
#define FAT_KEEP_VIEW(i) ((uint32_t)(FAT_MAX_FILES + (i)))  /* views[i]'s */

/* Put duplicates of hs[0..n) with the keeper as slot `slot`: before the
 * request that made them commits. keep_put's errors. */
status_t kept_put(uint32_t slot, const handle_t *hs, unsigned n);
/* The state has forgotten slot `slot`: the keeper is told now, or after
 * the commit if an operation runs (kept_flush). */
void     kept_drop(uint32_t slot);
/* The drops owed by the operation just committed, sent. */
void     kept_flush(void);
/* The drops owed are forgotten: the operation was undone. */
void     kept_cancel(void);

/* ---- test.c ----------------------------------------------------------------------- */

/* Where a test power may end the process (test.c). */
enum fat_die {
    FAT_DIE_HELD,             /* a write being held (hold_put) */
    FAT_DIE_COMMIT,           /* right after a commit */
    FAT_DIE_SEND,             /* after a block write of what was held went out */
    FAT_DIE_REPLY,            /* sent, not answered */
    FAT_DIE_ANSWERED,         /* answered: its reply waits for the next take or port wait */
    FAT_DIE_COUNT,
};
/* An argv word that is a test power: taken (true), or not one. */
bool     test_word(const char *w);
void     test_die(enum fat_die point);
/* fs.stat of `path` (resolved): crash-on's crash. */
void     test_crash(const char *path);
/* fixed-time: get_fattime gives 2026-01-01 00:00:00 only. */
bool     test_fixed_time(void);

/* ---- undo.c ----------------------------------------------------------------------- */

/* Begin the undo copy of operation number `op` (kept->ops_done + 1): a
 * request's (seq: its slot's number) or a close's (seq 0) of file f
 * (FAT_OP_FILE and FAT_OP_CLOSE). */
void     undo_begin(enum fat_op kind, uint64_t op, uint64_t seq, const struct fat_file *f);
/* opens[] entry o is about to change: into the copy, once per operation. */
void     undo_open(const struct fat_open *o);
/* Hold index i, held by an earlier (committed) operation, is about to be
 * overwritten: its bytes into the copy. false: no room (the caller holds
 * the sector anew instead). */
bool     undo_sector(uint32_t i);
/* The operation went out in part before its commit: it can't be undone. */
void     undo_spend(void);
/* Is there an operation to undo: begun, its copy whole, not committed? */
bool     undo_pending(void);
/* Put the state back as it was before the operation in progress (a
 * successor's first step for a request it runs again); the block cache
 * and the directory cursors, which may hold what it did, are dropped.
 * Handles are the caller's to reconcile. false: nothing to undo, or it
 * can't be (spent). */
bool     undo_restore(void);

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
 * called dirty. Asked while an operation runs, it is done by the
 * operation's send, after its commit (kept->post), and answers OK here. */
status_t disk_settle(bool durable);
/* What is held out, then block.sync if anything was written since the
 * last one (FatFs's CTRL_SYNC, which an operation leaves to its send). */
status_t disk_flush(void);

/* One block.write of `count` sectors at `sector` from the block buffer
 * (vol.bbuf), then into the cache; count at most the buffer's worth. */
status_t disk_block_write(uint64_t sector, uint32_t count);
/* data is about to be written as `sector`: if that is FAT sector 0 of a
 * FAT copy, its clean bit is cleared (the volume is in use). */
void     disk_patch_dirty(uint8_t *data, uint64_t sector);
/* One block.read of `count` sectors at `sector` into the block buffer
 * (vol.bbuf), after what is held; count at most the buffer's worth. */
status_t disk_block_read(uint64_t sector, uint32_t count);
/* Sectors straight from the disk into buff, a buffer's worth per call. */
status_t disk_read_direct(uint64_t sector, uint32_t count, uint8_t *buff);

/* ---- hold.c ----------------------------------------------------------------------- */

/* Are writes held now (an FS_GATHER file's, waiting for its sync or
 * close)? */
bool     hold_pending(void);
/* Before an operation: what is held (all of it committed) goes out if the
 * hold lacks room for a whole request (FAT_OP_ROOM, FAT_OP_RUNS). */
status_t hold_make_room(void);
/* count sectors of buff for `sector` on, held (while an operation runs:
 * disk_write): each replaces an earlier hold of the same sector (saved
 * first in the undo copy if an earlier operation held it); the cache gets
 * them at once. ERR_NO_MEMORY: no pages for the hold. */
status_t hold_put(const uint8_t *buff, uint64_t sector, uint32_t count);
/* buf holds count sectors at `sector` just read from the disk: the held
 * ones among them, newer than the disk's, copied over them. */
void     hold_overlay(uint64_t sector, uint32_t count, uint8_t *buf);
/* Everything held, out on the disk now; its failure sets kept->disk.hold_failed. */
status_t disk_release(void);
/* Sectors that went out held so far, and in how many block writes. */
void     disk_hold_stats(uint64_t *sectors, uint64_t *writes);

/* ---- cache.c ---------------------------------------------------------------------- */

struct fat_cache_stats {
    uint64_t hits;            /* reads served from a line */
    uint64_t fills;           /* lines read from the disk */
    uint64_t bypassed;        /* big reads that went straight to the disk */
    uint64_t updated;         /* sectors written that a line held (copied in) */
};

/* count sectors at `sector` (inside the partition) into buff, through
 * the cache. */
status_t cache_read(uint64_t sector, uint32_t count, uint8_t *buff);
/* count sectors at `sector` have been written to the disk with `data`:
 * the lines that hold any of them take the new bytes. */
void     cache_wrote(uint64_t sector, uint32_t count, const uint8_t *data);
/* count sectors at `sector` did not reach the disk after all (a held
 * write that failed): the lines that hold any of them go. */
void     cache_forget(uint64_t sector, uint32_t count);
void     cache_stats(struct fat_cache_stats *out);

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
/* A name or label read from the volume (NUL-terminated UTF-8), made safe to
 * show, in place: every control character (C0, DEL, and C1 as UTF-8)
 * becomes '?'. Another computer can write any name; one that holds them
 * can't be opened through fat anyway (path_resolve refuses them, and '?'
 * too), and on a terminal they would move the cursor or clear the screen. */
void     name_shown(char *s);
/* The generic FRESULT -> ERR_* mapping (the methods refine FR_DENIED). */
status_t fr_status(FRESULT r);
/* A FAT date and time (the clock's own zone) as seconds since 1970 counted
 * as if it were UTC; 0 for no date. */
uint64_t fat_unix_time(WORD date, WORD time);

/* ---- fsops.c --------------------------------------------------------------------- */

extern const struct fs_ops fat_fs_ops;

/* ---- main.c ---------------------------------------------------------------------- */

extern const struct fsctl_ops fat_ctl_ops;

/* ---- dirs.c ---------------------------------------------------------------------- */

/* Entry `index` of directory `path` (resolved) into *fi, as a walk from
 * its start would find it, from a cursor where one can go on.
 * ERR_NOT_FOUND: past the end; ERR_WRONG_TYPE: path is a file. */
status_t dirs_read(const char *path, uint32_t index, FILINFO *fi);
/* Close every cursor: before anything that adds, removes or renames an
 * entry. */
void     dirs_forget(void);
/* f_readdir calls so far (fsctl.stats). */
uint64_t dirs_entries_read(void);

/* ---- views.c --------------------------------------------------------------------- */

/* Serve ch (consumed, whatever happens) as a view with FS_VIEW_* flags
 * (fs_view_serve_one's `add`; host unused). ERR_NO_RESOURCES: FAT_VIEWS
 * of them already. */
status_t views_add(void *host, handle_t ch, uint32_t flags);
/* The port said a view's channel has news (key's slot and gen): serve it. */
void     views_event(uint64_t key);
/* Close the channel of every view the state doesn't know (after an undo:
 * views an undone fs.view made). */
void     views_drop_unknown(void);
/* A successor's views (adopt.c): keep_restore's slot i handed back (true:
 * the state knows it, the handle is taken); then every view the state
 * knows and got its channel back waited on again, the rest dropped (the
 * number kept is returned). Its channel made again, the client's end into
 * *out (consumed by the reply): the reply that handed out view i never
 * went out (its slot's mark). */
bool     views_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned views_adopt(void);
status_t views_remake(uint32_t i, handle_t *out);
/* View i's channel, as serve_one takes it, if the state has view i with
 * generation gen16 (its low 16 bits) and its channel came back. */
bool     views_chan(uint32_t i, uint32_t gen16, struct fat_chan *out);

/* ---- fileops.c ------------------------------------------------------------------- */

extern const struct file_ops fat_file_ops;   /* ctx: the file's struct fat_file */

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
/* Close the handles of every slot the state doesn't know (after an undo:
 * files an undone fs.open made). */
void     files_drop_unknown(void);
/* Is the file at `path` (resolved) open? Told by its directory entry, so
 * that an 8.3 alias of an open file's path is that file (FatFs's own lock
 * is off: ffport/ffconf.h). An open file can't be removed or renamed. */
bool     files_is_open(const char *path);
/* A successor's files, as views_take and the rest (adopt.c): files_adopt
 * closes (as operations) the files whose handles didn't come back. */
bool     files_take(uint32_t i, const handle_t *hs, unsigned n);
unsigned files_adopt(void);
status_t files_remake(uint32_t i, handle_t *out_ch, handle_t *out_vmo);
/* Slot i forgotten without FatFs (its close crashed fat twice): its file's
 * unsynced changes are lost, the slot and its handles go. */
void     files_forget(uint32_t i);
/* Slot i's channel and the file it is, as serve_one takes it (adopt.c);
 * false if the state has no such open file or its handles are gone. */
bool     files_chan(uint32_t i, uint32_t gen16, struct fat_chan *out);
