/* audio: how an output's writing is paced (<audio.h>): the room in the
 * ring, waiting for it, starting and pausing the mixer's taking, and where
 * the playing is. For a program that writes on a clock of its own (a
 * browser's audio back end asks for its next buffer when the one before is
 * about to run out) instead of as fast as the ring takes frames
 * (audio_write). The conversion and the ring itself are audio.c's. */
#include <audio.h>
#include <idl/audio.h>
#include <mixer.h>

#define SOON     (5 * NS_PER_S)
#define NS_FRAME_MAX (1u << 22)   /* frames: more than a ring and its lead; guards the ns sum */

/* Device frames the ring must have free for n of the caller's frames to
 * fit for sure: n converted to 48 kHz and rounded up, and one more for the
 * resampler's phase. */
static size_t dev_frames(const struct audio_out *a, size_t n)
{
    if (!a->rs.coef)
        return n;
    return (size_t)(((uint64_t)n * AUDIO_RATE + a->rate - 1) / a->rate) + 1;
}

/* The caller's frames that `ring` free device frames take for sure: the
 * inverse of dev_frames. */
static size_t caller_frames(const struct audio_out *a, size_t ring)
{
    if (!a->rs.coef)
        return ring;
    return ring ? (size_t)((uint64_t)(ring - 1) * a->rate / AUDIO_RATE) : 0;
}

size_t audio_room(const struct audio_out *a)
{
    return a->open ? caller_frames(a, mixer_room(&a->s)) : 0;
}

size_t audio_room_max(const struct audio_out *a)
{
    return a->open ? caller_frames(a, a->s.frames) : 0;
}

uint64_t audio_queued(const struct audio_out *a)
{
    if (!a->open)
        return 0;
    uint64_t r = __atomic_load_n(&a->s.hdr->read, __ATOMIC_ACQUIRE);
    return a->s.write > r ? a->s.write - r : 0;
}

handle_t audio_room_event(const struct audio_out *a)
{
    return a->open ? a->s.event : HANDLE_INVALID;
}

/* The checks the two waits share: min_frames in range, and a stream the
 * mixer will empty. Sets *need to the ring room it means. */
static status_t room_prepare(struct audio_out *a, size_t min_frames, size_t *need)
{
    if (!a->open)
        return ERR_BAD_STATE;
    if (!min_frames || min_frames > audio_room_max(a))
        return ERR_INVALID_ARGS;
    *need = dev_frames(a, min_frames);
    /* The ring is too full for min_frames, and a stream nobody started
     * takes nothing out of it: start it, unless the caller paused it. */
    if (mixer_room(&a->s) < *need && !a->s.started && !a->held)
        return mixer_start(&a->s, now() + SOON);
    return OK;
}

int audio_wait_room(struct audio_out *a, size_t min_frames, uint64_t deadline)
{
    size_t need;
    status_t st = room_prepare(a, min_frames, &need);
    if (st != OK)
        return st;
    a->armed = false;
    while (mixer_room(&a->s) < need) {
        st = mixer_wait_room(&a->s, need, deadline);
        if (st != OK)
            return st;
        if (mixer_room(&a->s) >= need)
            break;
        if (now() >= deadline)
            return ERR_TIMED_OUT;
    }
    return OK;
}

int audio_arm_room(struct audio_out *a, size_t min_frames)
{
    size_t need;
    status_t st = room_prepare(a, min_frames, &need);
    if (st != OK)
        return st;
    if (mixer_arm_room(&a->s, need)) {
        a->armed = false;
        return OK;
    }
    a->armed = true;
    return mixer_peer_closed(&a->s) ? ERR_PEER_CLOSED : ERR_SHOULD_WAIT;
}

int audio_start(struct audio_out *a)
{
    if (!a->open)
        return ERR_BAD_STATE;
    a->held = false;
    return a->s.started ? OK : mixer_start(&a->s, now() + SOON);
}

int audio_pause(struct audio_out *a)
{
    if (!a->open)
        return ERR_BAD_STATE;
    a->held = true;
    return a->s.started ? mixer_stop(&a->s, now() + SOON) : OK;
}

int audio_position(struct audio_out *a, struct audio_pos *pos)
{
    if (!a->open)
        return ERR_BAD_STATE;
    uint64_t t0 = now(), written = 0, consumed = 0, played = 0;
    status_t st = audio_stream_position_until(a->s.ch, t0 + SOON, &written, &consumed, &played);
    if (st != OK)
        return st;
    uint64_t t1 = now();
    /* A frame written now is heard once the play position gets to `write`
     * (the mixer's `played` is never past what it took, which is never
     * past what we wrote), after the resampler has made it. */
    uint64_t ahead = a->s.write > played ? a->s.write - played : 0;
    if (ahead > NS_FRAME_MAX)
        ahead = NS_FRAME_MAX;
    uint64_t ns = ahead * NS_PER_S / AUDIO_RATE;
    if (a->rs.coef)
        ns += (uint64_t)(a->rs.taps / 2) * NS_PER_S / a->rate;
    *pos = (struct audio_pos){
        .written = a->s.write, .played = played, .latency_ns = ns, .at = t0 + (t1 - t0) / 2,
        .lead = a->s.lead,
    };
    return OK;
}
