/* audio: a program's sound output (<audio.h>). The format conversions and
 * the resampler at the top are pure; the rest is the backend, a stream on
 * the mixer (<mixer.h>: its ring is written through our own mapping, no
 * call per write), and is the only place that knows it. The mixer takes
 * only 48 kHz stereo 16-bit frames, so everything is converted here
 * first. */
#include <audio.h>
#include <idl/audio.h>
#include <mixer.h>

#define BLOCK      1024u          /* output frames converted at a time */
#define FADE       240u           /* frames: 5 ms of fade-out at close */
#define TAIL       4096u          /* frames of silence after a drain: a period */
#define OPEN_WAIT  (15 * NS_PER_S)   /* a mixer init is restarting answers late */
#define SOON       (5 * NS_PER_S)
#define DRAIN_WAIT (10 * NS_PER_S)

/* ---- conversions (pure) -------------------------------------------------------- */

static int16_t clamp16(int64_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void audio_s16_from_u8(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++)
        out[i] = (int16_t)(((int)in[i] - 128) * 256);
}

void audio_s16_from_s24le(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++, in += 3) {
        int32_t v = (int32_t)((uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16);
        v = (v ^ 0x800000) - 0x800000;            /* sign-extend bit 23 */
        out[i] = clamp16(((int64_t)v + 128) >> 8);  /* to nearest; >> floors */
    }
}

void audio_s16_from_s32le(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++, in += 4) {
        int32_t v = (int32_t)((uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
                              (uint32_t)in[3] << 24);
        out[i] = clamp16(((int64_t)v + 32768) >> 16);
    }
}

/* ---- the resampler (pure) ---------------------------------------------------------- */

void audio_rs_init(struct audio_rs *rs, unsigned in_rate, unsigned out_rate, unsigned channels)
{
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->pos = 0;
    rs->prev[0] = rs->prev[1] = 0;
    rs->primed = false;
    rs->channels = channels;
}

/* a + (b - a) * pos / one, rounded to nearest (halves up). */
static int16_t lerp(int16_t a, int16_t b, uint64_t pos, uint32_t one)
{
    int64_t x = (int64_t)(b - a) * (int64_t)pos + one / 2;
    int64_t q = x >= 0 ? x / one : -((-x + one - 1) / one);   /* floor */
    return (int16_t)(a + q);
}

size_t audio_rs_run(struct audio_rs *rs, const int16_t *in, size_t in_frames, size_t *used,
                    int16_t *out, size_t cap)
{
    unsigned ch = rs->channels;
    size_t i = 0, o = 0;
    if (rs->in_rate == rs->out_rate) {   /* the frames as they are */
        for (; i < in_frames && o < cap; i++, o++) {
            out[2 * o] = in[i * ch];
            out[2 * o + 1] = in[i * ch + ch - 1];
        }
        *used = i;
        return o;
    }
    for (; i < in_frames; i++) {
        int16_t l = in[i * ch], r = in[i * ch + ch - 1];
        if (!rs->primed) {
            rs->prev[0] = l;
            rs->prev[1] = r;
            rs->primed = true;
            continue;
        }
        /* Every output frame between prev (at 0) and this one (at out_rate). */
        while (rs->pos < rs->out_rate) {
            if (o == cap) {
                *used = i;   /* this input frame again next time */
                return o;
            }
            out[2 * o] = lerp(rs->prev[0], l, rs->pos, rs->out_rate);
            out[2 * o + 1] = lerp(rs->prev[1], r, rs->pos, rs->out_rate);
            o++;
            rs->pos += rs->in_rate;
        }
        rs->pos -= rs->out_rate;
        rs->prev[0] = l;
        rs->prev[1] = r;
    }
    *used = i;
    return o;
}

/* ---- the backend: a mixer stream ------------------------------------------------- */

int audio_open_as(struct audio_out *a, unsigned rate, unsigned channels, const char *name)
{
    memset(a, 0, sizeof(*a));
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX || (channels != 1 && channels != 2))
        return ERR_NOT_SUPPORTED;
    handle_t svc = startup_handle(SR_AUDIO);
    if (svc == HANDLE_INVALID)
        return ERR_NOT_FOUND;   /* started without the mixer's channel */
    status_t st = mixer_open(svc, name, now() + OPEN_WAIT, &a->s);
    if (st != OK)
        return st;
    a->open = true;
    a->ring_min = UINT64_MAX;
    a->rate = rate;
    a->channels = channels;
    audio_rs_init(&a->rs, rate, AUDIO_RATE, channels);
    return OK;
}

int audio_open(struct audio_out *a, unsigned rate, unsigned channels)
{
    return audio_open_as(a, rate, channels, "audio");
}

long audio_write(struct audio_out *a, const void *frames, size_t nframes)
{
    if (!a->open)
        return ERR_BAD_STATE;
    const int16_t *in = frames;
    size_t left = nframes;
    int16_t out[2 * BLOCK];
    while (left) {
        size_t used = 0, done = 0;
        size_t n = audio_rs_run(&a->rs, in, left, &used, out, BLOCK);
        if (!n && !used)
            return ERR_INTERNAL;
        /* Blocks while the ring is full: the mixer takes a period at a
         * time, so room comes within 43 ms while it plays. */
        if (a->s.started) {
            uint64_t r = __atomic_load_n(&a->s.hdr->read, __ATOMIC_ACQUIRE);
            uint64_t queued = a->s.write > r ? a->s.write - r : 0;
            if (queued < a->ring_min)
                a->ring_min = queued;
        }
        status_t st = mixer_write(&a->s, out, n, now() + SOON, &done);
        if (st != OK)
            return st;
        in += used * a->channels;
        left -= used;
    }
    return (long)nframes;
}

int audio_drain(struct audio_out *a)
{
    if (!a->open)
        return ERR_BAD_STATE;
    if (!a->s.write)
        return OK;   /* nothing was ever written */
    status_t st = mixer_drain(&a->s, now() + DRAIN_WAIT);
    static const int16_t quiet[2 * BLOCK];
    for (unsigned k = 0; st == OK && k < TAIL / BLOCK; k++) {
        size_t done = 0;
        st = mixer_write(&a->s, quiet, BLOCK, now() + SOON, &done);
    }
    if (st == OK)
        st = mixer_drain(&a->s, now() + DRAIN_WAIT);
    return st;
}

/* Drop what the mixer has not taken yet: the FADE frames after its `read`
 * faded down to nothing, `write` moved back to their end, and the fade
 * waited for. (The mixer may take a frame or two of them unfaded while we
 * write: one period's worth of race in a 5 ms fade, at worst a click.) */
static void fade_out(struct audio_out *a)
{
    struct mixer_stream *s = &a->s;
    uint64_t r = __atomic_load_n(&s->hdr->read, __ATOMIC_ACQUIRE);
    if (s->write <= r)
        return;   /* nothing queued: it ended where the writes did */
    uint64_t f = s->write - r < FADE ? s->write - r : FADE;
    for (uint64_t i = 0; i < f; i++) {
        int16_t *x = s->data + 2 * ((r + i) % s->frames);
        int32_t g = (int32_t)(f - i);
        x[0] = (int16_t)(x[0] * g / (int32_t)(f + 1));
        x[1] = (int16_t)(x[1] * g / (int32_t)(f + 1));
    }
    s->write = r + f;
    __atomic_store_n(&s->hdr->write, s->write, __ATOMIC_RELEASE);
    (void)mixer_drain(s, now() + SOON);
}

int audio_get_volume(struct audio_out *a, int *centibels)
{
    if (!a->open)
        return ERR_BAD_STATE;
    int32_t vol = 0, master = 0, device = 0;
    status_t st = audio_stream_levels_until(a->s.ch, now() + SOON, &vol, &master, &device);
    if (st == OK)
        *centibels = vol + master + device;
    return st;
}

int audio_set_volume(struct audio_out *a, int centibels)
{
    if (!a->open)
        return ERR_BAD_STATE;
    return mixer_set_volume(&a->s, centibels, now() + SOON, NULL);
}

int audio_stats(struct audio_out *a, struct audio_stats *st)
{
    if (!a->open)
        return ERR_BAD_STATE;
    *st = (struct audio_stats){ .ring_min = a->ring_min, .ring_frames = a->s.frames };
    return audio_stream_stats_until(a->s.ch, now() + SOON, &st->underruns, &st->late,
                                    &st->min_lead, &st->limited, &st->bits, &st->played);
}

void audio_close(struct audio_out *a)
{
    if (a->open && a->s.started)
        fade_out(a);
    if (a->open)
        mixer_close(&a->s);   /* the mixer drops the stream */
    memset(a, 0, sizeof(*a));
}
