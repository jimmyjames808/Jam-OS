/* fat: a restart its clients don't see (docs/M11.6-PLAN.md, "Where each
 * service's state lives", "The request in progress"). A fat started with
 * a dead instance's state (devmgr's SR_STATE, accepted by svcstate)
 * carries on where it stopped:
 *
 *   1. the operation in progress is put back as it was before it began
 *      (undo_restore) if it hadn't committed: a request (svcstate's
 *      RERUN) or the close of a file whose client had gone. Then the state
 *      is checked (check_views first, check_volume after the undo, since
 *      an operation cut short leaves it half changed): every count, index
 *      and pointer, and the flags views and open files were given (they
 *      are authority: a view's read-only bit);
 *   2. FatFs is told the volume is mounted, without reading the disk:
 *      f_mount(&kept->fs, "", 0) only records the pointer (and clears
 *      fs_type), and the committed FATFS is copied back over it, with the
 *      window, mount id and hints every adopted FIL agrees with. FatFs's
 *      own mount counter (Fsid) starts again at 0 in this instance; it is
 *      used only by a real mount, which is never made over adopted FILs
 *      (a state given up on is set up empty first);
 *   3. a committed close whose send hadn't finished: its send done again;
 *   4. the keeper's handles are taken back (keep_restore), each only if
 *      the state knows its slot: a slot an undone open or view made is
 *      refused, and so closed and dropped; a slot the state knows and the
 *      keeper didn't return is closed (files_adopt, views_adopt);
 *   5. the request in progress is finished exactly once (svcstate's
 *      cases): uncommitted, it runs again from its slot (a write's bytes
 *      from the slot too: request.c's bounce); committed, its send is done
 *      again (the same bytes to the same sectors) and it is answered,
 *      unless its reply went out (the slot's mark, which the kernel sets
 *      in the system call that sends it: then there is nothing to do). A
 *      reply that carries handles (fs.open's file, fs.view's view) and
 *      never went out took them with the process: the slot gets a new
 *      channel and the reply goes again.
 *      A request in progress at two crashes (not deliberate kills: how
 *      the last instance ended is in argv) is answered ERR_IO and dropped,
 *      so one bad request can't crash every successor in turn;
 *   6. every kept channel is waited on again, and the restart logged.
 *
 * When the state can't be carried on from (it fails a check, a held write
 * had failed, the operation in progress went out in steps and can't be
 * undone, a committed one crashed its send twice, the keeper's restore
 * failed), fat starts fresh as it did before: the volume is mounted from
 * the disk and every open file is closed (their clients see
 * ERR_PEER_CLOSED). Views survive that if their table checks out (a view
 * is its channel and its flags, nothing of the volume), so namespaces
 * need not change: the mount's generation is its `fs` channel's (devmgr). */
#include <fsview.h>
#include <keep.h>
#include "fat.h"

#define RESTORE_WAIT (2 * NS_PER_S)   /* devmgr writes the restore right after the start */
#define CRASHES_MAX  2u               /* crashes a request may be in progress at */

/* What this start found and did, for its log line. */
struct found {
    enum svcstate_case k;     /* the request in progress: svcstate's case ... */
    unsigned    slot;         /* ... in this slot */
    bool        fresh;        /* given up on: mounted from the disk */
    bool        keep_views;   /* ... with the views kept */
    bool        restored;     /* keep_restore was asked (it can't be asked twice) */
    bool        forget;       /* the close in progress crashed fat twice: forget its file */
    const char *why;          /* why it was given up on */
    const char *request;      /* what became of the request in progress */
    const char *close;        /* ... and of a close in progress, or NULL */
    unsigned    files, views; /* kept and waited on again */
};

static bool crashed(void)
{
    return vol.ended && !strcmp(vol.ended, FAT_ARG_CRASHED);
}

/* ---- 1. the checks ------------------------------------------------------------------ */

/* The view table: flags fat gives, nothing else. */
static bool check_views(void)
{
    for (unsigned i = 0; i < FAT_VIEWS; i++) {
        const struct fat_view *v = &kept->views[i];
        if (v->used && (v->flags & ~FS_VIEW_FLAGS))
            return false;
    }
    return true;
}

static const char *check_fatfs(void)
{
    const FATFS *fs = &kept->fs;
    if (fs->fs_type < FS_FAT12 || fs->fs_type > FS_FAT32 || fs->pdrv != 0)
        return "not a mounted FAT volume";
    if (!fs->csize || (fs->csize & (fs->csize - 1)) || fs->n_fats < 1 || fs->n_fats > 2)
        return "a volume's geometry out of range";
    if (fs->n_fatent < 3 || fs->fsize >= vol.blocks || fs->database >= vol.blocks ||
        fs->fatbase >= vol.blocks)
        return "a volume bigger than its partition";
    if (fs->winsect != (LBA_t)-1 && fs->winsect >= vol.blocks)
        return "a window outside the partition";
    return NULL;
}

/* One open file's FatFs object, pointing where an adopted one must. */
static bool fil_ok(const FIL *fp)
{
    const FATFS *fs = &kept->fs;
    if (fp->obj.fs != &kept->fs || fp->obj.id != fs->id)
        return false;
    return !fp->dir_ptr || (fp->dir_ptr >= fs->win && fp->dir_ptr < fs->win + FAT_SECTOR);
}

static const char *check_files(void)
{
    uint32_t refs[FAT_MAX_FILES] = { 0 };
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        const struct fat_file *f = &kept->files[i];
        if (!f->used)
            continue;
        if (f->open >= FAT_MAX_FILES || (f->flags & ~FS_FLAGS) ||
            ((f->flags & FS_WRITE) && vol.read_only))
            return "an open file out of range";
        refs[f->open]++;
    }
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        const struct fat_open *o = &kept->opens[i];
        if (o->refs != refs[i] || strnlen(o->path, sizeof(o->path)) == sizeof(o->path))
            return "open files that don't add up";
        if (o->refs && !fil_ok(&o->fil))
            return "an open file of another volume";
    }
    return NULL;
}

static const char *check_hold(void)
{
    const struct fat_hold *h = &kept->hold;
    if (h->held > FAT_HOLD_MAX || h->runs > FAT_HOLD_RUNS || h->ready > FAT_HOLD_MAX ||
        h->held > h->ready)
        return "a hold out of range";
    uint32_t len[FAT_HOLD_RUNS] = { 0 };
    for (uint32_t i = 0; i < h->held; i++) {
        if (h->run_of[i] >= h->runs || h->lba[i] >= vol.blocks)
            return "a held sector out of range";
        len[h->run_of[i]]++;
    }
    for (uint32_t r = 0; r < h->runs; r++)
        if (len[r] != h->run_len[r])
            return "a hold's runs that don't add up";
    return NULL;
}

/* Everything of the volume a successor carries on from, or why not. */
static const char *check_volume(void)
{
    if (!kept->mounted)
        return "no volume was mounted";
    if (kept->disk.hold_failed)
        return "a held write had failed to reach the disk";
    if (kept->disk.nfats > 2 || kept->made.index >= FAT_MAX_FILES + FAT_VIEWS)
        return "the disk's facts out of range";
    const char *why = check_fatfs();
    if (!why)
        why = check_files();
    return why ? why : check_hold();
}

/* ---- 3. the operation in progress, put back -------------------------------------------- */

/* The request (f->k, f->slot) or close in progress, put back as before it
 * began if it hadn't committed; NULL, or why the state can't be carried
 * on from. */
static const char *undo_in_progress(struct found *f)
{
    struct svcstate *s = state_slots();
    const struct fat_undo *u = &kept->undo;
    struct svcstate_slot *sl = &s->h->slot[f->slot];
    bool undo = undo_pending();
    if (f->k == SVCSTATE_RESEND && sl->runs >= CRASHES_MAX)
        return "a committed request crashed its send twice";
    if (f->k == SVCSTATE_RERUN && undo && u->seq == sl->seq) {
        if (u->spent)
            return "the request in progress went out in steps: it can't be undone";
        (void)undo_restore();   /* checked by undo_pending; it can't refuse now */
        return NULL;
    }
    if (f->k != SVCSTATE_RERUN && undo && !u->seq) {   /* a close, its client gone */
        if (crashed())
            kept->close_crashes++;
        if (u->spent)
            return "the close in progress went out in steps: it can't be undone";
        (void)undo_restore();
        f->close = "undone (it happens again)";
        if (kept->close_crashes >= CRASHES_MAX && u->closing < FAT_MAX_FILES) {
            printf("fat %s: closing a file crashed fat twice: it is forgotten, its unsynced "
                   "changes lost\n", vol.name);
            kept->close_crashes = 0;
            f->close = "forgotten (it crashed fat twice)";
            f->forget = true;
        }
    }
    if (f->k == SVCSTATE_RESEND && u->op == kept->ops_done + 1 && u->seq == sl->seq)
        __atomic_store_n(&kept->ops_done, u->op, __ATOMIC_RELEASE);   /* it committed */
    return NULL;
}

/* svcstate's case for the request in progress into f, and the crash it
 * was in progress at counted. runs counts them: written now, so a crash in
 * its re-run counts too. */
static void find_request(struct found *f)
{
    struct svcstate *s = state_slots();
    f->k = svcstate_pending(s, &f->slot);
    if ((f->k == SVCSTATE_RERUN || f->k == SVCSTATE_RESEND) && crashed())
        s->h->slot[f->slot].runs++;
}

/* ---- 4. the kept handles --------------------------------------------------------------- */

struct taking {
    bool files;   /* the state's open files are carried on */
    bool views;   /* ... its views */
};

static bool take(void *ctx, uint32_t slot, const handle_t *hs, unsigned n)
{
    const struct taking *t = ctx;
    if (slot < FAT_KEEP_VIEW(0))
        return t->files && files_take(slot, hs, n);
    return t->views && slot - FAT_KEEP_VIEW(0) < FAT_VIEWS &&
           views_take(slot - FAT_KEEP_VIEW(0), hs, n);
}

/* What the keeper kept, taken back (into fh[], view_ch[]) as t allows; the
 * rest closed and dropped. false: the restore failed (said); what was
 * taken before the failure is the caller's to close. Asked once. */
static bool restore(struct found *f, struct taking t)
{
    f->restored = true;
    if (!vol.keep)
        return true;   /* nothing was kept: files_adopt, views_adopt close the rest */
    struct keep_restored got;
    status_t st = keep_restore(vol.keep, now() + RESTORE_WAIT, take, &t, &got);
    if (st == OK)
        return true;
    printf("fat %s: what the keeper kept couldn't be taken back (%s)\n", vol.name,
           status_str(st));
    return false;
}

/* ---- 5. the request in progress, finished ------------------------------------------------- */

/* The channel the request in `slot` came on, if it is still served. */
static bool chan_of(unsigned slot, struct fat_chan *c)
{
    uint32_t id = state_slots()->h->slot[slot].channel;
    if (id == FAT_CHAN_FS) {
        *c = (struct fat_chan){ .ch = vol.serve, .id = FAT_CHAN_FS, .proto = FAT_PROTO_FS };
        return true;
    }
    uint32_t kind = id >> 30, i = id & 0xff, gen = id >> 8 & 0xffff;
    if (kind == 1)
        return files_chan(i, gen, c);
    if (kind == 2)
        return views_chan(i, gen, c);
    return false;   /* fsctl's: a channel of the dead instance's, gone with it */
}

/* A committed request's reply, which never went out (its slot's mark
 * says so: svcstate_pending), sent now; the handles one carried died with
 * the process: made again. */
static const char *reply_again(const struct fat_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    const struct svcstate_slot *sl = &s->h->slot[slot];
    const struct idl_rep_hdr *r = svcstate_reply_area(s, slot);
    const struct fat_made *m = &kept->made;
    bool made = m->seq == sl->seq && sl->reply_len >= sizeof(*r) && r->status == OK;
    if (!made) {
        answer(c, slot, sl->reply_len, NULL, 0);
        return "answered";
    }
    bool file = m->kind == FAT_MADE_FILE;
    handle_t hs[2];
    status_t st = file ? files_remake(m->index, &hs[0], &hs[1]) : views_remake(m->index, &hs[0]);
    if (st != OK) {
        answer_status(c, slot, st);
        return "its handles couldn't be made again: answered with the error";
    }
    answer(c, slot, sl->reply_len, hs, file ? 2 : 1);
    return "answered, with its handles made again";
}

static const char *finish(enum svcstate_case k, unsigned slot)
{
    struct svcstate *s = state_slots();
    const struct svcstate_slot *sl = &s->h->slot[slot];
    if (k == SVCSTATE_IDLE)
        return "none";
    struct fat_chan c;
    bool have = chan_of(slot, &c);
    if (k == SVCSTATE_RERUN && have && sl->runs >= CRASHES_MAX) {
        const struct idl_req_hdr *q = svcstate_request(s, slot, NULL);
        printf("fat %s: a request (ordinal %u) was in progress at %u crashes: answered ERR_IO "
               "and dropped\n", vol.name, q->ordinal, sl->runs);
        answer_status(&c, slot, ERR_IO);
        return "answered ERR_IO (in progress at two crashes)";
    }
    if (!have) {
        if (k == SVCSTATE_RESEND)
            (void)op_resend();
        answer_status(NULL, slot, ERR_PEER_CLOSED);
        return "its caller gone";
    }
    if (k == SVCSTATE_RERUN) {
        if (sl->nhandles) {   /* they went with the process; refused anyway */
            answer_status(&c, slot, ERR_INVALID_ARGS);
            return "refused, as before";
        }
        serve_slot(&c, slot);
        return "run again";
    }
    if (k == SVCSTATE_RESEND) {
        status_t st = op_resend();
        if (st != OK) {
            answer_status(&c, slot, st);
            return "its send failed again: answered with the error";
        }
    }
    return reply_again(&c, slot);
}

/* ---- the whole ---------------------------------------------------------------------- */

static void say(const struct found *f)
{
    uint64_t kill = standby_kill_ns(), promoted = standby_promoted_ns(), t = now();
    char since[160] = "";
    /* Where the time went, from the kill (a promoted spare is told it). */
    if (kill && t > kill && promoted >= kill && vol.at[FAT_AT_HANDLES] >= vol.at[FAT_AT_STATE])
        snprintf(since, sizeof(since), "; %lu us after the %s (promoted %lu, main %lu, block "
                 "channel %lu, state %lu, handles %lu)", (unsigned long)((t - kill) / NS_PER_US),
                 vol.ended && !strcmp(vol.ended, FAT_ARG_KILLED) ? "kill" : "end",
                 (unsigned long)((promoted - kill) / NS_PER_US),
                 (unsigned long)((vol.at[FAT_AT_MAIN] - kill) / NS_PER_US),
                 (unsigned long)((vol.at[FAT_AT_DISK] - kill) / NS_PER_US),
                 (unsigned long)((vol.at[FAT_AT_STATE] - kill) / NS_PER_US),
                 (unsigned long)((vol.at[FAT_AT_HANDLES] - kill) / NS_PER_US));
    if (f->fresh) {
        printf("fat %s: restart (%s, adoption %lu): starting fresh (%s): mounted from the "
               "disk, open files closed%s%s\n", vol.name, vol.ended ? vol.ended : "?",
               (unsigned long)state_slots()->h->adopted, f->why,
               f->keep_views ? ", views kept" : ", views closed", since);
        return;
    }
    printf("fat %s: restart (%s, adoption %lu): %u file(s), %u view(s); request in "
           "progress: %s%s%s%s\n", vol.name, vol.ended ? vol.ended : "?",
           (unsigned long)state_slots()->h->adopted, f->files, f->views, f->request,
           f->close ? "; close in progress: " : "", f->close ? f->close : "", since);
}

/* Given up on the adopted state: mounted from the disk, the request in
 * progress answered ERR_IO where it can be. */
static status_t start_fresh(struct found *f, bool *no_volume)
{
    f->fresh = true;
    /* A restore asked already failed: nothing is kept (what it handed back
     * before failing is closed below). */
    if (f->restored)
        f->keep_views = false;
    state_reset(f->keep_views);
    if (!f->restored && !restore(f, (struct taking){ .views = f->keep_views })) {
        f->keep_views = false;
        state_reset(false);
    }
    files_drop_unknown();   /* and the keeper told: those clients see ERR_PEER_CLOSED */
    views_drop_unknown();
    status_t st = mount(no_volume);
    if (st != OK)
        return st;
    f->views = views_adopt();
    struct fat_chan c;
    if (f->k == SVCSTATE_RERUN || f->k == SVCSTATE_RESEND)
        answer_status(chan_of(f->slot, &c) ? &c : NULL, f->slot, ERR_IO);
    say(f);
    return OK;
}

/* Is a send of a committed operation still owed (kept->post)? */
static bool post_owed(void)
{
    const struct fat_post *p = &kept->post;
    return p->release || p->flush || p->settle;
}

status_t adopt(bool adopted, bool *no_volume)
{
    struct found f = { .request = "none" };
    if (!adopted) {
        if (!restore(&f, (struct taking){ 0 }))   /* nothing of it is ours: closed, dropped */
            printf("fat %s: starting without what the keeper kept\n", vol.name);
        return mount(no_volume);
    }
    find_request(&f);
    f.keep_views = check_views();
    /* An operation in progress may have left the state half changed: it
     * is put back first (undo.c checks its own copy), then checked. */
    f.why = !f.keep_views ? "a view out of range" : undo_in_progress(&f);
    if (!f.why)
        f.why = check_volume();
    if (f.why)
        return start_fresh(&f, no_volume);
    FATFS committed = kept->fs;
    (void)f_mount(&kept->fs, "", 0);   /* only records the pointer: never fails for "" */
    kept->fs = committed;
    if (!restore(&f, (struct taking){ .files = true, .views = true }))
        f.why = "the keeper's restore failed";
    if (f.why)
        return start_fresh(&f, no_volume);
    vol.at[FAT_AT_HANDLES] = now();
    if (f.forget)
        files_forget(kept->undo.closing);
    /* A close that committed and didn't finish its send: before anything
     * else goes out (a committed request's own send is finish's). */
    if (f.k != SVCSTATE_RESEND && post_owed())
        (void)op_resend();
    f.request = finish(f.k, f.slot);
    reply_flush();   /* its answer now, not after the closes files_adopt makes */
    f.files = files_adopt();
    f.views = views_adopt();
    say(&f);
    return OK;
}
