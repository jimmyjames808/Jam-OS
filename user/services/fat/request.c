/* fat: every request run as one operation (docs/M11.6-PLAN.md, "The
 * request in progress" and "Held writes become the write-ahead buffer").
 *
 * Every request fat serves (fs on the mount's channel or a view, file,
 * fsctl) is read into a slot of the state VMO (svcstate_prepare, then the
 * protocol's generated <proto>_take_slot) and run from there with its
 * generated <proto>_run_slot (fs_view_dispatch for fs, which checks the
 * view's flags first), in five steps:
 *   1. begin: if the hold lacks room for a whole request, what it holds
 *      (all of it committed) goes out first; then the undo copy (undo.c);
 *   2. run, with every disk write held (hold.c): nothing of it reaches the
 *      disk, and what it asks of the disk afterwards (a flush, the clean
 *      mark) is noted in kept->post, with whether what it held must go out
 *      (anything but an FS_GATHER file's writes);
 *   3. commit: the reply into the slot and the commit word, one store
 *      (svcstate_commit), which also makes the undo copy stale; from here
 *      the request counts as done;
 *   4. send: kept->post, done (disk_release, disk_flush, disk_settle);
 *   5. answer: the slot's reply, made to wait for fat's next system call
 *      (svcstate_answer): the port wait that follows (fat_wait) or the
 *      next take sends it (channel_reply_wait), and the kernel sets the
 *      slot's mark once it went out, so a successor knows whether it did.
 *      A request costs fat three system calls: the port wait (which sends
 *      the last reply), the take, and the port binding again.
 * A send that fails turns a successful reply into the failure (its
 * handles are closed: the client never gets them), as a write that failed
 * on the spot would have been answered.
 *
 * The close of a file whose client has gone (fileops.c) is an operation
 * too, with no slot and no reply: its commit word is kept->ops_done, which
 * every operation sets (released) once it is committed. So a successor
 * finds, for the operation in progress, either its undo copy valid
 * (undo_pending: memory goes back, and the request runs again from its
 * slot, or the close happens again, its client still gone) or not (it
 * committed: the send is done again, harmless, then the reply).
 *
 * The bounce buffer. The slot's request area holds the request, then, a
 * page in (FAT_SLOT_DATA), its data: a file.write's bytes are copied there
 * from the client's transfer buffer before FatFs sees them, so a re-run
 * writes the same bytes even if the client has changed its buffer since,
 * and a file.read's go from FatFs to there and on to the client. That is
 * why a slot is a little over 64 KiB (state.c): one buffer, the one copy it
 * always was, now in the state.
 *
 * A successor finds the request in progress in its slot, and the undo
 * copy, and finishes it exactly once (adopt.c, with serve_slot and
 * answer_status here).
 *
 * Test powers (test.c) end the process at each step: a held write, the
 * commit, the send, before and after the answer. */
#include <fsview.h>
#include "fat.h"

enum op_phase {
    OP_IDLE,       /* no operation */
    OP_RUNNING,    /* begun, not committed: disk writes are held */
    OP_SENDING,    /* committed: what it held is going out */
};

/* The operation in progress: what fat itself needs of it (what a successor
 * needs is in the state: the slot, kept->undo, kept->post). */
static struct {
    enum op_phase phase;
    uint64_t      num;      /* its number: kept->ops_done + 1 */
    uint64_t      seq;      /* its request's number (0: a close) */
    unsigned      slot;     /* its request's slot */
    uint32_t      first;    /* hold index of its first sector */
    bool          gather;   /* what it holds may stay held */
    bool          wrote;    /* it held a sector */
    bool          stepped;  /* the hold filled: logged */
} op;

bool op_running(void)
{
    return op.phase == OP_RUNNING;
}

void op_gather(void)
{
    op.gather = true;
}

uint8_t *op_bounce(void)
{
    return (uint8_t *)svcstate_request(state_slots(), op.slot, NULL) + FAT_SLOT_DATA;
}

bool op_data_in(void)
{
    return op.seq && kept->data_seq[op.slot & 1] == op.seq;
}

void op_data_copied(void)
{
    __atomic_store_n(&kept->data_seq[op.slot & 1], op.seq, __ATOMIC_RELEASE);
}

uint32_t op_hold_first(void)
{
    return op.first;
}

void op_wrote(void)
{
    op.wrote = true;
}

void op_made(uint32_t kind, uint32_t index)
{
    if (op.phase == OP_RUNNING && op.seq)
        kept->made = (struct fat_made){ .seq = op.seq, .kind = kind, .index = index };
}

void op_steps(void)
{
    if (!op.stepped)
        printf("fat %s: a request's writes don't fit the hold: they go out in steps (a death "
               "meanwhile can leave clusters no file reaches)\n", vol.name);
    op.stepped = true;
    op.first = 0;   /* everything held goes out now: what comes after is this operation's */
    undo_spend();
}

/* Steps 1 and 2's start. */
static void begin(enum fat_op kind, uint64_t seq, const struct fat_file *f, unsigned slot)
{
    (void)hold_make_room();   /* a failure is logged (hold.c); later writes are refused */
    op.num = kept->ops_done + 1;
    op.seq = seq;
    op.slot = slot;
    op.first = kept->hold.held;
    op.gather = op.wrote = op.stepped = false;
    op.phase = OP_RUNNING;
    undo_begin(kind, op.num, seq, f);
}

/* What the send must do: what was held goes out unless it may stay. Set
 * before the commit, so a successor that finds the commit finds it too. */
static void plan_send(void)
{
    if (op.wrote && !op.gather)
        kept->post.release = true;
}

/* Step 3's end, for every operation: no longer running. */
static void committed(void)
{
    __atomic_store_n(&kept->ops_done, op.num, __ATOMIC_RELEASE);
    op.phase = OP_SENDING;
}

/* Step 4. */
static status_t send(void)
{
    struct fat_post *p = &kept->post;
    status_t st = p->release ? disk_release() : OK;
    if (st == OK && p->flush)
        st = disk_flush();
    if (st == OK && p->settle)
        st = disk_settle(p->settle == FAT_SETTLE_SYNC);
    memset(p, 0, sizeof(*p));
    op.phase = OP_IDLE;
    kept_flush();   /* the slots it closed, now that the state has forgotten them */
    return st;
}

status_t op_resend(void)
{
    return send();
}

void op_close_begin(const struct fat_file *f)
{
    begin(FAT_OP_CLOSE, 0, f, 0);
}

void op_close_end(void)
{
    plan_send();
    committed();   /* a close's commit word */
    kept->close_crashes = 0;
    test_die(FAT_DIE_COMMIT);
    status_t st = send();
    if (st != OK && !vol.disk_gone)
        printf("fat %s: closing a file: its writes: %s\n", vol.name, status_str(st));
}

/* ---- a request in a slot ------------------------------------------------------------ */

/* The reply of the request answered last, waiting for fat's next system
 * call that can carry it: a take, or the port wait. One at a time. */
static struct idl_reply reply;

/* The next message on c into the slot set up as is, by c's protocol's
 * generated take (<idl/common.h> idl_take: one too big for the slot is
 * answered ERR_INVALID_ARGS, one without a txid dropped), the waiting
 * reply sent first in the same system call. */
static status_t take(const struct fat_chan *c, const struct idl_slot *is)
{
    if (c->proto == FAT_PROTO_FILE)
        return file_take_slot(c->ch, is, &reply);
    if (c->proto == FAT_PROTO_CTL)
        return fsctl_take_slot(c->ch, is, &reply);
    return fs_take_slot(c->ch, is, &reply);
}

void reply_flush(void)
{
    (void)idl_reply_flush(&reply);   /* whether more is queued: the port says */
}

status_t fat_wait(struct port_packet *pkt)
{
    return idl_wait_after(vol.port, DEADLINE_NEVER, pkt, &reply);
}

/* Run the request in `slot` with c's protocol's generated run (fs: through
 * the view's checks, fs_view_dispatch), its reply into the slot. */
static uint32_t dispatch(const struct fat_chan *c, unsigned slot, handle_t *rhs, uint32_t *rhn)
{
    struct idl_slot is;
    svcstate_slot(state_slots(), slot, &is);
    if (c->proto == FAT_PROTO_FILE)
        return file_run_slot(c->ch, &is, &fat_file_ops, c->file, rhs, rhn);
    if (c->proto == FAT_PROTO_CTL)
        return fsctl_run_slot(c->ch, &is, &fat_ctl_ops, NULL, rhs, rhn);
    const struct fs_view_server v = {
        .flags = c->flags, .ops = &fat_fs_ops, .add = views_add,
    };
    return fs_view_dispatch(&v, is.q, *is.n, is.r, rhs, rhn);
}

/* The send failed: a successful reply becomes the failure (committed
 * again: the commit word stays, the reply's length changes). */
static void reply_failed(unsigned slot, uint32_t *rn, status_t st, handle_t *rhs, uint32_t *rhn)
{
    struct svcstate *s = state_slots();
    struct idl_rep_hdr *h = svcstate_reply_area(s, slot);
    if (*rn < sizeof(*h) || h->status != OK)
        return;
    h->status = st;
    *rn = sizeof(*h);
    idl_close_all(rhs, *rhn);
    *rhn = 0;
    (void)svcstate_commit(s, slot, *rn);   /* within rep_cap */
}

void answer(const struct fat_chan *c, unsigned slot, uint32_t rn, handle_t *rhs, uint32_t rhn)
{
    struct svcstate *s = state_slots();
    if (!rn) {   /* no txid: nothing to answer */
        svcstate_sent(s, slot);
        idl_close_all(rhs, rhn);
        return;
    }
    reply_flush();   /* one waits at a time: a take sent the last one already */
    svcstate_answer(s, slot, c->ch, rhs, rhn, &reply);
}

void answer_status(const struct fat_chan *c, unsigned slot, status_t status)
{
    struct svcstate *s = state_slots();
    const struct idl_req_hdr *q = svcstate_request(s, slot, NULL);
    struct idl_rep_hdr *r = svcstate_reply_area(s, slot);
    r->txid = q->txid;
    r->status = status;
    (void)svcstate_commit(s, slot, sizeof(*r));   /* within rep_cap */
    if (c)
        answer(c, slot, sizeof(*r), NULL, 0);
    else
        svcstate_sent(s, slot);   /* its caller is gone */
}

/* A request that carried handles: refused, as the generated servers do. */
static void refuse(const struct fat_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    idl_close_all(s->handles, s->h->slot[slot].nhandles);
    answer_status(c, slot, ERR_INVALID_ARGS);
}

static enum fat_op kind_of(const struct fat_chan *c)
{
    return c->proto == FAT_PROTO_FILE ? FAT_OP_FILE : FAT_OP_FS;
}

#ifdef FAT_RERUN_CHECK
/* A test build's check of the undo copy (make EXTRA_CFLAGS_fat="-Ibuild/fatfs
 * -Iuser/services/fat/ffport -DFAT_RERUN_CHECK"): every request is run,
 * undone (undo_restore, and the handles an undone open or view made
 * closed) and run again from its slot, before its commit, as a successor
 * runs a request it finds uncommitted. The second run must answer as the
 * first did and leave as much held and the same send to do; a difference
 * is logged ("rerun check FAILED"). The client gets the second run's
 * answer. A close (op_close_again) is undone and run again the same way. A
 * request that went out in steps can't be undone: not checked. The hold
 * is compared byte by byte too: every held sector's place and bytes,
 * those of earlier requests it overwrote in place included. */
static struct {
    uint64_t lba[FAT_HOLD_MAX];
    uint8_t  data[FAT_HOLD_MAX * FAT_SECTOR];
} first_hold;

static void hold_snapshot(void)
{
    const struct fat_hold *h = &kept->hold;
    memcpy(first_hold.lba, h->lba, h->held * sizeof(h->lba[0]));
    memcpy(first_hold.data, h->data, (size_t)h->held * FAT_SECTOR);
}

/* The first held sector that differs from the snapshot of `held` sectors,
 * or `held` if none does. */
static uint32_t hold_differs(uint32_t held)
{
    const struct fat_hold *h = &kept->hold;
    for (uint32_t i = 0; i < held && i < h->held; i++)
        if (h->lba[i] != first_hold.lba[i] ||
            memcmp(h->data + (size_t)i * FAT_SECTOR, first_hold.data + (size_t)i * FAT_SECTOR,
                   FAT_SECTOR))
            return i;
    return held;
}

static uint32_t rerun(const struct fat_chan *c, const void *req, uint32_t rn, handle_t *rhs,
                      uint32_t *rhn)
{
    static uint8_t first[FS_REP_MAX];   /* the biggest reply fat gives */
    static uint64_t checked;
    uint8_t *rep = svcstate_reply_area(state_slots(), op.slot);
    if (op.stepped || rn > sizeof(first))
        return rn;
    if (!checked++)
        printf("fat %s: rerun check: every request is run twice\n", vol.name);
    memcpy(first, rep, rn);
    uint32_t held = kept->hold.held, runs = kept->hold.runs;
    struct fat_post post = kept->post;
    bool wrote = op.wrote, gather = op.gather;
    hold_snapshot();
    idl_close_all(rhs, *rhn);
    *rhn = 0;
    if (!undo_restore()) {
        printf("fat %s: rerun check FAILED: nothing to undo\n", vol.name);
        return rn;
    }
    files_drop_unknown();
    views_drop_unknown();
    begin(kind_of(c), op.seq, c->file, op.slot);
    uint32_t rn2 = dispatch(c, op.slot, rhs, rhn);
    if (rn2 != rn || memcmp(first, rep, rn) || held != kept->hold.held ||
        runs != kept->hold.runs || memcmp(&post, &kept->post, sizeof(post)) ||
        wrote != op.wrote || gather != op.gather)
        printf("fat %s: rerun check FAILED: request %u (ordinal %u, channel %x): reply %u/%u "
               "bytes, status %d/%d, held %u/%u in %u/%u runs\n", vol.name,
               (unsigned)op.seq, ((const struct idl_req_hdr *)req)->ordinal, c->id, rn, rn2,
               rn >= 8 ? ((const struct idl_rep_hdr *)first)->status : 0,
               rn2 >= 8 ? ((const struct idl_rep_hdr *)rep)->status : 0, held,
               kept->hold.held, runs, kept->hold.runs);
    else if (hold_differs(held) != held)
        printf("fat %s: rerun check FAILED: request %u (ordinal %u): held sector %u differs\n",
               vol.name, (unsigned)op.seq, ((const struct idl_req_hdr *)req)->ordinal,
               hold_differs(held));
    return rn2;
}

void op_close_again(struct fat_file *f, void (*close)(struct fat_file *f))
{
    if (op.stepped)
        return;
    uint32_t held = kept->hold.held, runs = kept->hold.runs;
    struct fat_post post = kept->post;
    bool wrote = op.wrote;
    hold_snapshot();
    if (!undo_restore()) {
        printf("fat %s: rerun check FAILED: a close with nothing to undo\n", vol.name);
        return;
    }
    begin(FAT_OP_CLOSE, 0, f, 0);
    close(f);
    if (held != kept->hold.held || runs != kept->hold.runs || wrote != op.wrote ||
        memcmp(&post, &kept->post, sizeof(post)))
        printf("fat %s: rerun check FAILED: a close: held %u/%u in %u/%u runs\n", vol.name,
               held, kept->hold.held, runs, kept->hold.runs);
    else if (hold_differs(held) != held)
        printf("fat %s: rerun check FAILED: a close: held sector %u differs\n", vol.name,
               hold_differs(held));
}
#endif

status_t serve_one(const struct fat_chan *c)
{
    struct svcstate *s = state_slots();
    unsigned slot = 0;
    struct idl_slot is;
    svcstate_prepare(s, c->id, &slot, &is);
    status_t st = take(c, &is);
    /* OK and nothing taken: a message thrown away or answered by the take. */
    if (!svcstate_taken(s, slot) || st != OK)
        return st;
    if (s->h->slot[slot].nhandles) {
        refuse(c, slot);
        return OK;
    }
    serve_slot(c, slot);
    return OK;
}

void serve_slot(const struct fat_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    begin(kind_of(c), s->h->slot[slot].seq, c->file, slot);
    uint32_t rn = dispatch(c, slot, rhs, &rhn);
#ifdef FAT_RERUN_CHECK
    rn = rerun(c, svcstate_request(s, slot, NULL), rn, rhs, &rhn);
#endif
    plan_send();
    (void)svcstate_commit(s, slot, rn);   /* the dispatch's reply fits rep_cap: the commit */
    committed();
    test_die(FAT_DIE_COMMIT);
    status_t st = send();
    if (st != OK)
        reply_failed(slot, &rn, st, rhs, &rhn);
    test_die(FAT_DIE_REPLY);
    answer(c, slot, rn, rhs, rhn);
    test_die(FAT_DIE_ANSWERED);
}
