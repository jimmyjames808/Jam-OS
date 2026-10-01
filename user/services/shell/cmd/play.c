/* play: a sound file in the headphones. The file is opened as a source
 * (play_src.h: the format is chosen by what the file starts with), which
 * hands out 16-bit frames a chunk at a time (the file is never loaded
 * whole); they are written through <audio.h>, which makes mono stereo and
 * resamples to 48 kHz. Ctrl+C stops it within a chunk, with audio_close's
 * 5 ms fade. -v sets the volume for this file only (its mixer stream's). */
#include <audio.h>
#include "play_src.h"
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

/* The frames of src into a, CHUNK at a time. */
static status_t stream(struct play_src *src, struct audio_out *a, uint64_t *done)
{
    int16_t *pcm = malloc((size_t)CHUNK * src->channels * sizeof(int16_t));
    status_t st = pcm ? OK : ERR_NO_MEMORY;
    *done = 0;
    while (st == OK) {
        if (sh_interrupted()) {
            st = ERR_CANCELED;
            break;
        }
        long n = play_src_read(src, pcm, CHUNK);
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

SH_CMD(play)
{
    int cb = 0, i = 1;
    bool vol = argc > 1 && !strcmp(argv[1], "-v");
    if (vol)
        i = 3;
    if (argc != i + 1 || (vol && !parse_db(argv[2], &cb))) {
        sh_tty("usage: play [-v dB] <file.wav>   (-v -20: 20 dB down; at most 0)\n");
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
    uint64_t done = 0;
    st = stream(&src, &a, &done);
    if (st == OK)
        st = audio_drain(&a);
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
