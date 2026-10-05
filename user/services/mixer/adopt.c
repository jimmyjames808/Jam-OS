/* mixer: a restart its clients don't see (docs/M11.6-PLAN.md, "The
 * mixer"). A mixer started with a used state (init's SR_STATE, checked by
 * state.c) carries on where the dead instance stopped:
 *
 *   1. the keeper's handles (<keep.h>, on SR_KEEP) are taken back, each
 *      only if the state knows its slot (else closed, and the keeper told
 *      to drop it); a slot the state knows and the keeper didn't return is
 *      forgotten (its client's end closed with the keeper's);
 *   2. each stream's channel and event, each opener's channel, are watched
 *      on the new port again, each stream's `idle` written again from the
 *      state (it is written before a commit, so the header may be ahead)
 *      and its committed `read` published again;
 *   3. the driver's stream, which ran on meanwhile through what was written
 *      ahead, is mapped again and asked where it is; the periods from the
 *      last commit on are mixed again from the committed numbers (the same
 *      frames, gains, limiter and dither: the same samples the dead one
 *      wrote, or would have) over the same place in the ring, which is
 *      still ahead of the play position. The lead left (what was written
 *      ahead at the last commit and not yet played) is logged: the
 *      restart's whole budget. The state can't say whether its last
 *      wait_period reached the driver, so it sends one (output.c's header
 *      says why a second one is harmless);
 *   4. the request in progress is finished exactly once (svcstate's
 *      cases): taken and not committed, it is run again from the committed
 *      numbers (a request may have committed part-way, e.g. a start that
 *      opened the output: each one's re-run is written to find its own
 *      work done); committed and its reply not out (the slot's mark, which
 *      the kernel sets in the system call that sends it), its reply is
 *      sent, unless it carried handles (open_output's stream, svc.connect's
 *      channel), which died with the process: then the object goes and
 *      the request runs again. A request in progress at two
 *      crashes (not deliberate kills: init's argv[1]) is answered
 *      ERR_INTERNAL and dropped, so one bad request can't take the mixer
 *      away from everyone;
 *   5. audioctl.device requests the dead thread hadn't answered go to the
 *      new thread (device.c), before step 4 runs one again.
 *
 * A state that fails a check, or a keeper that can't restore, and the
 * mixer starts fresh as before: whatever the keeper held is closed, and
 * clients see ERR_PEER_CLOSED and open again. */
#include <idl/hda.h>
#include <keep.h>
#include "internal.h"

#define RESTORE_WAIT (2 * NS_PER_S)   /* init writes the restore right after the start */
#define CALL_WAIT    (2 * NS_PER_S)
#define KEY_LOW(k)   ((k) & 0xffu)
#define KEY_GEN(k)   ((k) >> 8)       /* a slot's generation, as its 24 bits in a port key */

/* What a restart found, for its log line. */
struct found {
    unsigned    streams, clients, dropped;
    bool        out;        /* the driver's stream taken over, running */
    uint64_t    lead;       /* frames written ahead at the last commit, not yet played */
    const char *request;    /* what became of the request in progress */
};

/* keep_restore's context. */
struct restoring {
    struct mixer *m;
    bool          adopting;   /* false: a fresh start keeps nothing */
};

handle_t key_channel(struct mixer *m, uint32_t key, struct stream **s, uint32_t *owner,
                     bool *ctl)
{
    uint32_t low = KEY_LOW(key), gen = KEY_GEN(key);
    *s = NULL;
    *owner = 0;
    *ctl = key == KEY_CTL;
    if (key == KEY_SVC || key == KEY_CTL)
        return key == KEY_SVC ? m->svc : m->ctl;
    if (low >= KEY_CLIENT && low < KEY_CLIENT + MIXER_CLIENTS) {
        unsigned i = low - KEY_CLIENT;
        const struct client *c = &m->nums->c[i];
        if (!c->used || (c->gen & 0xffffff) != gen)
            return HANDLE_INVALID;
        *ctl = c->ctl;
        *owner = c->ctl ? 0 : i + 1;
        return m->own_c[i].ch;
    }
    if (low >= KEY_STREAM && low < KEY_STREAM + MIXER_MAX_STREAMS) {
        struct stream *x = &m->nums->s[low - KEY_STREAM];
        if (!x->used || (x->gen & 0xffffff) != gen)
            return HANDLE_INVALID;
        *s = x;
        return stream_own(m, x)->ch;
    }
    return HANDLE_INVALID;
}

/* ---- 1. the keeper's handles ------------------------------------------------------ */

/* keep_restore's take: a kept slot the state knows (keep.h). */
static bool take_kept(void *ctx, uint32_t slot, const handle_t *hs, unsigned n)
{
    struct restoring *r = ctx;
    struct mixer *m = r->m;
    if (!r->adopting)
        return false;
    if (slot - KEEP_STREAM < MIXER_MAX_STREAMS) {
        struct stream_own *w = &m->own_s[slot - KEEP_STREAM];
        if (!m->nums->s[slot - KEEP_STREAM].used || n != 3 || w->ch)
            return false;
        *w = (struct stream_own){ .ch = hs[0], .vmo = hs[1], .event = hs[2] };
        return true;
    }
    if (slot - KEEP_CLIENT < MIXER_CLIENTS) {
        struct client_own *w = &m->own_c[slot - KEEP_CLIENT];
        if (!m->nums->c[slot - KEEP_CLIENT].used || n != 1 || w->ch)
            return false;
        w->ch = hs[0];
        return true;
    }
    struct out_own *w = &m->own_out;
    if (slot == KEEP_OUT && n == 2 && m->nums->out.frames && !w->ch) {
        w->ch = hs[0];
        w->vmo = hs[1];
        w->kept = true;
        return true;
    }
    if (slot == KEEP_DRIVER && n == 1 && !w->svc) {
        w->svc = hs[0];
        w->svc_kept = true;
        return true;
    }
    return false;
}

/* Close everything taken (a restore that failed half way). */
static void untake(struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream_own *w = &m->own_s[i];
        handle_t hs[] = { w->ch, w->vmo, w->event };
        for (unsigned k = 0; k < 3; k++)
            if (hs[k])
                jam_handle_close(hs[k]);
        *w = (struct stream_own){ 0 };
    }
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        if (m->own_c[i].ch)
            jam_handle_close(m->own_c[i].ch);
        m->own_c[i] = (struct client_own){ 0 };
    }
    struct out_own *w = &m->own_out;
    handle_t hs[] = { w->ch, w->vmo, w->svc };
    for (unsigned k = 0; k < 3; k++)
        if (hs[k])
            jam_handle_close(hs[k]);
    *w = (struct out_own){ 0 };
}

/* Take what the keeper kept. false: the restore failed (said), and
 * nothing was kept: start fresh. */
static bool restore(struct mixer *m, bool adopting)
{
    if (!m->keep)
        return true;
    struct restoring r = { m, adopting };
    struct keep_restored got;
    status_t st = keep_restore(m->keep, now() + RESTORE_WAIT, take_kept, &r, &got);
    if (st == OK)
        return true;
    printf("mixer: what the keeper kept couldn't be taken back (%s): starting fresh\n",
           status_str(st));
    untake(m);
    return false;
}

/* ---- 2. streams and openers -------------------------------------------------------- */

/* Stream i's channel and event watched again, its header's mixer line
 * written again from the state. */
static status_t bind_stream(struct mixer *m, unsigned i)
{
    struct stream *s = &m->nums->s[i];
    struct stream_own *w = &m->own_s[i];
    uint64_t gen = (uint64_t)s->gen << 8;
    status_t st = jam_port_bind(m->port, w->ch, (KEY_STREAM + i) | gen,
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(m->port, w->event, (KEY_EVENT + i) | gen, MIXER_SIG_DATA,
                           PORT_BIND_ONCE);
    if (st != OK)
        return st;
    uint32_t idle = s->idle;
    (void)jam_vmo_write(w->vmo, offsetof(struct mixer_ring, idle), &idle, sizeof(idle));
    w->pending = true;   /* requests may have queued meanwhile */
    w->took = true;      /* its committed `read` goes into the header (out_publish) */
    return OK;
}

/* Every stream and opener the state and the keeper both know, watched
 * again; the rest forgotten. */
static void bind_all(struct mixer *m, struct found *f)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->nums->s[i];
        if (!s->used)
            continue;
        if (!m->own_s[i].ch) {   /* the keeper had dropped it: its client's end is closed */
            *s = (struct stream){ .gen = s->gen };
            f->dropped++;
            continue;
        }
        status_t st = bind_stream(m, i);
        if (st == OK)
            f->streams++;
        else
            stream_drop(m, s, "it can't be watched after a restart");
    }
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        struct client *c = &m->nums->c[i];
        struct client_own *w = &m->own_c[i];
        if (!c->used)
            continue;
        uint64_t key = (KEY_CLIENT + i) | (uint64_t)c->gen << 8;
        status_t st = !w->ch ? ERR_PEER_CLOSED
                             : jam_port_bind(m->port, w->ch, key, SIG_READABLE | SIG_PEER_CLOSED,
                                             PORT_BIND_PERSISTENT);
        if (st == OK) {
            w->pending = true;
            f->clients++;
        } else {
            clients_drop(m, i);
        }
    }
}

/* ---- 3. the output ---------------------------------------------------------------- */

/* The kept driver's stream mapped, watched, running and caught up. */
static void adopt_out(struct mixer *m, struct found *f)
{
    struct out *o = &m->nums->out;
    struct out_own *w = &m->own_out;
    if (!w->ch) {   /* closed (or its close was under way): as out_close leaves it */
        out_close(m, NULL);
        return;
    }
    uint64_t size = 0, pos = 0;
    uint32_t off = 0, bytes = o->frames * o->frame_bytes;
    status_t st = jam_vmo_get_size(w->vmo, &size);
    if (st == OK && size < bytes)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = out_map(m, w->vmo, bytes, o->period * o->frame_bytes);
    if (st == OK)
        st = jam_port_bind(m->port, w->ch, KEY_OUT | (uint64_t)++o->gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)   /* the dead one may have died before it started it; else a no-op */
        st = hda_start_until(w->ch, now() + CALL_WAIT);
    if (st == OK)
        st = hda_position_until(w->ch, now() + CALL_WAIT, &pos, &off);
    if (st != OK) {
        printf("mixer: the driver's stream can't be taken over (%s): it is opened again\n",
               status_str(st));
        out_close(m, NULL);
        if (st == ERR_PEER_CLOSED)
            out_forget_driver(m);
        return;
    }
    f->out = true;
    f->lead = o->written > pos ? o->written - pos : 0;
    w->pending = true;   /* answers may have queued meanwhile */
    out_fill(m, pos);
    if (w->ch)   /* one may be out already: output.c's header */
        out_send_wait(m, pos);
}

/* ---- 4. the request in progress ------------------------------------------------- */

static void run(struct mixer *m, unsigned slot, handle_t ch, struct stream *s, uint32_t owner,
                bool ctl)
{
    if (ctl)
        run_control(m, slot, ch, m->state.h->slot[slot].channel);
    else
        run_audio(m, slot, ch, owner, s);
}

/* A committed request in slot whose reply never went out (its slot's
 * mark: svcstate_pending): answered now. */
static const char *resend(struct mixer *m, unsigned slot, handle_t ch, struct stream *s,
                          uint32_t owner, bool ctl)
{
    struct svcstate_slot *sl = &m->state.h->slot[slot];
    if (!sl->reply_len) {   /* answered later: a drain (the state), a device (its slot) */
        svcstate_sent(&m->state, slot);
        return "answered later, as before";
    }
    if (m->nums->made_seq != sl->seq) {
        /* Waits for the loop's first take or port wait (the caller gone: no matter). */
        svcstate_answer(&m->state, slot, ch, NULL, 0, &m->reply);
        return "its reply sent";
    }
    /* Its handles were never sent: they died with the process. */
    uint32_t i = m->nums->made_index;
    if (m->nums->made_kind == MADE_STREAM)
        stream_drop(m, &m->nums->s[i], "its open's reply never went out");
    else
        clients_drop(m, i);
    run(m, slot, ch, s, owner, ctl);
    return "run again (its reply with handles never went out)";
}

/* A request taken and not committed in slot: run again. */
static const char *rerun(struct mixer *m, unsigned slot, handle_t ch, struct stream *s,
                         uint32_t owner, bool ctl, bool crashed)
{
    struct svcstate_slot *sl = &m->state.h->slot[slot];
    uint32_t n = 0;
    const struct idl_req_hdr *q = svcstate_request(&m->state, slot, &n);
    /* runs counts the crashes it was in progress at: written now, so a
     * crash in its re-run counts too. */
    if (crashed && ++sl->runs >= 2) {
        printf("mixer: a request (ordinal %#x) was in progress at two crashes: answered "
               "ERR_INTERNAL and dropped\n", q->ordinal);
        req_status(m, slot, ch, ERR_INTERNAL);
        return "answered ERR_INTERNAL (in progress at two crashes)";
    }
    if (sl->nhandles || n > (ctl ? REQ_CAP_CTL : REQ_CAP_AUDIO)) {
        req_status(m, slot, ch, ERR_INVALID_ARGS);   /* as req_take answers it */
        return "refused, as before";
    }
    if (ctl && device_holds(m, sl->seq)) {
        req_answer(m, slot, ch, 0, NULL, 0);   /* the device thread answers it */
        return "with the device thread";
    }
    run(m, slot, ch, s, owner, ctl);
    return "run again";
}

static const char *finish_request(struct mixer *m, bool crashed)
{
    unsigned slot = 0;
    enum svcstate_case c = svcstate_pending(&m->state, &slot);
    if (c == SVCSTATE_IDLE)
        return "none";
    struct stream *s;
    uint32_t owner;
    bool ctl;
    handle_t ch = key_channel(m, m->state.h->slot[slot].channel, &s, &owner, &ctl);
    if (!ch) {   /* its channel is gone, and its caller with it */
        req_answer(m, slot, HANDLE_INVALID, 0, NULL, 0);
        return "its caller gone";
    }
    if (c == SVCSTATE_RERUN)
        return rerun(m, slot, ch, s, owner, ctl, crashed);
    return resend(m, slot, ch, s, owner, ctl);
}

/* ---- the whole --------------------------------------------------------------------- */

static void say(struct mixer *m, const struct found *f, const char *restart)
{
    uint64_t kill = standby_kill_ns(), t = now();
    char lead[64] = "the output closed";
    if (f->out)
        snprintf(lead, sizeof(lead), "output running, lead left %lu frames (%lu ms)",
                 (unsigned long)f->lead, (unsigned long)(f->lead * 1000 / MIXER_RATE));
    char since[48] = "";
    if (kill && t > kill)
        snprintf(since, sizeof(since), "; %lu us after the %s",
                 (unsigned long)((t - kill) / NS_PER_US),
                 restart && !strcmp(restart, "killed") ? "kill" : "end");
    printf("mixer: restart (%s, adoption %lu): %u stream(s), %u opener(s)%s; %s; request in "
           "progress: %s%s\n", restart ? restart : "?", (unsigned long)m->state.h->adopted,
           f->streams, f->clients, f->dropped ? " (some dropped: closed meanwhile)" : "", lead,
           f->request, since);
}

/* Set up empty: what a first start does. */
static void fresh(struct mixer *m)
{
    memset(m->saved, 0, sizeof(*m->saved));
    m->nums->master_gain = MIX_UNITY;
    m->nums->next_id = 1;
}

void adopt(struct mixer *m, bool adopted, const char *restart)
{
    if (!restore(m, adopted))
        adopted = false;
    if (!adopted) {
        fresh(m);
        state_commit(m);
        return;
    }
    struct found f = { 0 };
    bind_all(m, &f);
    adopt_out(m, &f);
    device_resume(m);   /* first: a device request run again below takes a new slot */
    f.request = finish_request(m, !restart || strcmp(restart, "killed"));
    state_commit(m);
    out_publish(m);
    if (!m->own_out.ch && any_playing(m))
        m->nums->out.retry_at = now();   /* out_tick opens it */
    say(m, &f, restart);
}
