/* beep: a tone in the headphones (docs/A1-PLAN.md, stage 3). A sine with
 * a 5 ms fade in and out (no click), made here in floating point, written
 * through <audio.h> as 48 kHz mono (the library makes it stereo and keeps
 * the device's ring written ahead); the device plays at its gain (`hda
 * gain`) and its path is unmuted only while the stream runs. Ctrl+C stops
 * it within a period, with audio_close's 5 ms fade. */
#include <audio.h>
#include "sh.h"

#define RATE       48000u
#define AMPLITUDE  8192.0     /* -12 dBFS: a quarter of full scale, before the codec's gain */
#define FADE       240u       /* frames: 5 ms */
#define MAX_MS     5000u
#define BLOCK      1024u      /* frames written at a time */

/* sin and cos of x in [-pi, pi] by their series (x^25 and x^24 terms are
 * below 1e-15 there): enough for one oscillator step, set up once. */
static void sincos_(double x, double *s, double *c)
{
    double term = x, sum = x;
    for (int k = 1; k < 13; k++) {
        term *= -x * x / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    *s = sum;
    term = 1;
    sum = 1;
    for (int k = 1; k < 13; k++) {
        term *= -x * x / ((2 * k - 1) * (2 * k));
        sum += term;
    }
    *c = sum;
}

/* The tone: an oscillator turned by the angle of one frame, with the
 * fades as its envelope. Frames come in order (next()). */
struct tone {
    double   re, im;          /* e^(i * phase) */
    double   step_re, step_im;
    uint64_t at, frames;      /* the next frame, and how many there are */
    uint32_t fade;
};

static void tone_init(struct tone *t, uint32_t hz, uint64_t frames)
{
    const double pi = 3.14159265358979323846;
    sincos_(2 * pi * hz / RATE, &t->step_im, &t->step_re);
    t->re = 1;
    t->im = 0;
    t->at = 0;
    t->frames = frames;
    t->fade = frames / 2 < FADE ? (uint32_t)(frames / 2) : FADE;
}

static int16_t next(struct tone *t)
{
    uint64_t f = t->at++;
    double env = 1;
    if (f < t->fade)
        env = (double)f / t->fade;
    else if (t->frames - 1 - f < t->fade)
        env = (double)(t->frames - 1 - f) / t->fade;
    double v = AMPLITUDE * env * t->im;
    double re = t->re * t->step_re - t->im * t->step_im;
    t->im = t->re * t->step_im + t->im * t->step_re;
    t->re = re;
    if ((f & 1023) == 1023) {   /* keep |z| at 1: rounding would drift it */
        double g = 1.5 - 0.5 * (t->re * t->re + t->im * t->im);
        t->re *= g;
        t->im *= g;
    }
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

static bool number(const char *s, uint64_t lo, uint64_t hi, uint32_t *out)
{
    uint64_t v;
    if (!sh_parse_u64(s, &v) || v < lo || v > hi)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Play t, BLOCK frames at a time (Ctrl+C is looked at between them), then
 * wait until it and a period of silence have played. */
static status_t play(struct audio_out *a, struct tone *t)
{
    static int16_t buf[BLOCK];
    while (t->at < t->frames) {
        if (sh_interrupted())
            return ERR_CANCELED;
        size_t n = 0;
        while (n < BLOCK && t->at < t->frames)
            buf[n++] = next(t);
        long w = audio_write(a, buf, n);
        if (w < 0)
            return (status_t)w;
    }
    return audio_drain(a);
}

SH_CMD(beep)
{
    uint32_t hz = 440, ms = 300;
    if (argc > 3 || (argc > 1 && !number(argv[1], 20, 20000, &hz)) ||
        (argc > 2 && !number(argv[2], 10, MAX_MS, &ms))) {
        sh_tty("usage: beep [hz] [ms]   (20-20000 Hz, default 440; 10-%u ms, default 300)\n",
               MAX_MS);
        return 2;
    }
    struct audio_out a;
    status_t st = audio_open_as(&a, RATE, 1, "beep");
    if (st == ERR_NOT_FOUND) {
        sh_say("beep: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
        return 1;
    }
    if (st == ERR_BAD_STATE) {
        sh_say("beep: the audio output is busy: another program has its stream open\n");
        return 1;
    }
    if (st != OK) {
        sh_say("beep: can't open the audio output: %s\n", status_str(st));
        return 1;
    }
    struct tone t;
    tone_init(&t, hz, (uint64_t)ms * RATE / 1000);
    st = play(&a, &t);
    if (st != OK && st != ERR_CANCELED)
        sh_say("beep: playing: %s (the log's \"[hda] output:\" lines say why)\n", status_str(st));
    int gain = 0;
    bool have_gain = st == OK && audio_get_volume(&a, &gain) == OK;
    audio_close(&a);   /* stops (mutes) and releases the stream */
    if (have_gain) {
        uint32_t g = gain < 0 ? (uint32_t)-gain : (uint32_t)gain;
        sh_say("beep: %u Hz for %u ms at %s%u.%u dB\n", hz, ms, gain < 0 ? "-" : "", g / 10, g % 10);
    } else if (st == OK) {
        sh_say("beep: %u Hz for %u ms\n", hz, ms);
    }
    return st == OK ? 0 : st == ERR_CANCELED ? 130 : 1;
}
