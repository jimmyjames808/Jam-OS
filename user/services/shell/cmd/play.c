/* play: a WAV file in the headphones. The header is read with <wav.h>
 * (chunks skipped until "fmt " and "data"), then the samples a chunk at a
 * time (the file is never loaded whole), turned into 16-bit if they are 8,
 * 24 or 32, and written through <audio.h>, which makes mono stereo and
 * resamples to 48 kHz. Ctrl+C stops it within a chunk, with audio_close's
 * 5 ms fade. -v sets the volume for this file only (its mixer stream's).
 * -s prints how the playing went afterwards (a line the owner can judge
 * real hardware by: underruns and late periods should be 0):
 *   play: stats: 12345 frames, 0 underruns, 0 late periods, mixer >= 128 ms ahead,
 *   ring >= 1190 ms, slowest read 3 ms, 0 limited, out 48 kHz 24-bit */
#include <audio.h>
#include <wav.h>
#include "sh.h"

#define CHUNK 4096u   /* frames read and written at a time (93 ms at 44.1 kHz) */

static status_t read_file(void *ctx, uint64_t offset, void *dst, size_t n, size_t *got)
{
    return file_read(ctx, offset, dst, n, got);
}

/* "-20", "-20.5", "0": dB into centibels (at most 100 dB either way). */
static bool parse_db(const char *s, int *cb)
{
    bool neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    int v = 0;
    bool digits = false;
    for (; *s >= '0' && *s <= '9'; s++, digits = true)
        if ((v = v * 10 + (*s - '0')) > 100)
            return false;
    v *= 10;
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') {
        v += s[1] - '0';
        s += 2;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    if (!digits || *s)
        return false;
    *cb = neg ? -v : v;
    return true;
}

/* "m:ss" of frames at rate, rounded to the nearest second. */
static const char *mss(uint64_t frames, uint32_t rate, char *buf, size_t size)
{
    uint64_t s = (frames + rate / 2) / rate;
    snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    return buf;
}

/* The samples of w from f into a, CHUNK frames at a time; *slowest: the
 * longest one file_read took (ns). */
static status_t stream(struct jfile *f, const struct wav_info *w, struct audio_out *a,
                       uint64_t *done, uint64_t *slowest)
{
    uint8_t *raw = malloc((size_t)CHUNK * w->frame_bytes);
    int16_t *pcm = w->bits == 16 ? NULL : malloc((size_t)CHUNK * w->channels * 2);
    status_t st = raw && (w->bits == 16 || pcm) ? OK : ERR_NO_MEMORY;
    *done = 0;
    while (st == OK && *done < w->frames) {
        if (sh_interrupted()) {
            st = ERR_CANCELED;
            break;
        }
        uint64_t n = w->frames - *done;
        if (n > CHUNK)
            n = CHUNK;
        size_t got = 0;
        uint64_t t0 = now();
        st = file_read(f, w->data_offset + *done * w->frame_bytes, raw, n * w->frame_bytes, &got);
        if (now() - t0 > *slowest)
            *slowest = now() - t0;
        if (st != OK)
            break;
        n = got / w->frame_bytes;
        if (!n)
            break;   /* the file got shorter meanwhile */
        size_t samples = n * w->channels;
        const void *frames = raw;
        if (w->bits == 8)
            audio_s16_from_u8(pcm, raw, samples);
        else if (w->bits == 24)
            audio_s16_from_s24le(pcm, raw, samples);
        else if (w->bits == 32)
            audio_s16_from_s32le(pcm, raw, samples);
        if (pcm)
            frames = pcm;
        long wrote = audio_write(a, frames, n);
        if (wrote < 0)
            st = (status_t)wrote;
        else
            *done += n;
    }
    free(pcm);
    free(raw);
    return st;
}

/* The -s line (see the top). */
static void say_stats(struct audio_out *a, uint64_t slowest)
{
    struct audio_stats st;
    status_t r = audio_stats(a, &st);
    if (r != OK) {
        sh_say("play: stats: %s\n", status_str(r));
        return;
    }
    char lead[24] = "-", ring[24] = "-", out[24] = "closed";
    if (st.min_lead != UINT32_MAX)
        snprintf(lead, sizeof(lead), "%u", (unsigned)(st.min_lead * 1000ull / AUDIO_RATE));
    if (st.ring_min != UINT64_MAX)
        snprintf(ring, sizeof(ring), "%lu", (unsigned long)(st.ring_min * 1000 / AUDIO_RATE));
    if (st.bits)
        snprintf(out, sizeof(out), "48 kHz %u-bit", st.bits);
    sh_say("play: stats: %lu frames, %u underruns, %u late periods, mixer >= %s ms ahead, "
           "ring >= %s ms, slowest read %lu ms, %u limited, out %s\n",
           (unsigned long)st.played, st.underruns, st.late, lead, ring,
           (unsigned long)(slowest / NS_PER_MS), st.limited, out);
}

SH_CMD(play)
{
    int cb = 0, i = 1;
    bool vol = false, stats = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-s")) {
            stats = true;
        } else if (!strcmp(argv[i], "-v") && i + 1 < argc && parse_db(argv[i + 1], &cb)) {
            vol = true;
            i++;
        } else {
            i = argc;   /* usage */
            break;
        }
    }
    if (argc != i + 1) {
        sh_tty("usage: play [-s] [-v dB] <file.wav>   (-v -20: 20 dB down, at most 0; -s: "
               "how it went)\n");
        return 2;
    }
    const char *arg = argv[i];
    char abs[SH_PATH_MAX];
    bool dir = false;
    uint64_t size = 0;
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? sh_stat(abs, &dir, &size) : ERR_NOT_FOUND;
    if (st == ERR_NOT_FOUND) {
        sh_tty("play: %s: no such file\n", arg);
        return 1;
    }
    if (st == OK && dir) {
        sh_tty("play: %s: is a directory\n", arg);
        return 1;
    }
    struct jfile f;
    if (st == OK)
        st = file_open(abs, FS_READ, &f);
    if (st != OK) {
        sh_tty("play: %s: can't read it (%s)\n", arg, sh_why(st));
        return 1;
    }
    struct wav_info w;
    const char *why;
    st = wav_parse(&w, read_file, &f, size, &why);
    if (st != OK) {
        sh_tty("play: %s: %s\n", arg, why);
        file_close(&f);
        return 1;
    }
    char t[24];
    sh_say("play: %s: %u Hz, %u-bit, %u ch, %s\n", arg, w.rate, w.bits, w.channels,
           mss(w.frames, w.rate, t, sizeof(t)));
    sh_flush();
    struct audio_out a;
    st = audio_open_as(&a, w.rate, w.channels, "play");
    if (st != OK) {
        if (st == ERR_NOT_FOUND)
            sh_tty("play: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
        else if (st == ERR_BAD_STATE)
            sh_tty("play: the audio output is busy: another program has its stream open\n");
        else
            sh_tty("play: can't open the audio output: %s\n", status_str(st));
        file_close(&f);
        return 1;
    }
    if (vol && (st = audio_set_volume(&a, cb)) != OK)
        sh_tty("play: can't set the volume (%s): playing at `hda gain`\n", status_str(st));
    uint64_t done = 0, slowest = 0;
    st = stream(&f, &w, &a, &done, &slowest);
    if (st == OK)
        st = audio_drain(&a);
    if (stats)
        say_stats(&a, slowest);
    audio_close(&a);
    file_close(&f);
    if (st == ERR_CANCELED) {
        sh_say("play: %s: stopped at %s\n", arg, mss(done, w.rate, t, sizeof(t)));
        return 130;
    }
    if (st != OK) {
        sh_tty("play: %s: stopped: %s\n", arg, sh_why(st));
        return 1;
    }
    return 0;
}
