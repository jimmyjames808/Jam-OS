/* fat: the undo copy (docs/M11.6-PLAN.md, "The undo copy").
 *
 * Before an operation (a request, or the close of a file whose client has
 * gone: request.c) changes anything in the state, fat copies what it may
 * change into kept->undo, so that a successor that finds the operation
 * begun and not committed can put the state back as it was and run it
 * again from its slot. Nothing of an uncommitted operation is on the disk
 * (hold.c), so the state put back and the disk agree, and FatFs makes the
 * same decisions the second time.
 *
 * What is copied, by kind (enum fat_op):
 *   - always: FatFs's volume (FATFS: its window, free count, last cluster
 *     hint), and the hold's counts (held, runs);
 *   - a file request: that file's open (struct fat_open: its FIL and
 *     flags); FatFs touches no other file;
 *   - an fs or fsctl request: the tables of open slots and views (files[],
 *     views[]), and each open an operation changes, saved by the code
 *     that changes it (undo_open, fileops.c) the first time;
 *   - a close: that slot and its open.
 * The disk's facts (kept->disk: what the clean bit on the medium says, a
 * flush owed, a lost hold) are not copied: they say what the disk is, and
 * the operation's own disk I/O is either a read or the early dirty mark
 * (disk.c), which an undo can't take back and needn't.
 * A held sector an operation overwrites in place, if an earlier
 * (committed) one held it, is saved first (undo_sector); what an
 * operation appends to the hold is undone by putting back its counts, and
 * each run's length is counted again from what is left.
 *
 * Typical sizes: a file request 1.4 KiB (FATFS 576 bytes, an open 832);
 * an fs request 1.5 KiB (FATFS, the two tables 512 + 384), 2.3 KiB for an
 * open; a close 1.4 KiB; fs.sync or a stop up to 28 KiB (every open
 * file). The log holds FAT_UNDO_BYTES; a log that overflows (it can't
 * with today's sizes) spends the copy: the operation can't be undone.
 *
 * Validity. The copy is whole once `op` is set (the last store, released);
 * a later record is appended before the change it saves, and `used` moves
 * past it only after its bytes. The copy is for the operation in progress
 * while `op` is kept->ops_done + 1; the commit makes it stale with one
 * store: a request's is its slot's commit word (svcstate_commit), a
 * close's is kept->ops_done. A process dies between instructions, so each
 * word is either old or new (undo_pending).
 *
 * Built with -DFAT_UNDO_OFF (make EXTRA_CFLAGS_fat="-Ibuild/fatfs
 * -Iuser/services/fat/ffport -DFAT_UNDO_OFF") fat makes no copy: only for
 * measuring what the copy costs (docs/M11.6-PLAN.md, "Kill-to-first-answer"). */
#include "fat.h"

/* A record in the log: len bytes of the state at offset `off` from kept,
 * then the bytes, padded to 8. */
struct rec {
    uint32_t off;
    uint32_t len;
};

#define REC_MAX 64u   /* records in a log at most: 5 + every open */

static size_t pad8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

/* Append [p, p + len), a part of *kept, to the log; spend the copy if it
 * doesn't fit. */
static void save(const void *p, size_t len)
{
#ifdef FAT_UNDO_OFF
    (void)p;
    (void)len;
#else
    struct fat_undo *u = &kept->undo;
    if (u->spent)
        return;
    size_t need = sizeof(struct rec) + pad8(len);
    if (need > FAT_UNDO_BYTES - u->used) {
        printf("fat %s: an operation's undo copy didn't fit: it can't be undone\n", vol.name);
        undo_spend();
        return;
    }
    struct rec *r = (struct rec *)(u->log + u->used);
    r->off = (uint32_t)((const uint8_t *)p - (const uint8_t *)kept);
    r->len = (uint32_t)len;
    memcpy(r + 1, p, len);
    __atomic_store_n(&u->used, u->used + (uint32_t)need, __ATOMIC_RELEASE);
#endif
}

void undo_begin(enum fat_op kind, uint64_t op, uint64_t seq, const struct fat_file *f)
{
    struct fat_undo *u = &kept->undo;
    __atomic_store_n(&u->op, 0, __ATOMIC_RELEASE);   /* no copy while it is being made */
    u->seq = seq;
    u->closing = kind == FAT_OP_CLOSE ? (uint32_t)(f - kept->files) : 0;
    u->used = 0;
    u->opens = 0;
    u->held = kept->hold.held;
    u->runs = kept->hold.runs;
    u->nsect = 0;
    u->spent = false;
    save(&kept->fs, sizeof(kept->fs));
    if (kind == FAT_OP_FS) {
        save(kept->files, sizeof(kept->files));
        save(kept->views, sizeof(kept->views));
    } else if (kind == FAT_OP_CLOSE) {
        save(f, sizeof(*f));
    }
    __atomic_store_n(&u->op, op, __ATOMIC_RELEASE);   /* whole: from here it undoes */
    if (kind != FAT_OP_FS)
        undo_open(&kept->opens[f->open]);
}

void undo_open(const struct fat_open *o)
{
    struct fat_undo *u = &kept->undo;
    uint32_t bit = 1u << (unsigned)(o - kept->opens);
    if (!op_running() || (u->opens & bit))
        return;
    save(o, sizeof(*o));
    u->opens |= bit;
}

bool undo_sector(uint32_t i)
{
#ifdef FAT_UNDO_OFF
    (void)i;
    return true;
#else
    struct fat_undo *u = &kept->undo;
    if (u->spent)
        return true;   /* nothing to keep exact any more: overwrite in place */
    if (u->nsect == FAT_UNDO_SECTORS)
        return false;
    memcpy(u->sect[u->nsect], kept->hold.data + (size_t)i * FAT_SECTOR, FAT_SECTOR);
    u->sect_at[u->nsect] = i;
    __atomic_store_n(&u->nsect, u->nsect + 1, __ATOMIC_RELEASE);
    return true;
#endif
}

void undo_spend(void)
{
    __atomic_store_n(&kept->undo.spent, true, __ATOMIC_RELEASE);
}

bool undo_pending(void)
{
    const struct fat_undo *u = &kept->undo;
    uint64_t op = __atomic_load_n(&u->op, __ATOMIC_ACQUIRE);
    if (!op || op != kept->ops_done + 1)
        return false;
    return !u->seq || state_slots()->h->commit < u->seq;
}

/* The hold as it was: its counts back, the sectors overwritten in place
 * back (the newest save last, so the oldest bytes win), each run's length
 * counted again from the sectors left in it. */
static void restore_hold(const struct fat_undo *u)
{
    struct fat_hold *h = &kept->hold;
    for (uint32_t k = u->nsect; k-- > 0;)
        memcpy(h->data + (size_t)u->sect_at[k] * FAT_SECTOR, u->sect[k], FAT_SECTOR);
    h->held = u->held;
    h->runs = u->runs;
    for (uint32_t r = 0; r < h->runs; r++)
        h->run_len[r] = 0;
    for (uint32_t i = 0; i < h->held; i++)
        h->run_len[h->run_of[i]]++;
}

bool undo_restore(void)
{
    const struct fat_undo *u = &kept->undo;
    if (!undo_pending() || u->spent || u->used > FAT_UNDO_BYTES || u->held > FAT_HOLD_MAX ||
        u->runs > FAT_HOLD_RUNS || u->nsect > FAT_UNDO_SECTORS)
        return false;
    for (uint32_t k = 0; k < u->nsect; k++)
        if (u->sect_at[k] >= u->held)
            return false;   /* only committed sectors are saved */
    for (uint32_t i = 0; i < u->held; i++)
        if (kept->hold.run_of[i] >= u->runs)
            return false;   /* a committed sector in a run begun later */
    /* The records, last first, so that the first copy of anything wins. */
    uint32_t at[REC_MAX], n = 0;
    for (uint32_t off = 0; off < u->used;) {
        const struct rec *r = (const struct rec *)(u->log + off);
        if (n == REC_MAX || u->used - off < sizeof(*r) || r->len > sizeof(struct fat_state) ||
            r->off > sizeof(struct fat_state) - r->len ||
            sizeof(*r) + pad8(r->len) > u->used - off)
            return false;   /* not a copy fat made */
        at[n++] = off;
        off += (uint32_t)(sizeof(*r) + pad8(r->len));
    }
    while (n-- > 0) {
        const struct rec *r = (const struct rec *)(u->log + at[n]);
        memcpy((uint8_t *)kept + r->off, r + 1, r->len);
    }
    restore_hold(u);
    memset(&kept->post, 0, sizeof(kept->post));
    kept_cancel();   /* slots it closed are the state's again */
    cache_forget(0, UINT32_MAX);   /* FatFs's sectors are 32 bits */
    dirs_forget();
    return true;
}
