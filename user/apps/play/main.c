/* play: a sound file in the headphones, as its own program (bin/play,
 * docs/history/M8.6-SVC.md "The splits"): the shell's `play` opens the
 * file and starts this with nothing but that file and its list (the
 * mixer, /svc/audio), so a crafted file decoded here (WAV, or MP3 through
 * dr_mp3) can reach the sound output and nothing else: no namespace, no
 * console, no root. Its handles:
 *   SR_USER + 0   the open file's `file` channel   (file_give / file_adopt)
 *   SR_USER + 1   its transfer buffer
 *   SR_USER + 2   the shell's stop channel: a message (or its close) asks
 *                 it to stop, as Ctrl+C does; it fades out and says where
 *   SR_STDOUT     its lines, which the shell prints as its own
 * argv: play [-s] [-v dB | -n] <name> (the checks are the shell's; name is
 * the file as the owner typed it, for the lines).
 *
 * The file is opened as a source (<play_src.h>, libos: the format is
 * chosen by what the file starts with), which hands out 16-bit frames a
 * chunk at a time (the file is never loaded whole); they are written
 * through <audio.h>, which makes mono stereo and resamples to 48 kHz. -v
 * sets the volume for this file only (its mixer stream's). -s prints how
 * the playing went afterwards (a line the owner can judge real hardware
 * by: underruns and late periods should be 0):
 *   play: stats: 12345 frames, 0 underruns, 0 late periods, mixer >= 128 ms ahead,
 *   ring >= 1190 ms, slowest read 3 ms, 0 limited, out 48 kHz 24-bit
 * -n reads the file to its end as fast as it goes and says how long that
 * took per second of audio. Exit: 0, 1 (it can't), 130 (stopped). */
#include <audio.h>
#include <play_src.h>
#include <wants.h>

JAM_WANTS("svc audio\n");

#define CHUNK 4096u   /* frames read and written at a time (93 ms at 44.1 kHz) */
#define ROLE_FILE (SR_USER + 0)
#define ROLE_BUF  (SR_USER + 1)
#define ROLE_STOP (SR_USER + 2)

/* The shell asked us to stop (Ctrl+C), or is gone. */
static bool stop_asked(void)
{
    signals_t seen = 0;
    handle_t stop = startup_handle(ROLE_STOP);
    return stop && jam_object_wait_one(stop, SIG_READABLE | SIG_PEER_CLOSED, 0, &seen) == OK &&
           (seen & (SIG_READABLE | SIG_PEER_CLOSED));
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
        if (stop_asked()) {
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
 * long that took per second of audio (an MP3's decoding cost). */
static int time_src(const char *name, struct play_src *s)
{
    int16_t *pcm = malloc((size_t)CHUNK * s->channels * sizeof(int16_t));
    if (!pcm) {
        printf("play: %s: out of memory\n", name);
        return 1;
    }
    uint64_t frames = 0, t0 = now();
    long n = 0;
    bool stopped = false;
    while (!(stopped = stop_asked()) && (n = play_src_read(s, pcm, CHUNK)) > 0)
        frames += (uint64_t)n;
    uint64_t ns = now() - t0;
    free(pcm);
    if (n < 0) {
        printf("play: %s: stopped: %s\n", name, status_str((status_t)n));
        return 1;
    }
    /* Microseconds of decoding per second of audio. */
    uint64_t us = frames ? ns * s->rate / frames / 1000 : 0;
    char t[24];
    printf("play: %s: %s%s (%lu frames) decoded in %lu.%03lu s: %lu.%03lu ms per second of "
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
        printf("play: stats: %s\n", status_str(r));
        return;
    }
    char lead[24] = "-", ring[24] = "-", out[24] = "closed";
    if (st.min_lead != UINT32_MAX)
        snprintf(lead, sizeof(lead), "%u", (unsigned)(st.min_lead * 1000ull / AUDIO_RATE));
    if (st.ring_min != UINT64_MAX)
        snprintf(ring, sizeof(ring), "%lu", (unsigned long)(st.ring_min * 1000 / AUDIO_RATE));
    if (st.bits)
        snprintf(out, sizeof(out), "48 kHz %u-bit", st.bits);
    printf("play: stats: %lu frames, %u underruns, %u late periods, mixer >= %s ms ahead, "
           "ring >= %s ms, slowest read %lu ms, %u limited, out %s\n",
           (unsigned long)st.played, st.underruns, st.late, lead, ring,
           (unsigned long)(slowest / NS_PER_MS), st.limited, out);
}

/* The output for src; false (said why) if there is none. */
static bool open_output(const char *name, struct play_src *src, bool vol, int cb,
                        struct audio_out *a)
{
    status_t st = audio_open_as(a, src->rate, src->channels, "play");
    if (st == ERR_NOT_FOUND)
        printf("play: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
    else if (st == ERR_BAD_STATE)
        printf("play: the audio output is busy: another program has its stream open\n");
    else if (st != OK)
        printf("play: can't open the audio output: %s\n", status_str(st));
    if (st != OK)
        return false;
    if (vol && (st = audio_set_volume(a, cb)) != OK)
        printf("play: %s: can't set the volume (%s): playing at `hda gain`\n", name,
               status_str(st));
    return true;
}

/* Play src to its end (or a stop); play's exit status. */
static int play(const char *name, struct play_src *src, bool vol, int cb, bool stats)
{
    struct audio_out a;
    if (!open_output(name, src, vol, cb, &a))
        return 1;
    uint64_t done = 0, slowest = 0;
    status_t st = stream(src, &a, &done, &slowest);
    if (st == OK)
        st = audio_drain(&a);
    if (stats)
        say_stats(&a, slowest);
    audio_close(&a);   /* what the mixer hasn't taken fades out */
    char t[24];
    if (st == ERR_CANCELED) {
        printf("play: %s: stopped at %s\n", name, play_mss(done, src->rate, t, sizeof(t)));
        return 130;
    }
    if (st != OK) {
        printf("play: %s: stopped: %s\n", name, status_str(st));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int cb = 0, i = 1;
    bool vol = false, stats = false, dry = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-s")) {
            stats = true;
        } else if (!strcmp(argv[i], "-n")) {
            dry = true;
        } else if (!strcmp(argv[i], "-v") && i + 1 < argc && audio_parse_db(argv[i + 1], &cb)) {
            vol = true;
            i++;
        } else {
            break;
        }
    }
    struct jfile f;
    if (argc != i + 1 || !startup_handle(ROLE_FILE) ||
        file_adopt(startup_handle(ROLE_FILE), startup_handle(ROLE_BUF), FS_READ, &f) != OK) {
        printf("play: started without its file: the shell's `play <file>` starts it\n");
        return 2;
    }
    const char *name = argv[i], *why;
    struct play_src src;
    status_t st = play_src_open(&src, &f, f.size, &why);
    if (st != OK) {
        printf("play: %s: %s\n", name, why);
        file_close(&f);
        return 1;
    }
    printf("play: %s: %s\n", name, src.desc);
    int rc = dry ? time_src(name, &src) : play(name, &src, vol, cb, stats);
    play_src_close(&src);
    file_close(&f);
    return rc;
}
