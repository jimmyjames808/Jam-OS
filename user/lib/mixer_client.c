/* The client side of a mixer stream (<mixer.h>): the ring is written
 * through our own mapping, with no call per write; the stream's channel
 * carries start, stop, drain and the volume; the event is waited on only
 * when the ring is full. */
#include <idl/audio.h>
#include <mixer.h>
#include <os.h>

#define WAIT_SLICE (100 * NS_PER_MS)   /* how often a wait for room looks for a dead mixer */

_Static_assert(__builtin_offsetof(struct mixer_ring, write) == MIXER_RING_CLIENT,
               "the client's line");
_Static_assert(__builtin_offsetof(struct mixer_ring, read) == MIXER_RING_MIXER,
               "the mixer's line");
_Static_assert(sizeof(struct mixer_ring) <= MIXER_RING_HDR, "the header fits its page");
_Static_assert(sizeof(struct mixer_stream_info) == 40, "audioctl.idl's list entries");

static uint64_t ring_bytes(const struct mixer_stream *s)
{
    return MIXER_RING_HDR + (uint64_t)s->frames * MIXER_FRAME;
}

status_t mixer_open(handle_t svc, const char *name, uint64_t deadline, struct mixer_stream *s)
{
    uint8_t label[16] = { 0 };
    for (size_t i = 0; name && name[i] && i < sizeof(label) - 1; i++)
        label[i] = (uint8_t)name[i];
    struct mixer_stream o = { 0 };
    status_t st = audio_open_output_until(svc, deadline, MIXER_RATE, MIXER_CHANNELS, 16, label,
                                          &o.ch, &o.vmo, &o.event, &o.id, &o.frames, &o.lead);
    if (st != OK)
        return st;
    uint64_t va = 0;
    if (!o.frames || (o.frames & (o.frames - 1)) || o.frames > (1u << 20))
        st = ERR_BAD_STATE;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), o.vmo, 0, ring_bytes(&o),
                          VMAR_READ | VMAR_WRITE, &va);
    if (st == OK) {
        o.hdr = (struct mixer_ring *)(uintptr_t)va;
        o.data = (int16_t *)(uintptr_t)(va + MIXER_RING_HDR);
        if (o.hdr->magic != MIXER_RING_MAGIC || o.hdr->frames != o.frames)
            st = ERR_BAD_STATE;
        o.write = o.hdr->write;
    }
    if (st != OK) {
        if (va)
            jam_vmar_unmap(startup_handle(SR_SELF_VMAR), va, ring_bytes(&o));
        jam_handle_close(o.ch);
        jam_handle_close(o.vmo);
        jam_handle_close(o.event);
        return st;
    }
    *s = o;
    return OK;
}

void mixer_put(struct mixer_stream *s, const int16_t *frames, size_t n, size_t *done)
{
    /* Acquire: the mixer took the frames before it published `read`. */
    uint64_t read = __atomic_load_n(&s->hdr->read, __ATOMIC_ACQUIRE);
    uint64_t used = s->write >= read ? s->write - read : 0;
    size_t room = used < s->frames ? s->frames - (size_t)used : 0, put = 0;
    if (n > room)
        n = room;
    while (put < n) {
        uint32_t off = (uint32_t)(s->write % s->frames);
        size_t k = s->frames - off < n - put ? s->frames - off : n - put;
        memcpy(s->data + 2 * (size_t)off, frames + 2 * put, k * MIXER_FRAME);
        put += k;
        s->write += k;
    }
    /* Release: the frames are in place before `write` says so. */
    __atomic_store_n(&s->hdr->write, s->write, __ATOMIC_RELEASE);
    if (put && __atomic_load_n(&s->hdr->idle, __ATOMIC_SEQ_CST))
        (void)jam_event_signal(s->event, 0, MIXER_SIG_DATA);   /* a lost wake: one period */
    *done = put;
}

/* No room: wait for the mixer to take frames (MIXER_SIG_SPACE), a slice at
 * a time so a dead mixer is noticed. OK when there may be room now. */
static status_t wait_room(struct mixer_stream *s, uint64_t deadline)
{
    (void)jam_event_signal(s->event, MIXER_SIG_SPACE, 0);
    __atomic_store_n(&s->hdr->waiting, 1, __ATOMIC_SEQ_CST);
    /* Seen after `waiting` is up: a `read` the mixer moved before it
     * looked at `waiting` (its signal then never comes). */
    uint64_t read = __atomic_load_n(&s->hdr->read, __ATOMIC_SEQ_CST);
    status_t st = OK;
    if (s->write - read >= s->frames) {
        uint64_t t = now() + WAIT_SLICE;
        signals_t seen = 0;
        st = jam_object_wait_one(s->event, MIXER_SIG_SPACE, t < deadline ? t : deadline, &seen);
        if (st == ERR_TIMED_OUT && now() < deadline)
            st = OK;
        seen = 0;
        if (jam_object_wait_one(s->ch, SIG_PEER_CLOSED, 0, &seen) == OK &&
            (seen & SIG_PEER_CLOSED))
            st = ERR_PEER_CLOSED;
    }
    __atomic_store_n(&s->hdr->waiting, 0, __ATOMIC_RELAXED);
    return st;
}

status_t mixer_write(struct mixer_stream *s, const int16_t *frames, size_t n, uint64_t deadline,
                     size_t *done)
{
    size_t total = 0;
    status_t st = OK;
    while (st == OK && total < n) {
        size_t put = 0;
        mixer_put(s, frames + 2 * total, n - total, &put);
        total += put;
        if (total == n)
            break;
        if (!s->started)
            st = mixer_start(s, deadline);
        if (st == OK)
            st = wait_room(s, deadline);
    }
    *done = total;
    return st;
}

status_t mixer_start(struct mixer_stream *s, uint64_t deadline)
{
    status_t st = audio_stream_start_until(s->ch, deadline);
    if (st == OK)
        s->started = true;
    return st;
}

status_t mixer_stop(struct mixer_stream *s, uint64_t deadline)
{
    status_t st = audio_stream_stop_until(s->ch, deadline);
    if (st == OK)
        s->started = false;
    return st;
}

status_t mixer_drain(struct mixer_stream *s, uint64_t deadline)
{
    status_t st = s->started ? OK : mixer_start(s, deadline);
    uint64_t frames = 0;
    if (st == OK)
        st = audio_stream_drain_until(s->ch, deadline, &frames);
    return st;
}

status_t mixer_set_volume(struct mixer_stream *s, int32_t cb, uint64_t deadline, int32_t *out)
{
    int32_t got = 0;
    status_t st = audio_stream_set_volume_until(s->ch, deadline, cb, &got);
    if (st == OK && out)
        *out = got;
    return st;
}

void mixer_close(struct mixer_stream *s)
{
    if (s->hdr)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)s->hdr, ring_bytes(s));
    if (s->ch)
        jam_handle_close(s->ch);
    if (s->vmo)
        jam_handle_close(s->vmo);
    if (s->event)
        jam_handle_close(s->event);
    *s = (struct mixer_stream){ 0 };
}
