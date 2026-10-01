/* audio: a program's sound output (<audio.h>). The format conversions and
 * the resampler at the top are pure; the rest is the backend, which today
 * is the hda driver's one output stream (abi/idl/hda.idl), and is the only
 * place that knows it.
 *
 * The ring: 64 KiB of 48 kHz 16-bit stereo frames, device frame f at
 * index f % ring_frames. The driver zeroes the ring behind the play
 * position, so any frame in [position, position + ring) may be written,
 * from the last position it told us. `written` is the next frame to
 * write; the stream starts when the ring is first full (or at drain),
 * then each write waits a period (wait_period) whenever it is full. */
#include <audio.h>
#include <devmgr.h>
#include <idl/hda.h>

#define BLOCK      1024u          /* output frames converted at a time */
#define GUARD      256u           /* frames ahead of the position the controller may have
                                   * fetched already (5.3 ms): never written */
#define FADE       240u           /* frames: 5 ms of fade-out at close */
#define OPEN_WAIT  (15 * NS_PER_S)
#define SOON       (5 * NS_PER_S)
#define INFO_WAIT  (10 * NS_PER_S)

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

/* ---- the backend: the hda driver's output stream --------------------------------- */

__attribute__((weak)) handle_t audio_devmgr(void)
{
    return startup_handle(SR_DEVMGR);
}

/* The first hda driver with a path to a jack (as the shell's `hda` finds
 * it), or HANDLE_INVALID. */
static handle_t find_device(void)
{
    handle_t dm = audio_devmgr();
    for (uint32_t n = 0; dm != HANDLE_INVALID && n < 32; n++) {
        struct devmgr_rep r;
        handle_t ch;
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  now() + SOON);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        uint32_t codec, pin = 0, dac, pcm, formats, amp, jack, count;
        uint8_t nodes[8], text[240];
        st = hda_info_until(ch, now() + INFO_WAIT, &codec, &pin, &dac, &pcm, &formats, &amp,
                            &jack, &count, nodes, text);
        if (st == OK && pin)
            return ch;
        jam_handle_close(ch);
    }
    return HANDLE_INVALID;
}

int audio_open(struct audio_out *a, unsigned rate, unsigned channels)
{
    memset(a, 0, sizeof(*a));
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX || (channels != 1 && channels != 2))
        return ERR_NOT_SUPPORTED;
    a->dev = find_device();
    if (a->dev == HANDLE_INVALID)
        return ERR_NOT_FOUND;
    uint32_t size = 0, period = 0;
    status_t st = hda_open_output_until(a->dev, now() + OPEN_WAIT, AUDIO_RATE, 2, 16, &a->stream,
                                        &a->vmo, &size, &period);
    if (st != OK) {
        jam_handle_close(a->dev);
        a->dev = HANDLE_INVALID;
        return st;
    }
    uint64_t va = 0;
    st = size && period && size % period == 0 && period % 4 == 0
        ? jam_vmar_map(startup_handle(SR_SELF_VMAR), a->vmo, 0, size, VMAR_READ | VMAR_WRITE, &va)
        : ERR_BAD_STATE;
    if (st != OK) {
        audio_close(a);
        return st;
    }
    a->ring = (int16_t *)(uintptr_t)va;
    a->ring_bytes = size;
    a->ring_frames = size / 4;
    a->period_frames = period / 4;
    a->rate = rate;
    a->channels = channels;
    audio_rs_init(&a->rs, rate, AUDIO_RATE, channels);
    return OK;
}

/* The position now. If the writes have fallen behind it (or the stream
 * was drained), the next write goes GUARD frames ahead of it: what lies
 * between is silence (the driver zeroed it behind the last lap). */
static status_t refresh(struct audio_out *a)
{
    uint32_t off;
    status_t st = hda_position_until(a->stream, now() + SOON, &a->played, &off);
    if (st != OK)
        return st;
    if (a->written < a->played + GUARD) {
        if (!a->drained && a->written < a->played)
            a->underruns++;
        a->written = a->played + GUARD;
    }
    return OK;
}

/* Until the period holding the position has played. */
static status_t wait(struct audio_out *a)
{
    uint32_t off;
    return hda_wait_period_until(a->stream, now() + SOON, a->played, &a->played, &off);
}

static status_t start(struct audio_out *a)
{
    status_t st = hda_start_until(a->stream, now() + SOON);
    if (st == OK)
        a->started = true;
    return st;
}

/* n stereo device frames into the ring, waiting for room. */
static status_t put(struct audio_out *a, const int16_t *src, size_t n)
{
    status_t st;
    if (a->started && (st = refresh(a)) != OK)
        return st;
    while (n) {
        uint64_t space = a->played + a->ring_frames - a->written;
        if (!space) {
            st = a->started ? wait(a) : start(a);
            if (st != OK)
                return st;
            continue;
        }
        uint32_t at = (uint32_t)(a->written % a->ring_frames);
        size_t k = n;
        if (k > space)
            k = (size_t)space;
        if (k > a->ring_frames - at)
            k = a->ring_frames - at;
        memcpy(a->ring + 2 * at, src, k * 4);
        a->written += k;
        a->drained = false;
        src += 2 * k;
        n -= k;
    }
    return OK;
}

long audio_write(struct audio_out *a, const void *frames, size_t nframes)
{
    if (!a->ring)
        return ERR_BAD_STATE;
    const int16_t *in = frames;
    size_t left = nframes;
    int16_t out[2 * BLOCK];
    while (left) {
        size_t used = 0;
        size_t n = audio_rs_run(&a->rs, in, left, &used, out, BLOCK);
        if (!n && !used)
            return ERR_INTERNAL;
        status_t st = put(a, out, n);
        if (st != OK)
            return st;
        in += used * a->channels;
        left -= used;
    }
    return (long)nframes;
}

int audio_drain(struct audio_out *a)
{
    if (!a->ring)
        return ERR_BAD_STATE;
    if (!a->written)
        return OK;   /* nothing was ever written */
    status_t st;
    if (!a->started && (st = start(a)) != OK)
        return st;
    uint64_t end = a->written + a->period_frames;
    while (a->played < end)
        if ((st = wait(a)) != OK)
            return st;
    a->drained = true;
    return OK;
}

/* Drop what is queued past the next GUARD frames: fade the FADE frames
 * there down to nothing, zero the rest, and wait until the fade has
 * played. */
static void fade_out(struct audio_out *a)
{
    if (refresh(a) != OK)
        return;
    uint64_t from = a->played + GUARD;
    if (a->written <= from)
        return;
    uint64_t f = a->written - from;
    if (f > FADE)
        f = FADE;
    for (uint64_t i = 0; i < f; i++) {
        int16_t *s = a->ring + 2 * ((from + i) % a->ring_frames);
        int32_t g = (int32_t)(f - i);
        s[0] = (int16_t)(s[0] * g / (int32_t)(f + 1));
        s[1] = (int16_t)(s[1] * g / (int32_t)(f + 1));
    }
    for (uint64_t x = from + f; x < a->written; x++) {
        int16_t *s = a->ring + 2 * (x % a->ring_frames);
        s[0] = s[1] = 0;
    }
    a->written = from + f;
    while (a->played < a->written + GUARD)
        if (wait(a) != OK)
            return;
}

int audio_get_volume(struct audio_out *a, int *centibels)
{
    if (a->dev == HANDLE_INVALID)
        return ERR_BAD_STATE;
    int32_t gain = 0, min, max;
    uint32_t step;
    status_t st = hda_get_gain_until(a->dev, now() + SOON, &gain, &step, &min, &max);
    if (st == OK)
        *centibels = gain;
    return st;
}

int audio_set_volume(struct audio_out *a, int centibels)
{
    if (a->dev == HANDLE_INVALID)
        return ERR_BAD_STATE;
    int before;
    status_t st;
    if (!a->gain_set && (st = audio_get_volume(a, &before)) != OK)
        return st;
    int32_t gain, min, max;
    uint32_t step;
    st = hda_set_gain_until(a->dev, now() + SOON, centibels, &gain, &step, &min, &max);
    if (st == OK && !a->gain_set) {
        a->gain_set = true;
        a->gain_before = before;
    }
    return st;
}

void audio_close(struct audio_out *a)
{
    if (a->started) {
        fade_out(a);
        hda_stop_until(a->stream, now() + SOON);
    }
    if (a->ring)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)a->ring, a->ring_bytes);
    if (a->vmo != HANDLE_INVALID)
        jam_handle_close(a->vmo);
    if (a->stream != HANDLE_INVALID)
        jam_handle_close(a->stream);   /* the driver stops (mutes) and releases the stream */
    if (a->gain_set) {
        int32_t gain, min, max;
        uint32_t step;
        hda_set_gain_until(a->dev, now() + SOON, a->gain_before, &gain, &step, &min, &max);
    }
    if (a->dev != HANDLE_INVALID)
        jam_handle_close(a->dev);
    memset(a, 0, sizeof(*a));
}
