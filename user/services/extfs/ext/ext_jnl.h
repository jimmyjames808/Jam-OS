/* ext's journal: jbd2 (jbd2_disk.h) in the volume's journal inode, a
 * transaction per request (docs/M13.6-PLAN.md, "The disk is the state").
 * Pure library code like the rest of ext: no Jam OS calls, no allocation;
 * the caller gives it a region of memory (ext_jnl_init) and a struct
 * ext_jnl that lives as long as the mount.
 *
 * The model:
 *   - A transaction is every block dirty in the cache (ext_bdirty, the
 *     writers' "add a block") and every block revoked (ext_jnl_revoke)
 *     since the last commit. ext_jnl_commit writes it to the log (revoke
 *     blocks, descriptors and copies, then the commit block, FUA), then
 *     writes the blocks home (ext_sync: the checkpoint). A request is
 *     answered after its commit; once the commit block is on the disk the
 *     transaction survives anything: a cut, a kill, a failed checkpoint.
 *   - Two ways to commit. By default the log blocks are written, then the
 *     commit block in a write of its own, as Linux's jbd2 does: a cut can
 *     never leave a commit block whose transaction is torn, so Linux's
 *     replay (which reads a transaction's copies only as it replays them)
 *     is as safe as ours. EXT_JNL_ONE_WRITE writes them all in one write
 *     (jbd2's asynchronous commit): faster, safe for this library's replay
 *     (below), but a cut that lands the commit block without one of its
 *     copies is one Linux replays partly (it skips a copy whose checksum
 *     fails, and can't see some stale ones at all).
 *   - A device with a volatile cache (struct ext_dev's flush: a USB
 *     stick) lands its writes in any order until a flush, so the journal
 *     flushes where its order needs it: before the commit block (the file
 *     data and the copies first), after it (before anything goes home, and
 *     before the request is answered: an answered request is on the
 *     medium), before the tail moves past what was written home, and
 *     before a replayed log is marked empty. A device without one (FUA,
 *     drv/nvme) has no flush and costs nothing more.
 *   - The commit block carries a digest of its transaction (a Jam OS
 *     addition in h_chksum[1] to [3], words checksum v3 leaves unused and
 *     Linux never reads; its commit checksum covers them), checked by the
 *     replay before a transaction counts. jbd2's per-copy CRC32C can't
 *     do it alone: a block that ends in its own CRC32C (a 1 KiB volume's
 *     superblock, an extent node whose entries fill its block) gives the
 *     same tag checksum in every version of itself (CRC32C's residue), so
 *     a stale copy left by a torn write passes it. Commit blocks without
 *     the digest (Linux's) are taken on their tags.
 *   - The tail: the journal superblock's s_start moves past checkpointed
 *     transactions every EXT_JNL_TAIL_EVERY commits (tail_every), as soon
 *     as the log holds EXT_JNL_TAIL_BLOCKS blocks since it (tail_blocks:
 *     every commit but the smallest), when the log has no room for the
 *     next one, or when the caller asks (ext_jnl_checkpoint: the
 *     service's idle second), so a replay reads at most that many
 *     transactions and about that many blocks. A move is one write of the
 *     journal superblock; a replay is a read of every block twice and a
 *     write of every copy, so a service that is killed again and again
 *     (storm) must not find much to replay at each start.
 *   - The mount (ext_jnl_mount) is the recovery: ext_mount, then a replay
 *     of every whole transaction from the tail (a transaction any of whose
 *     blocks fails its checksum ends the log before it), then the journal
 *     marked empty, the volume mounted again over what the replay wrote,
 *     and the superblock's needs_recovery set for as long as it is mounted
 *     (Linux's convention: Linux replays a volume that has it, and a cut
 *     can leave the log holding anything). A restart and a power cut are
 *     the same path.
 *   - File data goes around the journal (ordered mode): the writers
 *     (ext_write) put it straight on the device, to blocks the committed
 *     bitmaps still call free or over the file's own blocks, before the
 *     commit that points at it, so a replay never needs it.
 *   - The freed-block hazard: a metadata block freed and then reused for
 *     file data must not be overwritten by a replay of an older
 *     transaction's copy of it. The journal remembers every block with a
 *     copy in the log (since the tail), and every free goes through
 *     ext_jnl_freed (ext_bfree calls it): a freed block with a copy gets a
 *     revoke record in the transaction, and the replay skips copies of it
 *     from that and every earlier transaction. Freed data blocks never
 *     had a copy, so a truncate of a huge file writes no revokes. A
 *     revoked block written again as metadata in the same transaction
 *     cancels its revoke (its new copy replays last).
 *   - Big operations go in steps: no transaction may carry more than a
 *     quarter of the log (nor more than the cache holds dirty), so a
 *     writer that changes many blocks asks ext_jnl_room and calls
 *     ext_jnl_step between steps that each leave the volume whole.
 *   - The request's slot (the service's state) records each transaction's
 *     id just before its commit is written (on_commit). After a mount,
 *     ext_jnl_committed says whether that id reached the disk: if so the
 *     request's effects are there (send its answer), if not none of them
 *     are (run it again). That answer holds for the first mount after the
 *     commit only: the mount gives later ids past an id the log never
 *     committed, so a mount after it would take the id for one before its
 *     end. on_end tells the caller that first id before the mount writes
 *     anything, and a slot holding it or a later one forgets its id then.
 *
 * Who calls what:
 *   - The service (U4): ext_jnl_init, ext_jnl_mount, then per request
 *     ext_jnl_begin ... ext_jnl_commit (or ext_jnl_abort on an error),
 *     ext_jnl_checkpoint when idle, ext_jnl_close at a clean unmount; sets
 *     on_commit and the commit time.
 *   - The writers (U2a, U2b): ext_bdirty as they change blocks (U1's
 *     cache), ext_jnl_freed for every block they free (ext_bfree does
 *     it), ext_jnl_room and ext_jnl_step inside an operation that may
 *     outgrow a transaction.
 *   - With a journal open, nothing else writes metadata: ext_sync is the
 *     checkpoint inside ext_jnl_commit, never called on its own.
 *
 * Errors: as ext.h's. ERR_BAD_STATE: no journal is open (the volume is
 * read-only, or has none), or a transaction was left open. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>
#include "ext.h"
#include "jbd2_disk.h"

#define EXT_JNL_RUNS       32   /* extents the journal's inode may have */
#define EXT_JNL_STAGE_MIN  4    /* blocks staged for the device, at least ... */
#define EXT_JNL_STAGE_MAX  64   /* ... and at most */
#define EXT_JNL_TAIL_EVERY 64   /* commits between the tail's moves (the plan's 64) ... */
#define EXT_JNL_TAIL_BLOCKS 8   /* ... or log blocks since it, if that comes first */

/* ext_jnl_init's flags */
#define EXT_JNL_ONE_WRITE  0x1  /* asynchronous commit: a transaction in one write (above) */

/* A run of the journal's blocks: log blocks [lblk, lblk + len) lie at
 * volume blocks [pblk, pblk + len). */
struct ext_jnl_run {
    uint64_t pblk;
    uint32_t lblk;
    uint32_t len;
};

/* A revoke record: block blk may not be replayed from a transaction whose
 * id is at or before tid. While a transaction runs the region holds its
 * revoked blocks as a list (tid unused); during a replay, a hash table. */
struct ext_jnl_revoke {
    uint64_t blk;
    uint32_t tid;
    uint32_t used;                   /* the hash table's slot is taken; in a transaction: cancelled */
};

struct ext_jnl {
    /* Set by the caller (ext_jnl_init gives the defaults). */
    uint32_t flags;                  /* EXT_JNL_* */
    uint32_t tail_every;             /* commits between tail moves */
    uint32_t tail_blocks;            /* log blocks since the tail that move it */
    void   (*on_commit)(void *ctx, uint32_t tid);   /* just before a commit is written */
    /* At the mount, once the log is read: `end` is the first id that was
     * not committed (ext_jnl_committed's answer for every id before the
     * mount). Called before anything is written, because the next ids go
     * past it (a replayed log is marked empty with the id after end): a
     * request slot holding end must be forgotten now, or a mount after
     * this one would count it committed. */
    void   (*on_end)(void *ctx, uint32_t end);
    void    *ctx;                    /* on_commit's and on_end's first argument */
    uint64_t commit_sec;             /* the time commit blocks carry (informational) */
    uint32_t commit_nsec;

    /* The region (ext_jnl_init), carved at the mount for the block size. */
    uint8_t *mem;
    size_t   mem_size;
    uint8_t *stage;                  /* nstage blocks: log blocks on their way to the device */
    uint32_t nstage;
    uint64_t *live;                  /* blocks with a copy in the log since the tail: a hash
                                      * set (EXT_NO_BLOCK: empty), twice the log's length */
    uint32_t cap_live;               /* a power of two */
    uint32_t nlive;
    struct ext_jnl_revoke *revokes;  /* the rest of the region */
    uint32_t cap_revokes;

    /* The journal: its inode's runs and its superblock. */
    struct ext_jnl_run runs[EXT_JNL_RUNS];
    uint32_t nruns;
    uint32_t maxlen;                 /* log blocks in the journal */
    uint32_t first;                  /* the circular log is [first, maxlen) */
    uint32_t seed;                   /* the checksums' seed: crc32c(~0, uuid) */
    uint32_t tag_bytes;              /* a tag's size (16 with checksum v3) */
    uint32_t rec_bytes;              /* a revoke record's (4, or 8 with 64bit) */
    uint32_t max_tx;                 /* log blocks one transaction may take: a quarter */
    struct jbd2_super sb;            /* the journal superblock, as on the disk (big-endian) */

    /* The log. Positions are log blocks; ids compare modulo 2^32. */
    uint32_t head;                   /* where the next transaction goes */
    uint32_t tid;                    /* the next transaction's id */
    uint32_t tail, tail_tid;         /* s_start and s_sequence on the disk; tail 0: empty */
    uint32_t clean, clean_tid;       /* every transaction before here is home */
    uint32_t since_tail;             /* commits since the tail moved */
    uint32_t mount_end;              /* the first id that was not committed at the mount */

    /* The running transaction. */
    uint32_t nrevokes;               /* blocks revoked in it (in revokes[]) */
    struct ext_super sb_before;      /* fs->sb as it was before it: ext_jnl_abort puts it back */

    /* What the mount's recovery did (for the log and the tests). */
    uint32_t replayed;               /* transactions replayed */
    uint32_t replayed_blocks;        /* copies written home */
    uint32_t revoked_blocks;         /* copies skipped for a revoke */
};

/* Bytes of region a journal of up to `journal_blocks` blocks needs on a
 * volume of block_size: the staging blocks, the set of blocks with a copy
 * in the log, and room for `revokes` revoke records (a replay holds every
 * record in the log; a transaction its own). */
size_t ext_jnl_mem_size(uint32_t block_size, uint32_t journal_blocks, uint32_t revokes);

/* Set j up with its region (kept until it is no longer used) and flags;
 * the defaults for the rest. ERR_INVALID_ARGS for an unknown flag. */
status_t ext_jnl_init(struct ext_jnl *j, void *mem, size_t mem_size, uint32_t flags);

/* Mount the volume and recover it: ext_mount (its arguments and errors),
 * then, if the volume may be written: the journal opened, every whole
 * transaction replayed, the journal marked empty, the volume mounted again
 * over what was replayed (fs->ro's EXT_RO_RECOVER cleared), the journal
 * superblock's features set for writing (checksum v3), needs_recovery set
 * on the disk, and fs->jnl = j. j->replayed says what the replay did.
 * A volume that may only be read is mounted and nothing is written (one
 * that needs recovery reads as it lies; fs->jnl stays NULL).
 * A damaged journal mounts the volume read-only (EXT_RO_DAMAGED and
 * fs->why: "journal: ..."), and still OK. ERR_NO_MEMORY: j's region
 * holds fewer than EXT_JNL_STAGE_MIN blocks and a few revoke records. */
status_t ext_jnl_mount(struct ext_fs *fs, struct ext_jnl *j, const struct ext_dev *dev,
                       void *mem, size_t mem_size, uint32_t flags);

/* A request starts: the cache must hold nothing dirty (ERR_BAD_STATE: the
 * last request neither committed nor aborted). *out_tid: the id its first
 * commit will have. */
status_t ext_jnl_begin(struct ext_fs *fs, uint32_t *out_tid);

/* Blocks the running transaction may still make dirty (0 when full): a
 * quarter of the log less what it holds and its descriptors, revoke and
 * commit blocks, and no more than the cache keeps clean for reading. */
uint32_t ext_jnl_room(const struct ext_fs *fs);

/* Blocks [blk, blk + count) the running transaction frees (data or
 * metadata): each with a copy in the log is revoked (ext_jnl_revoke), and
 * every one's dirty buffer forgotten. Call it after the last change to
 * them and before anything writes them as data. No journal open: OK,
 * nothing done (a volume mounted without one). ERR_NO_RESOURCES: the
 * region holds no more revoke records (commit with ext_jnl_step first). */
status_t ext_jnl_freed(struct ext_fs *fs, uint64_t blk, uint64_t count);

/* Revoke block blk in the running transaction, whether or not it has a
 * copy in the log: its replay from this and earlier transactions is
 * skipped, and its dirty buffer, if any, forgotten (a freed block's bytes
 * must never reach its home: it may hold file data before this commits).
 * ERR_NO_RESOURCES as ext_jnl_freed. */
status_t ext_jnl_revoke(struct ext_fs *fs, uint64_t blk);

/* Inside an operation in steps: if fewer than `need` blocks of room are
 * left, commit what is done so far and go on in a new transaction (the
 * volume must be whole at this point). ERR_NO_RESOURCES if `need` can never
 * fit in one transaction; else as ext_jnl_commit. */
status_t ext_jnl_step(struct ext_fs *fs, uint32_t need);

/* Commit the running transaction and write it home (above). Nothing dirty
 * and nothing revoked: OK, no id used.
 * ERR_ACCESS_DENIED: the volume may not be written. ERR_NO_RESOURCES: the
 * transaction is larger than a transaction may be (abort it).
 * ERR_IO before the commit block is on the disk: nothing of it counts;
 * the volume goes read-only (EXT_RO_DAMAGED). ERR_IO after (the
 * checkpoint failed): it is committed, so a replay brings it home, and
 * the volume goes read-only. */
status_t ext_jnl_commit(struct ext_fs *fs);

/* Give the running transaction up: its dirty blocks forgotten
 * (ext_discard), its revokes dropped, fs->sb as it was before it. */
void ext_jnl_abort(struct ext_fs *fs);

/* Move the tail past every checkpointed transaction (the service's idle
 * second). Nothing to move: OK, nothing written. */
status_t ext_jnl_checkpoint(struct ext_fs *fs);

/* Unmount cleanly: the journal marked empty and needs_recovery cleared on
 * the disk; fs->jnl = NULL. ERR_BAD_STATE with a transaction running. */
status_t ext_jnl_close(struct ext_fs *fs);

/* Was transaction `tid` committed (by this mount or before it)? For the
 * request slot's id after a mount: the replay counted every whole
 * transaction, and every id before the tail. */
bool ext_jnl_committed(const struct ext_fs *fs, uint32_t tid);

/* Ids compare modulo 2^32: is a after b? */
static inline bool ext_jnl_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}
