/* extfs: a start, and a restart its clients don't see.
 *
 * Every start mounts the volume from the disk: the journal's replay
 * (ext_jnl_mount: every whole transaction written home) is the recovery,
 * the same after a kill as after a power cut, then the orphan list
 * (recover_orphans: inodes with no name dropped, truncations finished).
 * An instance started with a dead one's state (SR_STATE, accepted by
 * svcstate) then carries on from it:
 *
 *   1. the state is checked: the volume's UUID is the one it was made for,
 *      every open file's inode is there with its generation, every view's
 *      flags and root in range (the watches' table was taken over before
 *      the mount, state.c: one that fails its own checks starts empty, and
 *      its watcher channels close, the rest carrying on);
 *   2. the request in progress (svcstate's cases): not committed in its
 *      slot, but its transaction id recorded and committed in the journal
 *      (ext_jnl_committed): it is done, and its slot is committed now with
 *      the reply it holds. Not committed in the journal: the slot it made
 *      (an fs.open's file, an fs.view's view, an fs.watch's watcher
 *      channel, an fs.opendir's directory channel) is forgotten, a
 *      write-back error a file.sync's open heard of is put back (the
 *      answer that reported it never went out: wback.c's kept->heard),
 *      and it runs again (step 5);
 *   3. the keeper's handles taken back (keep_restore), each only if the
 *      state knows its slot;
 *   4. the orphans dropped but those the state holds (files open or
 *      mapped, directories removed while a directory channel held them);
 *   5. the request finished exactly once: run again, or answered from its
 *      slot (a reply that carried handles and never went out has them
 *      made again: a new channel for the client), unless its reply went
 *      out. A request in progress at two crashes (not deliberate kills) is
 *      answered ERR_IO and dropped;
 *   6. every kept channel waited on again; a file whose handles didn't
 *      come back is closed, so is a watcher channel (or one whose bell
 *      didn't: the dead instance read the give and died before keeping
 *      it); the restart logged. Each watcher's queue starts with
 *      IN_Q_OVERFLOW, its bell rung: the dead queue, and any change the
 *      successor finished from its slot without running it again, went
 *      unreported.
 *
 * A state that fails a check is given up on: the views are kept if their
 * table checks out, every open file, directory channel and watcher channel
 * is closed (its client sees ERR_PEER_CLOSED; a watcher's bell is rung
 * first, so a poll wakes to see it), and the request in progress is
 * answered ERR_IO. */
#include <keep.h>
#include "extfs.h"

#define RESTORE_WAIT (2 * NS_PER_S)
#define CRASHES_MAX  2u
#define RECOVER_STEP 8u    /* orphans per transaction at a mount */

struct found {
    enum svcstate_case k;
    unsigned    slot;
    bool        fresh, keep_views, restored;
    const char *why;
    const char *request;
    unsigned    files, views, dirs, watchers;
};

static bool crashed(void)
{
    return vol.ended && !strcmp(vol.ended, EXTFS_ARG_CRASHED);
}

/* ---- the orphans ------------------------------------------------------------------- */

static bool keep_open(void *ctx, uint32_t ino)
{
    (void)ctx;
    return files_held(ino);   /* open, mapped, or a directory channel's */
}

void recover_orphans(void)
{
    struct ext_fs *fs = &vol.fs;
    uint32_t done = RECOVER_STEP, total = 0;
    status_t st = OK;
    while (st == OK && done == RECOVER_STEP && writable() && fs->sb.last_orphan) {
        struct ext_time now;
        time_now(&now);
        st = tx_begin();
        if (st == OK)
            st = ext_orphans_recover(fs, keep_open, NULL, &now, RECOVER_STEP, &done);
        if (st == OK)
            st = tx_commit();
        else
            tx_abort();
        total += st == OK ? done : 0;
    }
    if (st != OK) {
        if (fs->ro & EXT_RO_DAMAGED)
            went_bad("the orphan list");
        printf("extfs %s: the orphan list couldn't be finished: %s\n", vol.name,
               status_str(st));
    } else if (total) {
        printf("extfs %s: %u orphan(s) dropped or finished\n", vol.name, (unsigned)total);
    }
}

/* ---- the kept handles --------------------------------------------------------------- */

struct taking {
    bool files, views, watches, locks, dirs;
};

static bool take(void *ctx, uint32_t slot, const handle_t *hs, unsigned n)
{
    const struct taking *t = ctx;
    if (slot >= XF_KEEP_DIR(0))
        return t->dirs && dirs_take(slot - XF_KEEP_DIR(0), hs, n);
    if (slot >= XF_KEEP_LOCK(0))
        return t->locks && locks_take(slot - XF_KEEP_LOCK(0), hs, n);
    if (slot < XF_KEEP_VIEW(0))
        return t->files && files_take(slot, hs, n);
    if (slot >= XF_KEEP_WATCH(0)) {
        if (t->watches)
            return watch_take(slot - XF_KEEP_WATCH(0), hs, n);
        watch_refused(hs, n);
        return false;
    }
    if (slot >= XF_KEEP_MAP(0))
        return t->files && maps_take(slot - XF_KEEP_MAP(0), hs, n);
    return t->views && views_take(slot - XF_KEEP_VIEW(0), hs, n);
}

static bool restore(struct found *f, struct taking t)
{
    f->restored = true;
    if (!vol.keep)
        return true;
    struct keep_restored got;
    status_t st = keep_restore(vol.keep, now() + RESTORE_WAIT, take, &t, &got);
    if (st == OK)
        return true;
    printf("extfs %s: what the keeper kept couldn't be taken back (%s)\n", vol.name,
           status_str(st));
    return false;
}

/* ---- the request in progress --------------------------------------------------------- */

static bool chan_of(unsigned slot, struct xf_chan *c)
{
    uint32_t id = state_slots()->h->slot[slot].channel;
    if (id == XF_CHAN_FS || id == XF_CHAN_OWN) {
        *c = (struct xf_chan){ .ch = id == XF_CHAN_FS ? vol.serve : vol.own, .id = id,
                               .proto = XF_PROTO_FS };
        views_whole(&c->ctx);
        return c->ch != HANDLE_INVALID;
    }
    uint32_t kind = id >> 30, i = id & 0xff, gen = id >> 8 & 0xffff;
    if (kind == 0 && (id & ~0xffffffu) == XF_CHAN_DIR_BIT)
        return dirs_chan(i, gen, c);
    if (kind == 1)
        return files_chan(i, gen, c);
    if (kind == 2)
        return views_chan(i, gen, c);
    if (kind == 3)
        return watch_chan(i, gen, c);
    return false;   /* fsctl's: the dead instance's own channel */
}

/* Not committed in its slot: was its transaction? Then it is done: the
 * slot committed now with the reply it holds. */
static bool journal_has(unsigned slot)
{
    struct svcstate *s = state_slots();
    const struct xf_slot *x = &kept->slot[slot & 1];
    if (x->seq != s->h->slot[slot].seq || !__atomic_load_n(&x->has_tid, __ATOMIC_ACQUIRE) ||
        !ext_jnl_committed(&vol.fs, x->tid))
        return false;
    return svcstate_commit(s, slot, x->rn) == OK;
}

static const char *reply_again(const struct xf_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    const struct svcstate_slot *sl = &s->h->slot[slot];
    const struct idl_rep_hdr *r = svcstate_reply_area(s, slot);
    const struct xf_made *m = &kept->made;
    bool made = m->seq == sl->seq && sl->reply_len >= sizeof(*r) && r->status == OK;
    if (!made) {
        answer(c, slot, sl->reply_len, NULL, 0);
        return "answered";
    }
    handle_t hs[2];
    status_t st = m->kind == XF_MADE_FILE    ? files_remake(m->index, &hs[0], &hs[1])
                  : m->kind == XF_MADE_VIEW  ? views_remake(m->index, &hs[0])
                  : m->kind == XF_MADE_WATCH ? watch_remake(m->index, &hs[0])
                  : m->kind == XF_MADE_DIR   ? dirs_remake(m->index, &hs[0])
                                             : maps_remake(m->index, &hs[0]);
    if (st != OK) {
        answer_status(c, slot, st);
        return "its handles couldn't be made again: answered with the error";
    }
    answer(c, slot, sl->reply_len, hs, m->kind == XF_MADE_FILE ? 2 : 1);
    return "answered, with its handles made again";
}

static const char *finish(enum svcstate_case k, unsigned slot)
{
    struct svcstate *s = state_slots();
    const struct svcstate_slot *sl = &s->h->slot[slot];
    if (k == SVCSTATE_IDLE)
        return "none";
    struct xf_chan c;
    bool have = chan_of(slot, &c);
    if (k == SVCSTATE_RERUN && have && sl->runs >= CRASHES_MAX) {
        const struct idl_req_hdr *q = svcstate_request(s, slot, NULL);
        printf("extfs %s: a request (ordinal %u) was in progress at %u crashes: answered "
               "ERR_IO and dropped\n", vol.name, q->ordinal, sl->runs);
        answer_status(&c, slot, ERR_IO);
        return "answered ERR_IO (in progress at two crashes)";
    }
    if (!have) {
        if (k == SVCSTATE_RERUN)
            answer_status(NULL, slot, ERR_PEER_CLOSED);
        else
            svcstate_sent(s, slot);
        return "its caller gone";
    }
    if (k == SVCSTATE_RERUN) {
        if (sl->nhandles) {
            answer_status(&c, slot, ERR_INVALID_ARGS);
            return "refused, as before";
        }
        serve_slot(&c, slot);
        return "run again";
    }
    return reply_again(&c, slot);
}

/* ---- the whole --------------------------------------------------------------------- */

static void say(const struct found *f)
{
    uint64_t kill = standby_kill_ns(), t = now();
    char since[96] = "";
    if (kill && t > kill)
        snprintf(since, sizeof(since), "; %lu us after the %s (mounted at %lu)",
                 (unsigned long)((t - kill) / NS_PER_US),
                 vol.ended && !strcmp(vol.ended, EXTFS_ARG_KILLED) ? "kill" : "end",
                 (unsigned long)((vol.at[XF_AT_MOUNT] - kill) / NS_PER_US));
    const struct ext_jnl *j = &vol.jnl;
    if (f->fresh) {
        printf("extfs %s: restart (%s, adoption %lu): starting fresh (%s): open files, "
               "directory channels and watchers closed%s; journal: %u transaction(s) "
               "replayed%s\n", vol.name, vol.ended ? vol.ended : "?",
               (unsigned long)state_slots()->h->adopted, f->why,
               f->keep_views ? ", views kept" : ", views closed", (unsigned)j->replayed, since);
        return;
    }
    printf("extfs %s: restart (%s, adoption %lu): %u file(s), %u view(s), %u directory "
           "channel(s), %u watcher(s); request in progress: %s; journal: %u transaction(s) "
           "replayed%s\n", vol.name, vol.ended ? vol.ended : "?",
           (unsigned long)state_slots()->h->adopted, f->files, f->views, f->dirs, f->watchers,
           f->request, (unsigned)j->replayed, since);
}

static status_t start_fresh(struct found *f)
{
    f->fresh = true;
    if (f->restored)
        f->keep_views = false;
    state_reset(f->keep_views);
    memcpy(kept->uuid, vol.fs.sb.uuid, sizeof(kept->uuid));
    kept->mounted = true;
    if (!f->restored && !restore(f, (struct taking){ .views = f->keep_views })) {
        f->keep_views = false;
        state_reset(false);
        memcpy(kept->uuid, vol.fs.sb.uuid, sizeof(kept->uuid));
        kept->mounted = true;
    }
    files_drop_unknown();
    views_drop_unknown();
    dirs_drop_unknown();
    maps_drop_unknown();   /* their VMOs' pages: the requests of keys it doesn't know */
    recover_orphans();
    f->views = views_adopt();
    struct xf_chan c;
    if (f->k == SVCSTATE_RERUN || f->k == SVCSTATE_RESEND)
        answer_status(chan_of(f->slot, &c) ? &c : NULL, f->slot, ERR_IO);
    say(f);
    return OK;
}

/* A request not committed: what it made is forgotten, and an error its
 * file.sync's open heard of is put back (it runs again: the answer that
 * reports it never went out). */
static void unmake(unsigned slot)
{
    const struct xf_made *m = &kept->made;
    uint64_t seq = state_slots()->h->slot[slot].seq;
    wback_unhear(seq);
    if (m->seq != seq)
        return;
    if (m->kind == XF_MADE_FILE && m->index < XF_MAX_FILES)
        kept->files[m->index].used = false;   /* the keeper's copy is refused, so dropped */
    else if (m->kind == XF_MADE_VIEW && m->index < XF_VIEWS)
        kept->views[m->index].used = false;
    else if (m->kind == XF_MADE_MAP && m->index < XF_MAX_MAPS)
        kept->maps[m->index].used = false;    /* to run: its VMO's GONE finds a key nobody knows */
    else if (m->kind == XF_MADE_WATCH && m->index < FSW_CHANNELS)
        watch_unmake(m->index);               /* the keeper's copy is refused, so dropped */
    else if (m->kind == XF_MADE_DIR && m->index < XF_DIRS)
        kept->dirs[m->index].used = false;    /* the keeper's copy is refused, so dropped */
}

status_t adopt(bool adopted, bool *no_volume)
{
    struct found f = { .request = "none" };
    status_t st = mount(no_volume);
    vol.at[XF_AT_MOUNT] = now();
    if (st != OK)
        return st;
    if (!adopted) {
        memcpy(kept->uuid, vol.fs.sb.uuid, sizeof(kept->uuid));
        kept->mounted = true;
        if (!restore(&f, (struct taking){ 0 }))
            printf("extfs %s: starting without what the keeper kept\n", vol.name);
        recover_orphans();
        return OK;
    }
    struct svcstate *s = state_slots();
    f.k = svcstate_pending(s, &f.slot);
    if ((f.k == SVCSTATE_RERUN || f.k == SVCSTATE_RESEND) && crashed())
        s->h->slot[f.slot].runs++;
    f.why = views_check();
    f.keep_views = !f.why;
    if (!f.why && (!kept->mounted || memcmp(kept->uuid, vol.fs.sb.uuid, sizeof(kept->uuid))))
        f.why = "a state of another volume";
    if (!f.why && (kept->made.kind > XF_MADE_DIR ||
                   kept->made.index >= (kept->made.kind == XF_MADE_FILE    ? XF_MAX_FILES
                                        : kept->made.kind == XF_MADE_VIEW  ? XF_VIEWS
                                        : kept->made.kind == XF_MADE_WATCH ? FSW_CHANNELS
                                        : kept->made.kind == XF_MADE_DIR   ? XF_DIRS
                                                                           : XF_MAX_MAPS)))
        f.why = "the request's slot out of range";
    if (!f.why)
        f.why = files_check();
    if (!f.why)
        f.why = dirs_check();
    if (!f.why)
        f.why = maps_check();
    if (!f.why)
        f.why = locks_check();
    if (f.why)
        return start_fresh(&f);
    if (f.k == SVCSTATE_RERUN) {
        if (journal_has(f.slot))
            f.k = SVCSTATE_RESEND;   /* done: its answer is in the slot */
        else
            unmake(f.slot);
    }
    if (!restore(&f, (struct taking){ .files = true, .views = true, .watches = true,
                                       .locks = true, .dirs = true })) {
        f.why = "the keeper's restore failed";
        return start_fresh(&f);
    }
    vol.at[XF_AT_HANDLES] = now();
    recover_orphans();
    f.request = finish(f.k, f.slot);
    /* A rename in the journal whose reach check (held to its commit) died
     * with it: run now. Not at two crashes: the check may be what ends them. */
    watch_moves_again(f.k == SVCSTATE_RESEND && s->h->slot[f.slot].runs < CRASHES_MAX);
    reply_flush();
    f.files = files_adopt();
    (void)locks_adopt();   /* after files_adopt, whose closes let go of POSIX locks */
    f.views = views_adopt();
    f.dirs = dirs_adopt();
    f.watchers = watch_adopt();
    maps_adopt();   /* after files_adopt, whose closes let go of shared VMOs */
    say(&f);
    return OK;
}
