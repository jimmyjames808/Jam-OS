/* play_src: `play`'s sources (play_src.h). A WAV file: the header with
 * <wav.h>, then the samples straight from the file, 8-, 24- and 32-bit
 * ones made 16-bit. */
#include <audio.h>
#include <wav.h>
#include "play_src.h"

struct play_src_ops {
    long (*read)(struct play_src *s, int16_t *out, size_t frames);
    void (*close)(struct play_src *s);
};

const char *play_mss(uint64_t frames, uint32_t rate, char *buf, size_t size)
{
    uint64_t s = (frames + rate / 2) / rate;
    snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    return buf;
}

static status_t read_file(void *ctx, uint64_t offset, void *dst, size_t n, size_t *got)
{
    return file_read(ctx, offset, dst, n, got);
}

/* ---- WAV ---------------------------------------------------------------- */

struct wav_src {
    struct wav_info w;
    uint64_t done;      /* frames read so far */
    uint8_t *raw;       /* the file's bytes for one read */
    size_t   cap;       /* raw's size in frames */
};

static long wav_read(struct play_src *s, int16_t *out, size_t frames)
{
    struct wav_src *v = s->state;
    const struct wav_info *w = &v->w;
    if (w->bits == 16) {
        /* Straight into out: the file's samples are what out holds. */
    } else if (v->cap < frames) {
        free(v->raw);
        v->cap = 0;
        if (!(v->raw = malloc(frames * w->frame_bytes)))
            return ERR_NO_MEMORY;
        v->cap = frames;
    }
    uint64_t n = w->frames - v->done;
    if (n > frames)
        n = frames;
    if (!n)
        return 0;
    uint8_t *dst = w->bits == 16 ? (uint8_t *)out : v->raw;
    size_t got = 0;
    status_t st = file_read(s->f, w->data_offset + v->done * w->frame_bytes, dst,
                            n * w->frame_bytes, &got);
    if (st != OK)
        return st;
    n = got / w->frame_bytes;   /* 0: the file got shorter meanwhile */
    size_t samples = n * w->channels;
    if (w->bits == 8)
        audio_s16_from_u8(out, v->raw, samples);
    else if (w->bits == 24)
        audio_s16_from_s24le(out, v->raw, samples);
    else if (w->bits == 32)
        audio_s16_from_s32le(out, v->raw, samples);
    v->done += n;
    return (long)n;
}

static void wav_close(struct play_src *s)
{
    struct wav_src *v = s->state;
    free(v->raw);
    free(v);
}

static const struct play_src_ops wav_ops = { wav_read, wav_close };

static status_t wav_open(struct play_src *s, uint64_t size, const char **why)
{
    struct wav_src *v = calloc(1, sizeof(*v));
    if (!v) {
        *why = "out of memory";
        return ERR_NO_MEMORY;
    }
    status_t st = wav_parse(&v->w, read_file, s->f, size, why);
    if (st != OK) {
        free(v);
        return st;
    }
    s->rate = v->w.rate;
    s->channels = v->w.channels;
    s->frames = v->w.frames;
    char t[24];
    snprintf(s->desc, sizeof(s->desc), "%u Hz, %u-bit, %u ch, %s", v->w.rate, v->w.bits,
             v->w.channels, play_mss(v->w.frames, v->w.rate, t, sizeof(t)));
    s->ops = &wav_ops;
    s->state = v;
    return OK;
}

/* ---- the choice --------------------------------------------------------- */

status_t play_src_open(struct play_src *s, struct jfile *f, uint64_t size, const char **why)
{
    memset(s, 0, sizeof(*s));
    s->f = f;
    return wav_open(s, size, why);
}

long play_src_read(struct play_src *s, int16_t *out, size_t frames)
{
    return s->ops->read(s, out, frames);
}

void play_src_close(struct play_src *s)
{
    if (s->ops)
        s->ops->close(s);
    s->ops = NULL;
}
