/* play: a sound file in the headphones. The file is opened as a source
 * (<play_src.h>, libos: the format is chosen by what the file starts with), which
 * hands out 16-bit frames a chunk at a time (the file is never loaded
 * whole); they are written through <audio.h>, which makes mono stereo and
 * resamples to 48 kHz. Ctrl+C stops it within a chunk, with audio_close's
 * 5 ms fade. -v sets the volume for this file only (its mixer stream's).
 * -s prints how the playing went afterwards (a line the owner can judge
 * real hardware by: underruns and late periods should be 0):
 *   play: stats: 12345 frames, 0 underruns, 0 late periods, mixer >= 128 ms ahead,
 *   ring >= 1190 ms, slowest read 3 ms, 0 limited, out 48 kHz 24-bit */
#include <audio.h>
#include <play_src.h>
#include "sh.h"

#define CHUNK 4096u   /* frames read and written at a time (93 ms at 44.1 kHz) */

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

/* The frames of src into a, CHUNK at a time; *slowest: the longest one
 * read of the file took (ns). */
static status_t stream(struct play_src *src, struct audio_out *a, uint64_t *done,
                       uint64_t *slowest)
{
    int16_t *pcm = malloc((size_t)CHUNK * src->channels * sizeof(int16_t));
    status_t st = pcm ? OK : ERR_NO_MEMORY;
    *done = 0;
    while (st == OK) {
        if (sh_interrupted()) {
            st = ERR_CANCELED;
            break;
        }
        uint64_t t0 = now();
        long n = play_src_read(src, pcm, CHUNK);
        if (now() - t0 > *slowest)
            *slowest = now() - t0;
        if (n <= 0) {
            st = n < 0 ? (status_t)n : OK;
            break;
        }
        long wrote = audio_write(a, pcm, (size_t)n);
        if (wrote < 0)
            st = (status_t)wrote;
        else
            *done += (uint64_t)n;
    }
    free(pcm);
    return st;
}

/* play -n: read the source to its end as fast as it goes, and say how
 * long that took per second of audio (an MP3's decoding cost). Returns
 * play's exit status (130 after Ctrl+C). */
static int time_src(const char *name, struct play_src *s)
{
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
    bool vol = false, stats = false, dry = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-s")) {
            stats = true;
        } else if (!strcmp(argv[i], "-n")) {
            dry = true;
        } else if (!strcmp(argv[i], "-v") && i + 1 < argc && parse_db(argv[i + 1], &cb)) {
            vol = true;
            i++;
        } else {
            i = argc;   /* usage */
            break;
        }
    }
    if (argc != i + 1) {
        sh_tty("usage: play [-s] [-v dB | -n] <file.wav|file.mp3>   (-v -20: 20 dB down; at most 0;\n"
               "       -s: how it went; -n: decode only, as fast as it goes, and say how fast)\n");
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
    struct play_src src;
    const char *why;
    st = play_src_open(&src, &f, size, &why);
    if (st != OK) {
        sh_tty("play: %s: %s\n", arg, why);
        file_close(&f);
        return 1;
    }
    char t[24];
    sh_say("play: %s: %s\n", arg, src.desc);
    sh_flush();
    if (dry) {
        int rc = time_src(arg, &src);
        play_src_close(&src);
        file_close(&f);
        return rc;
    }
    struct audio_out a;
    st = audio_open_as(&a, src.rate, src.channels, "play");
    if (st != OK) {
        if (st == ERR_NOT_FOUND)
            sh_tty("play: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
        else if (st == ERR_BAD_STATE)
            sh_tty("play: the audio output is busy: another program has its stream open\n");
        else
            sh_tty("play: can't open the audio output: %s\n", status_str(st));
        play_src_close(&src);
        file_close(&f);
        return 1;
    }
    if (vol && (st = audio_set_volume(&a, cb)) != OK)
        sh_tty("play: can't set the volume (%s): playing at `hda gain`\n", status_str(st));
    uint64_t done = 0, slowest = 0;
    st = stream(&src, &a, &done, &slowest);
    if (st == OK)
        st = audio_drain(&a);
    if (stats)
        say_stats(&a, slowest);
    audio_close(&a);
    play_src_close(&src);
    file_close(&f);
    if (st == ERR_CANCELED) {
        sh_say("play: %s: stopped at %s\n", arg, play_mss(done, src.rate, t, sizeof(t)));
        return 130;
    }
    if (st != OK) {
        sh_tty("play: %s: stopped: %s\n", arg, sh_why(st));
        return 1;
    }
    return 0;
}
