/* mixer: the streams. `open_output` on the service channel (SR_AUDIO)
 * makes one: a ring VMO (the header page of <mixer.h>, then RING_FRAMES
 * frames), an event and a channel of its own, all three handed to the
 * client (the VMO and event with only the rights it needs) and watched
 * on our port. The stream channel serves start, stop, drain (answered
 * later, once its frames have been heard), position and the volume; its
 * closing drops the stream. The control channel (SR_AUDIO_CTL) lists the
 * streams, sets any stream's volume and the master volume, and hands out
 * the sound card's query channels (`device`: device.c's thread).
 *
 * `played` for a stream comes from its period history (output.c's
 * remember): the driver frames each period's frames went to, so the play
 * position says how many of its frames have been heard. */
#include <idl/audio.h>
#include <idl/audioctl.h>
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

bool any_playing(const struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->s[i].used && m->s[i].playing)
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

void stream_set_idle(struct stream *s, bool idle)
{
    uint32_t v = idle;
    s->idle = idle;
    (void)jam_vmo_write(s->vmo, MIXER_RING_MIXER + 8, &v, sizeof(v));   /* the `idle` field */
    if (!idle)
        s->empty = 0;
}

/* Answer the stream's waiting drain: st, and OK with its frames. */
static void drain_reply(struct stream *s, status_t st)
{
    struct audio_stream_drain_rep r = { .txid = s->drain_txid, .status = st };
    uint32_t n = sizeof(struct idl_rep_hdr);
    if (st == OK) {
        r.frames = s->drain_to;
        n = sizeof(r);
    }
    s->draining = false;
    (void)jam_channel_write(s->ch, &r, n, NULL, 0);   /* the client gone: nobody waits */
}

void drain_check(struct mixer *m, struct stream *s, uint64_t pos, status_t st)
{
    (void)m;
    if (!s->draining)
        return;
    if (st != OK)
        drain_reply(s, st);
    else if (stream_played(s, pos) >= s->drain_to)
        drain_reply(s, OK);
}

void stream_drop(struct mixer *m, struct stream *s, const char *why)
{
    if (s->draining)
        drain_reply(s, ERR_BAD_STATE);
    uint64_t key = (uint64_t)s->gen << 8;
    (void)jam_port_unbind(m->port, s->event, (KEY_EVENT + (uint32_t)(s - m->s)) | key);
    jam_handle_close(s->ch);   /* its persistent binding goes with our only handle */
    jam_handle_close(s->vmo);
    jam_handle_close(s->event);
    printf("mixer: stream %u (%s) closed (%s): %lu frames taken, %u underrun(s), %u late "
           "period(s), least ahead %u ms, %u limited period(s)\n", s->id, s->name, why,
           (unsigned long)s->read, s->underruns, s->late,
           s->min_lead == UINT32_MAX ? 0 : s->min_lead * 1000 / MIXER_RATE, s->limited);
    uint32_t gen = s->gen;
    *s = (struct stream){ .gen = gen };
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
    handle_t ours = HANDLE_INVALID, peer = HANDLE_INVALID;
    uint32_t slot = (uint32_t)(s - m->s);
    status_t st = jam_vmo_create(RING_BYTES, 0, HANDLE_INVALID, &s->vmo);
    if (st == OK)
        st = jam_vmo_write(s->vmo, 0, &h, sizeof(h));
    if (st == OK)
        st = jam_event_create(&s->event);
    if (st == OK)
        st = jam_channel_create(&ours, &peer);
    s->gen++;
    if (st == OK)
        st = jam_port_bind(m->port, ours, (KEY_STREAM + slot) | (uint64_t)s->gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(m->port, s->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8,
                           MIXER_SIG_DATA, PORT_BIND_ONCE);
    if (st != OK) {
        if (s->event)   /* a binding holds its object: let go of it first */
            (void)jam_port_unbind(m->port, s->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8);
        handle_t left[] = { s->vmo, s->event, ours, peer };
        for (unsigned k = 0; k < 4; k++)
            if (left[k])
                jam_handle_close(left[k]);   /* the channel's binding goes with our end */
        s->vmo = s->event = HANDLE_INVALID;
        return st;
    }
    s->ch = ours;
    *theirs = peer;
    return OK;
}

/* Duplicates of the stream's ring and event with the client's rights. */
static status_t client_handles(const struct stream *s, handle_t *ring, handle_t *event)
{
    status_t st = jam_handle_duplicate(s->vmo, CLIENT_RING, ring);
    if (st != OK)
        return st;
    st = jam_handle_duplicate(s->event, CLIENT_EVENT, event);
    if (st != OK)
        jam_handle_close(*ring);
    return st;
}

static status_t do_open_output(void *ctx, uint32_t rate, uint8_t channels, uint8_t bits,
                               const uint8_t name[16], handle_t *out_stream, handle_t *out_ring,
                               handle_t *out_event, uint32_t *out_id, uint32_t *out_frames,
                               uint32_t *out_lead)
{
    struct mixer *m = ctx;
    if (rate != MIXER_RATE || channels != MIXER_CHANNELS || bits != 16)
        return ERR_NOT_SUPPORTED;
    struct stream *s = NULL;
    for (unsigned i = 0; !s && i < MIXER_MAX_STREAMS; i++)
        if (!m->s[i].used)
            s = &m->s[i];
    if (!s)
        return ERR_NO_RESOURCES;
    status_t st = out_find(m);
    if (st != OK)
        return ERR_NOT_FOUND;   /* no driver with a path: nothing could ever be heard */
    handle_t theirs = HANDLE_INVALID, ring = HANDLE_INVALID, event = HANDLE_INVALID;
    st = make_stream(m, s, &theirs);
    if (st == OK && (st = client_handles(s, &ring, &event)) != OK) {
        (void)jam_port_unbind(m->port, s->event,
                              (KEY_EVENT + (uint32_t)(s - m->s)) | (uint64_t)s->gen << 8);
        jam_handle_close(theirs);
        jam_handle_close(s->ch);
        jam_handle_close(s->vmo);
        jam_handle_close(s->event);
    }
    if (st != OK) {
        uint32_t gen = s->gen;
        *s = (struct stream){ .gen = gen };
        return st;
    }
    s->used = true;
    s->id = m->next_id++;
    s->gain = MIX_UNITY;
    s->min_lead = UINT32_MAX;
    set_name(s, name);
    *out_stream = theirs;
    *out_ring = ring;
    *out_event = event;
    *out_id = s->id;
    *out_frames = RING_FRAMES;
    *out_lead = OUT_LEAD * (m->out.period ? m->out.period : PERIOD_GUESS) + MIX_LOOKAHEAD;
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
    stream_set_idle(s, false);
    status_t st = out_need(c->m);
    /* No driver at all, or another program has its output (a test):
     * nothing would be heard, so say so. Anything else is tried again. */
    if (st == ERR_NOT_FOUND || st == ERR_BAD_STATE) {
        s->playing = false;
        c->m->out.retry_at = 0;
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
        stream_set_idle(s, false);
    drain_check(c->m, s, 0, ERR_BAD_STATE);
    return OK;
}

/* The header's `write` now (the client's, so only a number to report). */
static uint64_t header_write(const struct stream *s)
{
    uint64_t w = s->written;
    (void)jam_vmo_read(s->vmo, MIXER_RING_CLIENT, &w, sizeof(w));
    return w;
}

static status_t do_position(void *ctx, uint64_t *out_written, uint64_t *out_consumed,
                            uint64_t *out_played)
{
    struct call *c = ctx;
    *out_written = header_write(c->s);
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
    *out_master = c->m->master;
    *out_device = out_device_gain(c->m);
    return OK;
}

static status_t do_stats(void *ctx, uint32_t *out_underruns, uint32_t *out_late,
                         uint32_t *out_min_lead, uint32_t *out_limited, uint32_t *out_bits,
                         uint64_t *out_played)
{
    struct call *c = ctx;
    *out_underruns = c->s->underruns;
    *out_late = c->s->late;
    *out_min_lead = c->s->min_lead;
    *out_limited = c->s->limited;
    *out_bits = c->m->out.ch ? c->m->out.bits : 0;
    *out_played = stream_played(c->s, c->m->out.ch ? c->m->out.played : 0);
    return OK;
}

static const struct audio_ops stream_ops = {
    .stream_start = do_start,
    .stream_stop = do_stop,
    .stream_position = do_position,
    .stream_set_volume = do_stream_volume,
    .stream_levels = do_levels,
    .stream_stats = do_stats,
};

/* stream_drain: answered from drain_check once the frames are heard. */
static void drain_begin(struct mixer *m, struct stream *s, const struct audio_stream_drain_req *q)
{
    if (!s->playing || s->draining) {
        idl_reply_status(s->ch, q, sizeof(*q), ERR_BAD_STATE);
        return;
    }
    uint64_t w = header_write(s);
    s->draining = true;
    s->drain_txid = q->txid;
    /* A client that says it wrote more than its ring holds past what was
     * taken can't have: those frames are never coming. */
    s->drain_to = w > s->read + RING_FRAMES ? s->read + RING_FRAMES : w < s->read ? s->read : w;
    drain_check(m, s, m->out.ch ? m->out.played : 0, OK);
}

/* One message from ch with these ops (a stream: drain handled here). */
static status_t serve_one(struct mixer *m, handle_t ch, const struct audio_ops *ops, void *ctx,
                          struct stream *s)
{
    _Alignas(8) uint8_t q[AUDIO_REQ_MAX + 8];
    _Alignas(8) uint8_t r[AUDIO_REP_MAX];
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = jam_channel_read(&(struct channel_read_args){
        .h = ch, .bytes_cap = sizeof(q), .bytes = (uint64_t)(uintptr_t)q,
        .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = IDL_READ_HANDLES, .actual_handles = (uint64_t)(uintptr_t)&nh });
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *hdr = (const void *)q;
    if (s && n == sizeof(struct audio_stream_drain_req) && hdr->ordinal == AUDIO_STREAM_DRAIN) {
        drain_begin(m, s, (const void *)q);
        return OK;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = audio_dispatch(ops, ctx, q, n, r, rhs, &rhn);
    if (!rn || jam_channel_write(ch, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: they're still ours */
    return OK;
}

void serve_svc(struct mixer *m)
{
    m->svc_pending = false;
    for (int i = 0; i < BUDGET; i++) {
        status_t st = serve_one(m, m->svc, &svc_ops, m, NULL);
        if (st != OK) {
            if (st != ERR_SHOULD_WAIT && st != ERR_PEER_CLOSED)
                printf("mixer: reading the service channel: %s\n", status_str(st));
            return;
        }
    }
    m->svc_pending = true;
}

void serve_stream(struct mixer *m, struct stream *s)
{
    struct call c = { m, s };
    s->pending = false;
    for (int i = 0; i < BUDGET; i++) {
        status_t st = serve_one(m, s->ch, &stream_ops, &c, s);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            stream_drop(m, s, "its channel closed");
        else if (st != ERR_SHOULD_WAIT)
            printf("mixer: reading stream %u's channel: %s\n", s->id, status_str(st));
        return;
    }
    s->pending = true;
}

void stream_event(struct mixer *m, struct stream *s)
{
    uint32_t slot = (uint32_t)(s - m->s);
    (void)jam_event_signal(s->event, MIXER_SIG_DATA, 0);
    status_t st = jam_port_bind(m->port, s->event, (KEY_EVENT + slot) | (uint64_t)s->gen << 8,
                                MIXER_SIG_DATA, PORT_BIND_ONCE);
    if (st != OK)
        printf("mixer: stream %u's event isn't watched any more (%s)\n", s->id,
               status_str(st));
    if (s->playing && s->idle) {
        stream_set_idle(s, false);
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
            const struct stream *s = &m->s[i];
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
    *out_master = m->master;
    return OK;
}

static status_t do_set_volume(void *ctx, uint32_t id, int32_t cb, int32_t *out)
{
    struct mixer *m = ctx;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->s[i];
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
    m->master = mix_clamp_volume(cb);
    m->master_gain = mix_gain(m->master);
    *out = m->master;
    return OK;
}

static const struct audioctl_ops ctl_ops = {
    .streams = do_streams,
    .set_volume = do_set_volume,
    .set_master = do_set_master,
};   /* device: device.c, on a thread of its own */

/* One message from the control channel; `device` goes to its thread
 * (device.c), the rest are answered here. */
static status_t ctl_serve_one(struct mixer *m)
{
    _Alignas(8) uint8_t q[AUDIOCTL_REQ_MAX];
    _Alignas(8) uint8_t r[AUDIOCTL_REP_MAX];
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(m->ctl, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(m->ctl, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(m->ctl, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *hdr = (const void *)q;
    if (n == sizeof(struct audioctl_device_req) && hdr->ordinal == AUDIOCTL_DEVICE) {
        device_ask(m, (const void *)q);
        return OK;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = audioctl_dispatch(&ctl_ops, m, q, n, r, rhs, &rhn);
    if (!rn || jam_channel_write(m->ctl, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: they're still ours */
    return OK;
}

void serve_ctl(struct mixer *m)
{
    m->ctl_pending = false;
    for (int i = 0; i < BUDGET; i++) {
        status_t st = ctl_serve_one(m);
        if (st != OK) {
            if (st != ERR_SHOULD_WAIT && st != ERR_PEER_CLOSED)
                printf("mixer: reading the control channel: %s\n", status_str(st));
            return;
        }
    }
    m->ctl_pending = true;
}
