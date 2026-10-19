/* extfs: every request run as one journal transaction (docs/M13.6-PLAN.md,
 * "The disk is the state").
 *
 * Every request extfs serves (fs on the mount's channels, a view or a
 * directory channel, file, fsctl, fswatch) is read into a slot of the
 * state VMO (svcstate_prepare, then the protocol's generated
 * <proto>_take_slot; fs's with svcstate_read, which keeps the writer's
 * pid) and run from there, in four steps:
 *   1. begin: the journal's transaction (ext_jnl_begin), and the slot's
 *      record of it emptied (kept->slot: no transaction id yet);
 *   2. run, with its generated <proto>_run_slot (fs_view_dispatch for fs,
 *      which checks the view's flags first): its changes collect in the
 *      block cache, file data goes to the disk around it (ordered mode:
 *      to blocks the committed bitmaps call free, or over the file's
 *      own); its reply into the slot's reply area;
 *   3. commit: an answer that is OK commits the transaction (one write of
 *      the log, its commit block, then the blocks home: ext_jnl_commit);
 *      just before the commit block goes, the transaction's id is put in
 *      the slot's record (on_commit). Any other answer gives the
 *      transaction up (ext_jnl_abort): nothing of it reaches the disk.
 *      Then the slot is committed (svcstate_commit: the reply's length and
 *      the commit word);
 *   4. answer: the slot's reply waits for extfs's next system call
 *      (svcstate_answer), which sends it and has the kernel mark the slot.
 *
 * A successor (adopt.c) finds the request in progress in its slot: not
 * committed in the slot but its transaction id in the record and the
 * journal saying that id committed (ext_jnl_committed, after the replay):
 * it is done, and the reply in the slot is its answer; no id, or one the
 * journal never committed: nothing of it is on the disk but data in free
 * blocks and overwrites a re-run writes again from the slot's bounce
 * buffer (step 2's data, copied in once): run it again. No undo copy and
 * no held writes: the journal is the one recovery, and a power cut takes
 * the same path.
 *
 * One transaction per request. What may be too big for one (a truncate of
 * a big file, an orphan whose blocks are many) does its first step in the
 * request, with the inode on the on-disk orphan list, and the rest after
 * the answer (defer_run), each step a transaction of its own; a death
 * meanwhile is finished by the next mount (the orphan list: adopt.c's
 * recover_orphans). defer_run finishes before the next request is read,
 * so no request sees a file half cut.
 *
 * A metadata error (a checksum that fails, a structure out of range: the
 * library's ERR_IO with the volume marked damaged) gives the transaction
 * up and turns the volume read-only, its superblock marked as having
 * errors (ext4's errors=remount-ro): went_bad. */
#include <wallclock.h>
#include "extfs.h"

#define FIXED_SECONDS 1767225600   /* 2026-01-01 00:00:00 UTC (test.c's fixed-time) */

static struct {
    bool     running;   /* a request's transaction is open */
    bool     final;     /* its commit is being written: on_commit records it */
    uint64_t seq;
    unsigned slot;
    uint32_t tid;       /* the journal's next id when it began ... */
    uint64_t writes;    /* ... and the disk's writes */
} op;

/* ---- transactions ------------------------------------------------------------------ */

bool writable(void)
{
    return vol.mounted && vol.fs.jnl && !vol.fs.ro && !vol.disk_gone;
}

void time_now(struct ext_time *t)
{
    int64_t utc = FIXED_SECONDS;
    uint32_t ns = 0;
    if (!test_fixed_time()) {
        (void)clock_now(&utc, NULL);   /* not set: still the best there is */
        ns = (uint32_t)(now() % NS_PER_S);
    }
    *t = (struct ext_time){ .sec = utc, .nsec = ns };
}

uint32_t new_generation(void)
{
    return os_random_u32();
}

static bool damaged(void)
{
    return vol.fs.ro & EXT_RO_DAMAGED;
}

void went_bad(const char *what)
{
    struct ext_fs *fs = &vol.fs;
    if (vol.marked || vol.disk_gone)
        return;
    vol.marked = true;
    printf("extfs %s: %s: %s (inode %u, block %lu): the volume is read-only from now on\n",
           vol.name, what, fs->why ? fs->why : "a disk error", (unsigned)fs->why_ino,
           (unsigned long)fs->why_blk);
    /* The error state on the disk, so the next mount reads it read-only
     * too (until e2fsck): through the journal like any change, the volume
     * let write for that one transaction. Not when the journal itself
     * failed, nor for a disk error (no `why`). */
    if (!fs->jnl || !fs->why || !strncmp(fs->why, "journal", 7))
        return;
    uint32_t ro = fs->ro;
    ext_jnl_abort(fs);
    fs->ro = 0;
    status_t st = ext_jnl_begin(fs, NULL);
    if (st == OK) {
        fs->sb.state |= EXT_STATE_ERROR;
        st = ext_super_write(fs);
    }
    if (st == OK)
        st = ext_jnl_commit(fs);
    if (st != OK) {
        ext_jnl_abort(fs);
        printf("extfs %s: the error state couldn't be written: %s\n", vol.name,
               status_str(st));
    }
    fs->ro = ro | EXT_RO_ERRORS;
}

status_t tx_begin(void)
{
    if (!writable())
        return ERR_ACCESS_DENIED;
    status_t st = ext_jnl_begin(&vol.fs, NULL);
    if (st == ERR_BAD_STATE) {   /* something left dirty: given up, then begun */
        ext_jnl_abort(&vol.fs);
        st = ext_jnl_begin(&vol.fs, NULL);
    }
    return st;
}

void tx_abort(void)
{
    if (vol.fs.jnl)
        ext_jnl_abort(&vol.fs);
    else
        ext_discard(&vol.fs);
}

status_t tx_commit(void)
{
    status_t st = vol.fs.jnl ? ext_jnl_commit(&vol.fs) : ERR_ACCESS_DENIED;
    if (st != OK) {
        tx_abort();
        if (damaged())
            went_bad("a commit");
    }
    return st;
}

/* Just before a commit block is written (ext_jnl's on_commit). */
static void on_commit(void *ctx, uint32_t tid)
{
    (void)ctx;
    if (!op.final)
        return;   /* a transaction of no request's */
    struct xf_slot *x = &kept->slot[op.slot & 1];
    x->tid = tid;
    __atomic_store_n(&x->has_tid, 1u, __ATOMIC_RELEASE);
}

/* ---- the work after an answer ------------------------------------------------------- */

enum { DEFER_DROP = 1, DEFER_TRUNC, DEFER_TRUNC_OWN };
static struct {
    uint32_t ino;
    uint32_t kind;
} defers[XF_DEFER_MAX];
static unsigned ndefers;
static bool defer_full_said;

static void defer(uint32_t ino, uint32_t kind)
{
    for (unsigned i = 0; i < ndefers; i++)
        if (defers[i].ino == ino && defers[i].kind == kind)
            return;
    if (ndefers == XF_DEFER_MAX) {   /* on the orphan list: the next mount finishes it */
        if (!defer_full_said)
            printf("extfs %s: too much left for after the answers: the next mount finishes "
                   "it\n", vol.name);
        defer_full_said = true;
        return;
    }
    defers[ndefers++] = (typeof(defers[0])){ .ino = ino, .kind = kind };
}

void defer_drop(uint32_t ino)
{
    defer(ino, DEFER_DROP);
}

void defer_trunc(uint32_t ino, bool own)
{
    defer(ino, own ? DEFER_TRUNC_OWN : DEFER_TRUNC);
}

/* One step of deferred work on ino: *more if it isn't finished. */
static status_t defer_step(uint32_t ino, uint32_t kind, bool *more)
{
    struct ext_fs *fs = &vol.fs;
    struct ext_time now;
    time_now(&now);
    *more = false;
    if (kind == DEFER_DROP) {
        status_t st = ext_orphan_drop(fs, ino, &now);
        *more = st == ERR_SHOULD_WAIT;
        return *more ? OK : st;
    }
    struct ext_inode in;
    status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
    bool done = false;
    if (st == OK)
        st = ext_truncate(fs, &in, ext_isize(&in), &done);
    if (st == OK && done && kind == DEFER_TRUNC_OWN && in.d.links_count)
        st = ext_orphan_remove(fs, ino);
    *more = st == OK && !done;
    return st;
}

/* Orphan ino goes now: its watches hear of it (IN_DELETE_SELF, IN_IGNORED). */
static void gone(uint32_t ino)
{
    struct ext_inode in;
    if (watching() && ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, &in) == OK &&
        !in.d.links_count) {
        const struct fsw_key key = xf_key(&in);
        ev_gone(&key);
    }
}

void defer_run(void)
{
    while (ndefers) {
        uint32_t ino = defers[0].ino, kind = defers[0].kind;
        memmove(defers, defers + 1, --ndefers * sizeof(defers[0]));
        if (kind == DEFER_DROP && files_held(ino))
            continue;   /* open or mapped: its last close or unmap drops it */
        if (kind == DEFER_DROP)
            gone(ino);
        bool more = true;
        status_t st = OK;
        while (more && st == OK) {
            st = tx_begin();
            if (st == OK)
                st = defer_step(ino, kind, &more);
            if (st == OK)
                st = tx_commit();
            else
                tx_abort();
        }
        if (st != OK) {
            if (damaged())
                went_bad("an orphan");
            if (!vol.disk_gone)
                printf("extfs %s: inode %u left on the orphan list (%s): the next mount "
                       "finishes it\n", vol.name, (unsigned)ino, status_str(st));
        }
    }
}

/* ---- a request in a slot ------------------------------------------------------------- */

static struct idl_reply reply;

uint8_t *op_bounce(void)
{
    return (uint8_t *)svcstate_request(state_slots(), op.slot, NULL) + XF_SLOT_DATA;
}

bool op_data_in(void)
{
    return op.seq && kept->data_seq[op.slot & 1] == op.seq;
}

void op_data_copied(void)
{
    __atomic_store_n(&kept->data_seq[op.slot & 1], op.seq, __ATOMIC_RELEASE);
}

void op_made(uint32_t kind, uint32_t index)
{
    if (op.running && op.seq)
        kept->made = (struct xf_made){ .seq = op.seq, .kind = kind, .index = index };
}

uint32_t op_pid(void)
{
    return op.running ? svcstate_pid(state_slots(), op.slot) : 0;
}

static status_t take(const struct xf_chan *c, unsigned slot, const struct idl_slot *is)
{
    if (c->proto == XF_PROTO_CTL)
        return fsctl_take_slot(c->ch, is, &reply);
    if (c->proto == XF_PROTO_WATCH)
        return fswatch_take_slot(c->ch, is, &reply);
    /* fs and file: read keeping the writer's pid (fs.watch's per-process
     * share, a POSIX lock's owner: locks.c), which a read that sends the
     * waiting reply can't ask for: that reply on its own first, if one
     * waits (seldom: the port's wait sent it). */
    reply_flush();
    return idl_taken(c->ch, is, svcstate_read(state_slots(), slot, c->ch));
}

void reply_flush(void)
{
    (void)idl_reply_flush(&reply);
}

status_t xf_wait(uint64_t deadline, struct port_packet *pkt)
{
    return idl_wait_after(vol.port, deadline, pkt, &reply);
}

static uint32_t dispatch(const struct xf_chan *c, unsigned slot, handle_t *rhs, uint32_t *rhn)
{
    struct idl_slot is;
    svcstate_slot(state_slots(), slot, &is);
    if (c->proto == XF_PROTO_FILE)
        return file_run_slot(c->ch, &is, &xf_file_ops, c->file, rhs, rhn);
    if (c->proto == XF_PROTO_CTL)
        return fsctl_run_slot(c->ch, &is, &xf_ctl_ops, NULL, rhs, rhn);
    if (c->proto == XF_PROTO_WATCH)
        return fswatch_run_slot(c->ch, &is, &xf_watch_ops, c->watch, rhs, rhn);
    struct xf_ctx *ctx = (struct xf_ctx *)&c->ctx;
    const struct fs_view_server v = {
        .flags = views_lexical(ctx), .ops = &xf_fs_ops, .ctx = ctx,
        .add = ctx->dir_ino ? dirs_no_view : views_add, .host = ctx,
    };
    return fs_view_dispatch(&v, is.q, *is.n, is.r, rhs, rhn);
}

void answer(const struct xf_chan *c, unsigned slot, uint32_t rn, handle_t *rhs, uint32_t rhn)
{
    struct svcstate *s = state_slots();
    if (!rn) {
        svcstate_sent(s, slot);
        idl_close_all(rhs, rhn);
        return;
    }
    reply_flush();
    svcstate_answer(s, slot, c->ch, rhs, rhn, &reply);
}

void answer_status(const struct xf_chan *c, unsigned slot, status_t status)
{
    struct svcstate *s = state_slots();
    const struct idl_req_hdr *q = svcstate_request(s, slot, NULL);
    struct idl_rep_hdr *r = svcstate_reply_area(s, slot);
    r->txid = q->txid;
    r->status = status;
    (void)svcstate_commit(s, slot, sizeof(*r));
    if (c)
        answer(c, slot, sizeof(*r), NULL, 0);
    else
        svcstate_sent(s, slot);
}

/* The slot made by the running request goes (its reply failed). */
static void unmake(void)
{
    const struct xf_made *m = &kept->made;
    if (m->seq != op.seq)
        return;
    if (m->kind == XF_MADE_FILE)
        files_release(m->index);
    else if (m->kind == XF_MADE_VIEW)
        views_drop(m->index);
    else if (m->kind == XF_MADE_MAP)
        maps_unmake(m->index);
    else if (m->kind == XF_MADE_DIR)
        dirs_drop(m->index);
}

/* Step 1. */
static void begin(uint64_t seq, unsigned slot)
{
    struct xf_slot *x = &kept->slot[slot & 1];
    __atomic_store_n(&x->has_tid, 0u, __ATOMIC_RELEASE);
    x->seq = seq;
    op = (typeof(op)){ .running = true, .seq = seq, .slot = slot, .tid = vol.jnl.tid,
                       .writes = disk_writes() };
    ev_begin();
    if (writable() && tx_begin() != OK)
        printf("extfs %s: a transaction couldn't begin\n", vol.name);
}

/* Step 3's journal half: the reply (rn bytes in the slot) decides. Returns
 * the reply's length, changed if the commit failed. */
static uint32_t commit(unsigned slot, uint32_t rn, uint32_t ro_before, handle_t *rhs,
                       uint32_t *rhn)
{
    struct svcstate *s = state_slots();
    struct idl_rep_hdr *h = svcstate_reply_area(s, slot);
    bool ok = rn >= sizeof(*h) && h->status == OK;
    struct ext_fs *fs = &vol.fs;
    status_t st = OK;
    if (ok && writable()) {
        kept->slot[slot & 1].rn = rn;
        op.final = true;
        st = ext_jnl_commit(fs);
        op.final = false;
        if (st != OK)
            tx_abort();
        else if (kept->slot[slot & 1].has_tid)
            test_die(XF_DIE_JOURNAL);
        /* An answered request is on the medium: a commit flushed its data
         * and itself; one that wrote and committed nothing flushes here (a
         * disk with a cache, disk.c). */
        if (st == OK && vol.jnl.tid == op.tid && disk_writes() != op.writes)
            st = disk_flush();
    } else {
        tx_abort();
    }
    op.running = false;
    maps_settle(ok && st == OK);   /* given up: the mapped files' sizes as the disk has them */
    ev_settle(ok && st == OK);     /* the request's events: queued now, or never */
    if (damaged() && !(ro_before & EXT_RO_DAMAGED))
        went_bad("a request");
    if (st == OK)
        return rn;
    /* Not on the disk: the answer is the failure, and what it made goes. */
    unmake();
    idl_close_all(rhs, *rhn);
    *rhn = 0;
    h->status = st == ERR_NO_RESOURCES ? ERR_NO_RESOURCES : ERR_IO;
    return sizeof(*h);
}

/* A request that carried handles: refused, as the generated servers do. */
static void refuse(const struct xf_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    idl_close_all(s->handles, s->h->slot[slot].nhandles);
    answer_status(c, slot, ERR_INVALID_ARGS);
}

status_t serve_one(const struct xf_chan *c)
{
    struct svcstate *s = state_slots();
    unsigned slot = 0;
    struct idl_slot is;
    svcstate_prepare(s, c->id, &slot, &is);
    status_t st = take(c, slot, &is);
    if (!svcstate_taken(s, slot) || st != OK)
        return st;
    if (s->h->slot[slot].nhandles) {
        refuse(c, slot);
        return OK;
    }
    serve_slot(c, slot);
    return OK;
}

void serve_slot(const struct xf_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0, ro_before = vol.fs.ro;
    begin(s->h->slot[slot].seq, slot);
    uint32_t rn = dispatch(c, slot, rhs, &rhn);
    rn = commit(slot, rn, ro_before, rhs, &rhn);
    (void)svcstate_commit(s, slot, rn);   /* the dispatch's reply fits rep_cap */
    test_die(XF_DIE_COMMIT);
    answer(c, slot, rn, rhs, rhn);
    test_die(XF_DIE_ANSWERED);
    if (ndefers) {
        reply_flush();   /* the answer first: what is left is no client's wait */
        defer_run();
    }
}

/* The mount read the log (ext_jnl's on_end): a slot whose id is `end` or
 * later never committed. Forgotten now, before the mount writes the next
 * id past it: an instance that died after this mount and before the
 * request ran again would otherwise leave its successor counting it
 * committed (an id before that mount's end) and answering it from the
 * slot, its changes nowhere on the disk. */
static void on_end(void *ctx, uint32_t end)
{
    (void)ctx;
    for (unsigned i = 0; i < 2; i++) {
        struct xf_slot *x = &kept->slot[i];
        if (__atomic_load_n(&x->has_tid, __ATOMIC_ACQUIRE) && !ext_jnl_after(end, x->tid))
            __atomic_store_n(&x->has_tid, 0u, __ATOMIC_RELEASE);
    }
}

void request_init(void)
{
    vol.jnl.on_commit = on_commit;
    vol.jnl.on_end = on_end;
}
