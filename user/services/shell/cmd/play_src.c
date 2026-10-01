/* play_src: `play`'s sources (play_src.h). A WAV file: the header with
 * <wav.h>, then the samples straight from the file, 8-, 24- and 32-bit
 * ones made 16-bit. An MPEG audio file (MP3, and MP2/MP1): decoded by
 * <mp3.h> (dr_mp3), which reads the file 64 KiB at a time. A file is a
 * WAV file if it starts with "RIFF", else an MP3 if mp3_sniff finds
 * frames (after any ID3v2 tag), else neither. */
#include <audio.h>
#include <mp3.h>
#include <wav.h>
#include "play_src.h"
#include "sh.h"

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

/* ---- MPEG audio ----------------------------------------------------------- */

static long mp3_src_read(struct play_src *s, int16_t *out, size_t frames)
{
    return mp3_decode(s->state, out, frames);
}

static void mp3_src_close(struct play_src *s)
{
    mp3_close(s->state);
    free(s->state);
}

static const struct play_src_ops mp3_ops = { mp3_src_read, mp3_src_close };

/* ERR_WRONG_TYPE: not MPEG audio either. */
static status_t mp3_src_open(struct play_src *s, uint64_t size, const char **why)
{
    struct mp3 *m = malloc(sizeof(*m));
    if (!m) {
        *why = "out of memory";
        return ERR_NO_MEMORY;
    }
    status_t st = mp3_open(m, read_file, s->f, size, why);
    if (st != OK) {
        free(m);
        return st;
    }
    const struct mp3_info *i = &m->info;
    s->rate = i->rate;
    s->channels = i->channels;
    s->frames = i->frames;
    char kbps[16], t[24];
    if (i->vbr)
        snprintf(kbps, sizeof(kbps), "VBR");
    else if (i->kbps)
        snprintf(kbps, sizeof(kbps), "%u kbps", i->kbps);
    else
        snprintf(kbps, sizeof(kbps), "free format");
    snprintf(s->desc, sizeof(s->desc), "MP%u, %u Hz, %u ch, %s, %s", i->layer, i->rate,
             i->channels, kbps, i->frames ? play_mss(i->frames, i->rate, t, sizeof(t)) : "?");
    s->ops = &mp3_ops;
    s->state = m;
    return OK;
}

/* ---- play -n ------------------------------------------------------------- */

int play_src_time(const char *name, struct play_src *s)
{
    enum { CHUNK = 4096 };
    int16_t *pcm = malloc((size_t)CHUNK * s->channels * sizeof(int16_t));
    if (!pcm) {
        sh_tty("play: %s: out of memory\n", name);
        return 1;
    }
    uint64_t frames = 0, t0 = now();
    long n = 0;
    bool stopped = false;
    while (!(stopped = sh_interrupted()) && (n = play_src_read(s, pcm, CHUNK)) > 0)
        frames += (uint64_t)n;
    uint64_t ns = now() - t0;
    free(pcm);
    if (n < 0) {
        sh_tty("play: %s: stopped: %s\n", name, sh_why((status_t)n));
        return 1;
    }
    /* Microseconds of decoding per second of audio. */
    uint64_t us = frames ? ns * s->rate / frames / 1000 : 0;
    char t[24];
    sh_say("play: %s: %s%s (%lu frames) decoded in %lu.%03lu s: %lu.%03lu ms per second of "
           "audio\n", name, stopped ? "stopped: " : "", play_mss(frames, s->rate, t, sizeof(t)),
           (unsigned long)frames, (unsigned long)(ns / NS_PER_S),
           (unsigned long)(ns % NS_PER_S / NS_PER_MS), (unsigned long)(us / 1000),
           (unsigned long)(us % 1000));
    return stopped ? 130 : 0;
}

/* ---- the choice --------------------------------------------------------- */

status_t play_src_open(struct play_src *s, struct jfile *f, uint64_t size, const char **why)
{
    memset(s, 0, sizeof(*s));
    s->f = f;
    char magic[4];
    size_t got = 0;
    status_t st = file_read(f, 0, magic, sizeof(magic), &got);
    if (st != OK) {
        *why = "can't read it";
        return st;
    }
    if (got == sizeof(magic) && !memcmp(magic, "RIFF", 4))
        return wav_open(s, size, why);
    st = mp3_src_open(s, size, why);
    if (st == ERR_WRONG_TYPE)
        *why = "not a WAV or MP3 file (no RIFF/WAVE header, no MPEG audio frames)";
    return st;
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
