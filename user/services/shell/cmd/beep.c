/* beep: a tone in the headphones (docs/A1-PLAN.md, stage 3). It opens
 * the hda driver's output stream (abi/idl/hda.idl), writes a sine into
 * the ring with a 5 ms fade in and out (no click), starts the stream
 * (which unmutes the path at the driver's gain: `hda gain`), keeps a
 * ring's worth written ahead of the play position with wait_period until
 * the tone and a period of silence after it have played, then stops
 * (which mutes the path again) and closes. The samples are made here, in
 * floating point; the driver never makes sound of its own. Ctrl+C stops
 * it at once. */
#include <idl/hda.h>
#include "sh.h"

#define RATE       48000u
#define AMPLITUDE  8192.0     /* -12 dBFS: a quarter of full scale, before the codec's gain */
#define FADE       240u       /* frames: 5 ms */
#define MAX_MS     5000u
#define OPEN_WAIT  (15 * NS_PER_S)
#define SOON       (5 * NS_PER_S)

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

/* The tone's frames [from, to) into the ring of `ring` frames. */
static void fill(struct tone *t, int16_t *ring, uint64_t ring_frames, uint64_t to)
{
    if (to > t->frames)
        to = t->frames;
    while (t->at < to) {
        uint64_t i = t->at % ring_frames;
        int16_t v = next(t);
        ring[2 * i] = v;
        ring[2 * i + 1] = v;
    }
}

static bool number(const char *s, uint64_t lo, uint64_t hi, uint32_t *out)
{
    uint64_t v;
    if (!sh_parse_u64(s, &v) || v < lo || v > hi)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Play t on the open stream (channel ch, ring mapped at va). */
static status_t play(handle_t ch, int16_t *ring, uint32_t size, uint32_t period, struct tone *t)
{
    uint64_t ring_frames = size / 4, tail = period / 4;
    fill(t, ring, ring_frames, ring_frames);
    status_t st = hda_start_until(ch, now() + SOON);
    if (st != OK) {
        sh_say("beep: the stream did not start (%s): the path stays muted; the log's "
               "\"[hda] output:\" lines say why\n", status_str(st));
        return st;
    }
    uint64_t frames = 0;
    uint32_t off;
    while (frames < t->frames + tail) {
        if (sh_interrupted()) {
            st = ERR_CANCELED;
            break;
        }
        st = hda_wait_period_until(ch, now() + SOON, frames, &frames, &off);
        if (st != OK) {
            sh_say("beep: waiting for the stream: %s\n", status_str(st));
            break;
        }
        fill(t, ring, ring_frames, frames + ring_frames);
    }
    status_t stop = hda_stop_until(ch, now() + SOON);
    if (stop != OK && st == OK) {
        sh_say("beep: stopping the stream: %s\n", status_str(stop));
        st = stop;
    }
    return st;
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
    handle_t hda = sh_hda();
    if (hda == HANDLE_INVALID) {
        sh_say("beep: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
        return 1;
    }
    handle_t ch, vmo;
    uint32_t size = 0, period = 0;
    status_t st = hda_open_output_until(hda, now() + OPEN_WAIT, RATE, 2, 16, &ch, &vmo, &size,
                                        &period);
    if (st == ERR_BAD_STATE) {
        sh_say("beep: the audio output is busy: another program has its stream open\n");
        jam_handle_close(hda);
        return 1;
    }
    if (st != OK) {
        sh_say("beep: can't open the audio output: %s\n", status_str(st));
        jam_handle_close(hda);
        return 1;
    }
    uint64_t va = 0;
    st = size && period && size % period == 0
        ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, VMAR_READ | VMAR_WRITE, &va)
        : ERR_BAD_STATE;
    if (st == OK) {
        struct tone t;
        tone_init(&t, hz, (uint64_t)ms * RATE / 1000);
        st = play(ch, (int16_t *)(uintptr_t)va, size, period, &t);
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), va, size);
    } else {
        sh_say("beep: can't map the ring: %s\n", status_str(st));
    }
    jam_handle_close(vmo);
    jam_handle_close(ch);   /* the driver stops (mutes) and releases the stream */
    if (st == OK) {
        int32_t gain = 0, min, max;
        uint32_t step;
        if (hda_get_gain_until(hda, now() + SOON, &gain, &step, &min, &max) == OK) {
            uint32_t a = gain < 0 ? (uint32_t)-gain : (uint32_t)gain;
            sh_say("beep: %u Hz for %u ms at %s%u.%u dB\n", hz, ms, gain < 0 ? "-" : "", a / 10,
                   a % 10);
        } else {
            sh_say("beep: %u Hz for %u ms\n", hz, ms);
        }
    }
    jam_handle_close(hda);
    return st == OK ? 0 : st == ERR_CANCELED ? 130 : 1;
}
