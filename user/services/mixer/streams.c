/* mixer: the streams. `open_output` on the service channel (SR_AUDIO)
 * makes one: a ring VMO (the header page of <mixer.h>, then RING_FRAMES
 * frames), an event and a channel of its own, all three handed to the
 * client (the VMO and event with only the rights it needs) and watched
 * on our port. The stream channel serves start, stop, drain (answered
 * later, once its frames have been heard), position and the volume; its
 * closing drops the stream. The control channel (SR_AUDIO_CTL) lists the
 * streams, sets any stream's volume and the master volume, and hands out
 * the sound card's query channels (`device`: device.c's thread). Both
 * shared channels, and every opener's channel made from them (clients.c),
 * are read here.
 *
 * `played` for a stream comes from its period history (output.c's
 * remember): the driver frames each period's frames went to, so the play
 * position says how many of its frames have been heard.
 *
 * Each request is read into a request slot of the state (req_take) and
 * run there with the protocol's generated run (audio_run_slot, audioctl_run_slot),
 * answered from the slot's reply area after a commit (req_answer); a
 * successor runs the one in progress again with the same code (run_audio,
 * run_control). A drain answered later is answered before the commit that
 * forgets it (drain_reply): a successor that finds it still waiting
 * answers it again, which the caller, already answered, never reads.
 *
 * A stream's channel, ring and event are put to the keeper (KEEP_STREAM +
 * its slot) before the state counts the stream, so a restarted mixer
 * serves the same stream; a put the keeper can't take refuses the open. */
#include <idl/audio.h>
#include <idl/audioctl.h>
#include <idl/svc.h>
#include <mixmath.h>
#include "internal.h"

#define BUDGET       32   /* messages from one channel per turn */
#define RING_BYTES   (MIXER_RING_HDR + RING_FRAMES * MIXER_FRAME)
#define CLIENT_RING  (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)
#define CLIENT_EVENT (RIGHT_WAIT | RIGHT_SIGNAL | RIGHT_TRANSFER)

_Static_assert((RING_FRAMES & (RING_FRAMES - 1)) == 0, "a power of two");

/* What a stream channel's handlers get. */
struct call {
    struct mixer  *m;
    struct stream *s;
};

/* What the service channel's handlers get: the opener streams count to. */
struct opener {
    struct mixer *m;
    uint32_t      owner;   /* struct stream's */
    uint32_t      gen;
};

bool any_playing(const struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->nums->s[i].used && m->nums->s[i].playing)
            return true;
    return false;
}

uint64_t stream_played(const struct stream *s, uint64_t pos)
{
    for (unsigned i = 0; i < s->nhist; i++) {
        const struct hist *h = &s->hist[i];
        if (pos < h->at + h->n)
            return pos <= h->at ? h->from : h->from + (pos - h->at);
    }
    return s->read;
}

void stream_set_idle(struct mixer *m, struct stream *s, bool idle)
{
    uint32_t v = idle;
    s->idle = idle;
    (void)jam_vmo_write(stream_own(m, s)->vmo, MIXER_RING_MIXER + 8, &v,
                        sizeof(v));   /* the `idle` field */
    if (!idle)
        s->empty = 0;
}

/* Answer the stream's waiting drain: st, and OK with its frames; the
 * drain's end committed first. */
static void drain_reply(struct mixer *m, struct stream *s, status_t st)
{
    struct audio_stream_drain_rep r = { .txid = s->drain_txid, .status = st };
    uint32_t n = sizeof(struct idl_rep_hdr);
    if (st == OK) {
        r.frames = s->drain_to;
        n = sizeof(r);
    }
    /* The client gone: nobody waits. */
    (void)jam_channel_write(stream_own(m, s)->ch, &r, n, NULL, 0);
    s->draining = false;
    state_commit(m);
}

void drain_check(struct mixer *m, struct stream *s, uint64_t pos, status_t st)
{
    if (!s->draining)
        return;
    if (st != OK)
        drain_reply(m, s, st);
    else if (stream_played(s, pos) >= s->drain_to)
        drain_reply(m, s, OK);
}

void stream_drop(struct mixer *m, struct stream *s, const char *why)
{
    struct stream_own *w = stream_own(m, s);
    if (s->draining)
        drain_reply(m, s, ERR_BAD_STATE);
    uint32_t slot = (uint32_t)(s - m->nums->s);
    uint64_t key = (uint64_t)s->gen << 8;
    /* A binding holds its object: let go of both before closing. */
    (void)jam_port_unbind(m->port, w->event, (KEY_EVENT + slot) | key);
    (void)jam_port_unbind(m->port, w->ch, (KEY_STREAM + slot) | key);
    jam_handle_close(w->ch);   /* the keeper's duplicates go below */
    jam_handle_close(w->vmo);
    jam_handle_close(w->event);
    printf("mixer: stream %u (%s) closed (%s): %lu frames taken, %u underrun(s), %u late "
           "period(s), least ahead %u ms, %u limited period(s)\n", s->id, s->name, why,
           (unsigned long)s->read, s->underruns, s->late,
           s->min_lead == UINT32_MAX ? 0 : s->min_lead * 1000 / MIXER_RATE, s->limited);
    uint32_t gen = s->gen;
    *s = (struct stream){ .gen = gen };
    *w = (struct stream_own){ 0 };
    keep_slot_drop(m, KEEP_STREAM + slot);
}

/* ---- the service channel: open_output --------------------------------------------- */

static void set_name(struct stream *s, const uint8_t name[16])
{
    unsigned i = 0;
    for (; i < sizeof(s->name) - 1 && name[i]; i++)
        s->name[i] = name[i] >= 0x20 && name[i] < 0x7f ? (char)name[i] : '?';
    s->name[i] = 0;
}

/* Slot s's ring (its header written), event and channel, bound to the
 * port; *theirs: the client's end of the channel. */
static status_t make_stream(struct mixer *m, struct stream *s, handle_t *theirs)
{
    struct mixer_ring h = { .magic = MIXER_RING_MAGIC, .frames = RING_FRAMES };
    struct stream_own *w = stream_own(m, s);
    handle_t ours = HANDLE_INVALID, peer = HANDLE_INVALID;
    uint32_t slot = (uint32_t)(s - m->nums->s);
    status_t st = jam_vmo_create(RING_BYTES, 0, HANDLE_INVALID, &w->vmo);
    if (st == OK)
        st = jam_vmo_write(w->vmo, 0, &h, sizeof(h));
    if (st == OK)
        st = jam_event_create(&w->event);
    if (st == OK)
        st = jam_channel_create(&ours, &peer);
    s->gen++;
    if (st == OK)
        st = jam_port_bind(m->port, ours, (KEY_STREAM + slot) | (uint64_t)s->gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(m->port, w->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8,
                           MIXER_SIG_DATA, PORT_BIND_ONCE);
    if (st != OK) {
        if (w->event)   /* a binding holds its object: let go of it first */
            (void)jam_port_unbind(m->port, w->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8);
        handle_t left[] = { w->vmo, w->event, ours, peer };
        for (unsigned k = 0; k < 4; k++)
            if (left[k])
                jam_handle_close(left[k]);   /* the channel's binding goes with our end */
        w->vmo = w->event = HANDLE_INVALID;
        return st;
    }
    w->ch = ours;
    *theirs = peer;
    return OK;
}

/* Duplicates of the stream's ring and event with the client's rights. */
static status_t client_handles(const struct stream_own *w, handle_t *ring, handle_t *event)
{
    status_t st = jam_handle_duplicate(w->vmo, CLIENT_RING, ring);
    if (st != OK)
        return st;
    st = jam_handle_duplicate(w->event, CLIENT_EVENT, event);
    if (st != OK)
        jam_handle_close(*ring);
    return st;
}

/* A free stream slot for opener o, or NULL: none free, or o holds
 * MIXER_STREAMS_PER_CLIENT. */
static struct stream *free_slot(struct mixer *m, const struct opener *o)
{
    struct stream *slot = NULL;
    unsigned held = 0;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->nums->s[i];
        if (!s->used && !slot)
            slot = s;
        if (s->used && s->owner == o->owner && s->owner_gen == o->gen)
            held++;
    }
    return held < MIXER_STREAMS_PER_CLIENT ? slot : NULL;
}

static status_t do_open_output(void *ctx, uint32_t rate, uint8_t channels, uint8_t bits,
                               const uint8_t name[16], handle_t *out_stream, handle_t *out_ring,
                               handle_t *out_event, uint32_t *out_id, uint32_t *out_frames,
                               uint32_t *out_lead)
{
    const struct opener *o = ctx;
    struct mixer *m = o->m;
    if (rate != MIXER_RATE || channels != MIXER_CHANNELS || bits != 16)
        return ERR_NOT_SUPPORTED;
    struct stream *s = free_slot(m, o);
    if (!s)
        return ERR_NO_RESOURCES;
    status_t st = out_find(m);
    if (st != OK)
        return ERR_NOT_FOUND;   /* no driver with a path: nothing could ever be heard */
    struct stream_own *w = stream_own(m, s);
    uint32_t slot = (uint32_t)(s - m->nums->s);
    handle_t theirs = HANDLE_INVALID, ring = HANDLE_INVALID, event = HANDLE_INVALID;
    st = make_stream(m, s, &theirs);
    if (st == OK && (st = client_handles(w, &ring, &event)) == OK) {
        handle_t kept[3] = { w->ch, w->vmo, w->event };
        if (keep_slot_put(m, KEEP_STREAM + slot, kept, 3) != OK) {
            st = ERR_NO_RESOURCES;
            jam_handle_close(ring);
            jam_handle_close(event);
        }
    }
    if (w->ch && st != OK) {
        (void)jam_port_unbind(m->port, w->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8);
        (void)jam_port_unbind(m->port, w->ch, (KEY_STREAM + slot) | (uint64_t)s->gen << 8);
        jam_handle_close(theirs);
        jam_handle_close(w->ch);
        jam_handle_close(w->vmo);
        jam_handle_close(w->event);
    }
    if (st != OK) {
        uint32_t gen = s->gen;
        *s = (struct stream){ .gen = gen };
        *w = (struct stream_own){ 0 };
        return st;
    }
    s->used = true;
    m->nums->made_seq = m->state.h->slot[m->cur_slot].seq;
    m->nums->made_kind = MADE_STREAM;
    m->nums->made_index = slot;
    s->id = m->nums->next_id++;
    s->owner = o->owner;
    s->owner_gen = o->gen;
    s->gain = MIX_UNITY;
    s->min_lead = UINT32_MAX;
    set_name(s, name);
    *out_stream = theirs;
    *out_ring = ring;
    *out_event = event;
    *out_id = s->id;
    *out_frames = RING_FRAMES;
    *out_lead = OUT_LEAD * (m->nums->out.period ? m->nums->out.period : PERIOD_GUESS) +
                MIX_LOOKAHEAD;
    printf("mixer: stream %u (%s) opened\n", s->id, s->name);
    return OK;
}

static const struct audio_ops svc_ops = { .open_output = do_open_output };

/* ---- a stream's channel --------------------------------------------------------- */

static status_t do_start(void *ctx)
{
    struct call *c = ctx;
    struct stream *s = c->s;
    if (s->playing)
        return OK;
    s->playing = true;
    s->empty = 0;
    stream_set_idle(c->m, s, false);
    status_t st = out_need(c->m);
    /* No driver at all, or another program has its output (a test):
     * nothing would be heard, so say so. Anything else is tried again. */
    if (st == ERR_NOT_FOUND || st == ERR_BAD_STATE) {
        s->playing = false;
        c->m->nums->out.retry_at = 0;
        return st;
    }
    return OK;
}

static status_t do_stop(void *ctx)
{
    struct call *c = ctx;
    struct stream *s = c->s;
    s->playing = false;
    if (s->idle)
        stream_set_idle(c->m, s, false);
    drain_check(c->m, s, 0, ERR_BAD_STATE);
    return OK;
}

/* The header's `write` now (the client's, so only a number to report). */
static uint64_t header_write(struct mixer *m, const struct stream *s)
{
    uint64_t w = s->written;
    (void)jam_vmo_read(stream_own(m, s)->vmo, MIXER_RING_CLIENT, &w, sizeof(w));
    return w;
}

static status_t do_position(void *ctx, uint64_t *out_written, uint64_t *out_consumed,
                            uint64_t *out_played)
{
    struct call *c = ctx;
    *out_written = header_write(c->m, c->s);
    *out_consumed = c->s->read;
    *out_played = stream_played(c->s, out_position(c->m));
    return OK;
}

static status_t do_stream_volume(void *ctx, int32_t cb, int32_t *out)
{
    struct call *c = ctx;
    c->s->volume = mix_clamp_volume(cb);
    c->s->gain = mix_gain(c->s->volume);
    *out = c->s->volume;
    return OK;
}

static status_t do_levels(void *ctx, int32_t *out_volume, int32_t *out_master,
                          int32_t *out_device)
{
    struct call *c = ctx;
    *out_volume = c->s->volume;
    *out_master = c->m->nums->master;
    *out_device = out_device_gain(c->m);
    return OK;
}

static status_t do_stats(void *ctx, uint32_t *out_underruns, uint32_t *out_late,
                         uint32_t *out_min_lead, uint32_t *out_limited, uint32_t *out_bits,
                         uint64_t *out_played)
{
    struct call *c = ctx;
    bool open = c->m->own_out.ch != HANDLE_INVALID;
    *out_underruns = c->s->underruns;
    *out_late = c->s->late;
    *out_min_lead = c->s->min_lead;
    *out_limited = c->s->limited;
    *out_bits = open ? c->m->nums->out.bits : 0;
    *out_played = stream_played(c->s, open ? c->m->nums->out.played : 0);
    return OK;
}

static status_t do_stream_title(void *ctx, const uint8_t title[64])
{
    struct call *c = ctx;
    char *t = c->s->title;
    size_t n = strnlen((const char *)title, 64);
    if (n == 64)
        n = 63;
    for (size_t i = 0; i < n; i++)
        t[i] = title[i] < ' ' || title[i] == 0x7f ? '?' : (char)title[i];
    t[n] = '\0';
    return OK;
}

static const struct audio_ops stream_ops = {
    .stream_set_title = do_stream_title,
    .stream_start = do_start,
    .stream_stop = do_stop,
    .stream_position = do_position,
    .stream_set_volume = do_stream_volume,
    .stream_levels = do_levels,
    .stream_stats = do_stats,
};

/* stream_drain (the request in slot): answered from drain_check once the
 * frames are heard. */
static void drain_begin(struct mixer *m, struct stream *s, unsigned slot,
                        const struct audio_stream_drain_req *q)
{
    handle_t ch = stream_own(m, s)->ch;
    if (!s->playing || s->draining) {
        req_status(m, slot, ch, ERR_BAD_STATE);
        return;
    }
    uint64_t w = header_write(m, s);
    s->draining = true;
    s->drain_txid = q->txid;
    /* A client that says it wrote more than its ring holds past what was
     * taken can't have: those frames are never coming. */
    s->drain_to = w > s->read + RING_FRAMES ? s->read + RING_FRAMES : w < s->read ? s->read : w;
    req_answer(m, slot, ch, 0, NULL, 0);   /* answered later, by drain_reply */
    drain_check(m, s, m->own_out.ch ? m->nums->out.played : 0, OK);
}

void run_audio(struct mixer *m, unsigned slot, handle_t ch, uint32_t owner, struct stream *s)
{
    uint32_t n = 0;
    const void *q = svcstate_request(&m->state, slot, &n);
    const struct idl_req_hdr *hdr = q;
    if (s && n == sizeof(struct audio_stream_drain_req) && hdr->ordinal == AUDIO_STREAM_DRAIN) {
        drain_begin(m, s, slot, q);
        return;
    }
    if (!s && n == sizeof(struct svc_connect_req) && hdr->ordinal == SVC_CONNECT) {
        clients_connect_reply(m, ch, slot, false, false);
        return;
    }
    struct call c = { m, s };
    struct opener o = { m, owner, owner ? m->nums->c[owner - 1].gen : 0 };
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    struct idl_slot is;
    svcstate_slot(&m->state, slot, &is);
    m->cur_slot = slot;
    uint32_t rn = audio_run_slot(ch, &is, s ? &stream_ops : &svc_ops, s ? (void *)&c : (void *)&o,
                            rhs, &rhn);
    req_answer(m, slot, ch, rn, rhs, rhn);
}

/* One message from ch (port key `key`): a stream's (s), or an opener's
 * `audio` one (`owner`). */
static status_t serve_one(struct mixer *m, handle_t ch, uint32_t key, uint32_t owner,
                          struct stream *s)
{
    unsigned slot;
    status_t st = req_take(m, ch, key, false, &slot);
    if (st == OK && slot != REQ_NONE)
        run_audio(m, slot, ch, owner, s);
    return st;
}

status_t serve_audio(struct mixer *m, handle_t ch, uint32_t owner)
{
    uint32_t gen = owner ? m->nums->c[owner - 1].gen : 0;
    uint32_t key = owner ? (KEY_CLIENT + owner - 1) | gen << 8 : KEY_SVC;
    for (int i = 0; i < BUDGET; i++) {
        status_t st = serve_one(m, ch, key, owner, NULL);
        if (st != OK)
            return st;
    }
    return req_budget_spent(m);
}

void serve_svc(struct mixer *m)
{
    status_t st = serve_audio(m, m->svc, 0);
    m->svc_pending = st == OK;
    if (st != OK && st != ERR_SHOULD_WAIT && st != ERR_PEER_CLOSED)
        printf("mixer: reading the service channel: %s\n", status_str(st));
}

void serve_stream(struct mixer *m, struct stream *s)
{
    struct stream_own *w = stream_own(m, s);
    uint32_t key = (KEY_STREAM + (uint32_t)(s - m->nums->s)) | s->gen << 8;
    w->pending = false;
    for (int i = 0; i < BUDGET; i++) {
        status_t st = serve_one(m, w->ch, key, 0, s);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            stream_drop(m, s, "its channel closed");
        else if (st != ERR_SHOULD_WAIT)
            printf("mixer: reading stream %u's channel: %s\n", s->id, status_str(st));
        return;
    }
    w->pending = req_budget_spent(m) == OK;
}

void stream_event(struct mixer *m, struct stream *s)
{
    uint32_t slot = (uint32_t)(s - m->nums->s);
    handle_t event = stream_own(m, s)->event;
    (void)jam_event_signal(event, MIXER_SIG_DATA, 0);
    status_t st = jam_port_bind(m->port, event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8,
                                MIXER_SIG_DATA, PORT_BIND_ONCE);
    if (st != OK)
        printf("mixer: stream %u's event isn't watched any more (%s)\n", s->id,
               status_str(st));
    if (s->playing && s->idle) {
        stream_set_idle(m, s, false);
        (void)out_need(m);
    }
}

/* ---- the control channel ---------------------------------------------------------- */

static status_t do_streams(void *ctx, uint32_t *out_count, int32_t *out_master, uint8_t list[640])
{
    struct mixer *m = ctx;
    uint64_t pos = out_position(m);
    uint32_t n = 0, last = 0;
    /* In the order opened: the smallest id above the last one listed. */
    for (; n < MIXER_MAX_STREAMS; n++) {
        const struct stream *best = NULL;
        for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
            const struct stream *s = &m->nums->s[i];
            if (s->used && s->id > last && (!best || s->id < best->id))
                best = s;
        }
        if (!best)
            break;
        struct mixer_stream_info e = {
            .id = best->id, .volume = best->volume, .underruns = best->underruns,
            .state = !best->playing ? MIXER_STATE_STOPPED
                     : best->idle   ? MIXER_STATE_IDLE : MIXER_STATE_PLAYING,
            .played = stream_played(best, pos),
        };
        memcpy(e.name, best->name, sizeof(e.name));
        memcpy(list + n * sizeof(e), &e, sizeof(e));
        last = best->id;
    }
    *out_count = n;
    *out_master = m->nums->master;
    return OK;
}

static status_t do_set_volume(void *ctx, uint32_t id, int32_t cb, int32_t *out)
{
    struct mixer *m = ctx;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->nums->s[i];
        if (s->used && s->id == id) {
            s->volume = mix_clamp_volume(cb);
            s->gain = mix_gain(s->volume);
            *out = s->volume;
            return OK;
        }
    }
    return ERR_NOT_FOUND;
}

static status_t do_set_master(void *ctx, int32_t cb, int32_t *out)
{
    struct mixer *m = ctx;
    m->nums->master = mix_clamp_volume(cb);
    m->nums->master_gain = mix_gain(m->nums->master);
    *out = m->nums->master;
    return OK;
}

/* What the desktop shows: the newest stream playing (and its frames
 * coming), its title or name; the output's name, asked of the driver the
 * first time (the driver answers at once: it made the name at its start). */
static status_t do_desk(void *ctx, int32_t *out_master, uint8_t *out_playing,
                        uint8_t title[64], uint8_t output[48])
{
    struct mixer *m = ctx;
    const struct stream *best = NULL;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        const struct stream *s = &m->nums->s[i];
        if (s->used && s->playing && !s->idle && (!best || s->id > best->id))
            best = s;
    }
    *out_master = m->nums->master;
    *out_playing = best != NULL;
    if (best)
        snprintf((char *)title, 64, "%s", best->title[0] ? best->title : best->name);
    memcpy(output, out_name(m), sizeof(m->out_name));
    return OK;
}

static const struct audioctl_ops ctl_ops = {
    .streams = do_streams,
    .set_volume = do_set_volume,
    .set_master = do_set_master,
    .desk = do_desk,
};   /* device: device.c, on a thread of its own; desk_channel: run_control */

/* The desktop's authority only: its shared channel, or an opener's made by
 * desk_channel. */
static bool desk_only(const struct mixer *m, uint32_t key)
{
    uint32_t i = (key & 0xff) - KEY_CLIENT;
    if (key == KEY_DESK)
        return true;
    return (key & 0xff) >= KEY_CLIENT && i < MIXER_CLIENTS && m->nums->c[i].used &&
           m->nums->c[i].desk;
}

/* `device` goes to its thread (device.c), the rest are answered here. */
void run_control(struct mixer *m, unsigned slot, handle_t ch, uint32_t key)
{
    uint32_t n = 0;
    const void *q = svcstate_request(&m->state, slot, &n);
    const struct idl_req_hdr *hdr = q;
    if (desk_only(m, key) && (n < sizeof(*hdr) || (hdr->ordinal != AUDIOCTL_DESK &&
                                                   hdr->ordinal != AUDIOCTL_SET_MASTER))) {
        req_status(m, slot, ch, ERR_ACCESS_DENIED);
        return;
    }
    if (n == sizeof(struct audioctl_desk_channel_req) && hdr->ordinal == AUDIOCTL_DESK_CHANNEL) {
        clients_connect_reply(m, ch, slot, true, true);
        return;
    }
    if (n == sizeof(struct audioctl_device_req) && hdr->ordinal == AUDIOCTL_DEVICE) {
        status_t st = device_ask(m, ch, key, slot, q);
        if (st == OK)
            req_answer(m, slot, ch, 0, NULL, 0);   /* answered by the thread */
        else
            req_status(m, slot, ch, st);
        return;
    }
    if (n == sizeof(struct svc_connect_req) && hdr->ordinal == SVC_CONNECT) {
        clients_connect_reply(m, ch, slot, true, false);
        return;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    struct idl_slot is;
    svcstate_slot(&m->state, slot, &is);
    uint32_t rn = audioctl_run_slot(ch, &is, &ctl_ops, m, rhs, &rhn);
    req_answer(m, slot, ch, rn, rhs, rhn);
}

/* One message from a control channel (ch, port key `key`: the shared one
 * or an opener's). */
static status_t ctl_serve_one(struct mixer *m, handle_t ch, uint32_t key)
{
    unsigned slot;
    status_t st = req_take(m, ch, key, true, &slot);
    if (st == OK && slot != REQ_NONE)
        run_control(m, slot, ch, key);
    return st;
}

status_t serve_control(struct mixer *m, handle_t ch, uint32_t key)
{
    for (int i = 0; i < BUDGET; i++) {
        status_t st = ctl_serve_one(m, ch, key);
        if (st != OK)
            return st;
    }
    return req_budget_spent(m);
}

void serve_desk(struct mixer *m)
{
    status_t st = serve_control(m, m->desk, KEY_DESK);
    m->desk_pending = st == OK;
    if (st == OK || st == ERR_SHOULD_WAIT)
        return;
    if (st != ERR_PEER_CLOSED)
        printf("mixer: reading the desktop's channel: %s\n", status_str(st));
    (void)jam_port_unbind(m->port, m->desk, KEY_DESK);
    jam_handle_close(m->desk);
    m->desk = HANDLE_INVALID;
}

void serve_ctl(struct mixer *m)
{
    status_t st = serve_control(m, m->ctl, KEY_CTL);
    m->ctl_pending = st == OK;
    if (st != OK && st != ERR_SHOULD_WAIT && st != ERR_PEER_CLOSED)
        printf("mixer: reading the control channel: %s\n", status_str(st));
}
