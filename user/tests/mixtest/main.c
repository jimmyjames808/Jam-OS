/* mixtest: the mixer (user/services/mixer, abi/idl/audio.idl and
 * audioctl.idl) from user space. The shell's `mixtest` runs it with what
 * its list asks for: the mixer's channels (/svc/audio, /svc/audioctl) and
 * init's control channel (/svc/init: it kills the mixer and the hda
 * driver once);
 * tools/mixer-test.sh runs it in QEMU with the codec's samples going to a
 * WAV file and checks the sound there.
 *
 * Every sound comes from a child process, `mixtest tone <name> <hz> <ms>
 * <centibels> [pause]`, a separate program with only /svc/audio in its
 * namespace (as play is given): a sine at a quarter of full scale with
 * 5 ms fades, written ahead with mixer_write, drained, closed; every call
 * must succeed. `pause`: after 300 ms of tone it waits 2 s with nothing
 * written, then plays the rest. Phases,
 * each a sound of its own with silence between (the WAV's segments):
 *   protocol       no sound: formats refused, the stream methods refused
 *                  on /svc/audio, drain on a stopped stream, volumes
 *                  clamped, an unknown id
 *   openers        no sound: two openers of /svc/audio, and of
 *                  /svc/audioctl, each get a channel of their own, and
 *                  what one asks is answered on its channel alone
 *   stream_cap     no sound: an opener's fifth stream refused, other
 *                  openers' up to 16 in all, the 17th refused; one closed
 *                  frees a stream for another opener, not for one at its
 *                  cap
 *   desk           no sound: the desktop's authority (desk_channel: what
 *                  the compositor's channel has): desk answers the master
 *                  volume, the output's name (QEMU's codec) and what
 *                  plays (a stream of silence titled with
 *                  stream_set_title), set_master works and the full
 *                  channel sees it; everything else is refused, a
 *                  desk_channel of it too
 *   device         no sound: audioctl.device's query channel to the hda
 *                  driver answers info and the gain but refuses
 *                  open_output and query (so nobody but the mixer can take
 *                  the one output stream); at most 8 at once
 *   two_at_once    440 Hz and 1000 Hz (the second at -6 dB) from two
 *                  programs at once; both are listed while they play
 *   client_killed  the same, and the 1000 Hz program killed mid-tone: the
 *                  other plays to its end; its stream is dropped
 *   master         440 Hz with the master at -6 dB
 *   ctl_volume     440 Hz, turned to -12 dB through audioctl mid-tone
 *   library_at_once  two programs on <audio.h> (beep's and play's
 *                  library) at once: 1000 Hz at 44.1 kHz (resampled) and
 *                  440 Hz at 48 kHz, each a stream named for `vol`
 *   mixer_killed   init kills the mixer mid-tone: the program never
 *                  knows (its restart adopts the stream), plays it all
 *   driver_killed  init kills the hda driver mid-tone: the mixer opens
 *                  the output again on its restart, the stream plays on
 *   idle_wakes     a playing stream left empty: listed idle (the mixer
 *                  closed the output), then its next write wakes it
 * and at the end no stream is left. The summary goes to the RESULTS box;
 * exit 0 if nothing failed. */
#define CHECK_PROG "mixtest"
#define CHECK_CUR  cur
#include <audio.h>
#include <check.h>
#include <idl/audio.h>
#include <idl/audioctl.h>
#include <idl/hda.h>
#include <idl/initctl.h>
#include <mixer.h>
#include <os.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc audio\n"
          "svc audioctl\n"
          "svc init\n");

#define AMPLITUDE 8192.0             /* -12 dBFS */
#define FADE      240u               /* frames: 5 ms */
#define CHUNK     1024u              /* frames written at a time */
#define SOON      (5 * NS_PER_S)
#define LONG      (20 * NS_PER_S)    /* a whole tone, a restart included */
#define GAP_MS    400u               /* silence between the phases */

static const char *cur;
static handle_t initctl;

/* Our channels to the mixer: libos keeps one per name and opens it again
 * once the mixer it was from has gone (each is an opener's own, which ends
 * with the mixer that made it). */
static handle_t svc(void) { return svc_get(SVC_AUDIO); }
static handle_t ctl(void) { return svc_get(SVC_AUDIOCTL); }

/* ---- the tone child ------------------------------------------------------------- */

/* sin and cos of x in [-pi, pi] by their series. */
static void sin_cos(double x, double *s, double *c)
{
    double ts = x, ss = x, tc = 1, sc = 1;
    for (int k = 1; k < 13; k++) {
        ts *= -x * x / ((2 * k) * (2 * k + 1));
        ss += ts;
        tc *= -x * x / ((2 * k - 1) * (2 * k));
        sc += tc;
    }
    *s = ss;
    *c = sc;
}

struct tone {
    double   re, im, sre, sim;   /* the phasor and its step */
    uint64_t at, frames;
};

static void tone_next(struct tone *t, int16_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, t->at++) {
        uint64_t f = t->at, left = t->frames - 1 - f;
        double env = f < FADE ? (double)f / FADE : left < FADE ? (double)left / FADE : 1;
        double v = AMPLITUDE * env * t->im;
        double re = t->re * t->sre - t->im * t->sim;
        t->im = t->re * t->sim + t->im * t->sre;
        t->re = re;
        if ((f & 1023) == 1023) {
            double g = 1.5 - 0.5 * (t->re * t->re + t->im * t->im);
            t->re *= g;
            t->im *= g;
        }
        out[2 * i] = out[2 * i + 1] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
    }
}

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    bool neg = *s == '-';
    for (s += neg; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (uint32_t)(*s - '0');
    return neg ? (uint32_t)-(int32_t)v : v;
}

static status_t tone_open(const char *name, int32_t cb, struct mixer_stream *s)
{
    status_t st = mixer_open(svc(), name, now() + LONG, s);
    if (st == OK && cb)
        st = mixer_set_volume(s, cb, now() + SOON, NULL);
    return st;
}

/* "mixtest tone <name> <hz> <ms> <cb> [pause]": the exit code says how
 * it went (0 played it all as asked). */
static int tone_main(int argc, char **argv)
{
    const char *name = argv[2];
    bool pause = false;
    for (int i = 6; i < argc; i++)
        pause |= !strcmp(argv[i], "pause");
    struct tone t = { .re = 1, .frames = (uint64_t)number(argv[4]) * MIXER_RATE / 1000 };
    sin_cos(2 * 3.14159265358979323846 * number(argv[3]) / MIXER_RATE, &t.sim, &t.sre);
    struct mixer_stream s;
    status_t st = tone_open(name, (int32_t)number(argv[5]), &s);
    int16_t buf[2 * CHUNK];
    while (st == OK && t.at < t.frames) {
        if (pause && t.at == 300 * MIXER_RATE / 1000) {
            st = mixer_drain(&s, now() + LONG);   /* all of it taken and heard */
            (void)jam_nanosleep(now() + 2 * NS_PER_S);
            if (st != OK)
                break;
        }
        uint64_t n = t.frames - t.at < CHUNK ? t.frames - t.at : CHUNK;
        if (pause && t.at < 300 * MIXER_RATE / 1000 && t.at + n > 300 * MIXER_RATE / 1000)
            n = 300 * MIXER_RATE / 1000 - t.at;
        tone_next(&t, buf, (uint32_t)n);
        size_t done = 0;
        st = mixer_write(&s, buf, (size_t)n, now() + LONG, &done);
    }
    if (st == OK)
        st = mixer_drain(&s, now() + LONG);
    printf("mixtest: tone %s: %lu frames, %s\n", name, (unsigned long)t.at, status_str(st));
    mixer_close(&s);
    return st != OK ? 1 : 0;
}

/* "mixtest libtone <name> <rate> <hz> <ms>": a mono tone at `rate` Hz
 * through <audio.h>, the library beep and play use (it resamples to the
 * mixer's 48 kHz), drained and closed. */
static int libtone_main(char **argv)
{
    unsigned rate = number(argv[3]);
    struct tone t = { .re = 1, .frames = (uint64_t)number(argv[5]) * rate / 1000 };
    sin_cos(2 * 3.14159265358979323846 * number(argv[4]) / rate, &t.sim, &t.sre);
    struct audio_out a;
    int st = audio_open_as(&a, rate, 1, argv[2]);
    int16_t buf[2 * CHUNK], mono[CHUNK];
    while (st == OK && t.at < t.frames) {
        uint32_t n = t.frames - t.at < CHUNK ? (uint32_t)(t.frames - t.at) : CHUNK;
        tone_next(&t, buf, n);
        for (uint32_t i = 0; i < n; i++)
            mono[i] = buf[2 * i];
        long w = audio_write(&a, mono, n);
        st = w < 0 ? (int)w : OK;
    }
    if (st == OK)
        st = audio_drain(&a);
    printf("mixtest: libtone %s: %lu frames at %u Hz, %s\n", argv[2], (unsigned long)t.at,
           rate, status_str(st));
    audio_close(&a);
    return st == OK ? 0 : 1;
}

/* ---- the parent ------------------------------------------------------------------ */

static uint64_t soon(void) { return now() + SOON; }

/* What a tone child is given: the mixer and nothing else. */
static const char *const audio_only[] = { "/svc/" SVC_AUDIO, NULL };

/* Start "mixtest tone ..." with /svc/audio only. */
static status_t tone(const char *name, const char *hz, const char *ms, const char *cb,
                     const char *opt, handle_t *proc)
{
    const char *argv[] = { "bin/mixtest", "tone", name, hz, ms, cb, opt, NULL };
    struct spawn_args sa = { .path = "bin/mixtest", .argc = opt ? 7 : 6, .argv = argv,
                             .job = startup_handle(SR_JOB), .ns = audio_only };
    return spawn(&sa, proc);
}

/* Wait for proc to end: its exit code, or -1000 (killed) / -1001 (lost). */
static long finish(handle_t proc)
{
    struct process_info info;
    status_t st = spawn_wait(proc, LONG, &info);
    jam_handle_close(proc);
    return st != OK ? -1001 : info.killed ? -1000 : (long)info.exit_code;
}

static void pause_ms(uint64_t ms)
{
    (void)jam_nanosleep(now() + ms * NS_PER_MS);
}

/* The mixer's list now; *n entries into e. */
static status_t streams(struct mixer_stream_info *e, uint32_t *n, int32_t *master)
{
    uint8_t raw[640];
    status_t st = audioctl_streams_until(ctl(), soon(), n, master, raw);
    if (st == OK && *n <= MIXER_MAX_STREAMS)
        memcpy(e, raw, *n * sizeof(*e));
    return st == OK && *n > MIXER_MAX_STREAMS ? ERR_INTERNAL : st;
}

/* The listed stream called name (at volume `vol`, unless it is 1), within
 * `ms`: true and *out. */
static bool find_at(const char *name, int32_t vol, uint64_t ms, struct mixer_stream_info *out)
{
    uint64_t until = now() + ms * NS_PER_MS;
    do {
        struct mixer_stream_info e[MIXER_MAX_STREAMS];
        uint32_t n = 0;
        int32_t m;
        if (streams(e, &n, &m) == OK)
            for (uint32_t i = 0; i < n; i++)
                if (!strcmp(e[i].name, name) && (vol == 1 || e[i].volume == vol)) {
                    *out = e[i];
                    return true;
                }
        pause_ms(20);
    } while (now() < until);
    return false;
}

static bool find_stream(const char *name, uint64_t ms, struct mixer_stream_info *out)
{
    return find_at(name, 1, ms, out);
}

/* GAP_MS of silence played through the mixer: QEMU's WAV file gets
 * samples only while the driver's stream runs, so a gap between two
 * sounds must be played to be in it. */
static void gap(void)
{
    handle_t g;
    if (tone("gap", "0", "400", "0", NULL, &g) == OK)
        (void)finish(g);
    else
        pause_ms(GAP_MS);
}

static bool t_protocol(void)
{
    uint8_t name[16] = "proto";
    handle_t ch, ring, ev;
    uint32_t id, frames, lead;
    CHECK_ST(audio_open_output_until(svc(), soon(), 44100, 2, 16, name, &ch, &ring, &ev, &id,
                                     &frames, &lead), ERR_NOT_SUPPORTED);
    CHECK_ST(audio_open_output_until(svc(), soon(), 48000, 1, 16, name, &ch, &ring, &ev, &id,
                                     &frames, &lead), ERR_NOT_SUPPORTED);
    CHECK_ST(audio_stream_start_until(svc(), soon()), ERR_NOT_SUPPORTED);
    struct mixer_stream s[1];
    CHECK_ST(mixer_open(svc(), "proto", soon(), &s[0]), OK);
    CHECK(s[0].frames >= 4096 && s[0].lead > 0);
    uint64_t f;
    CHECK_ST(audio_stream_drain_until(s[0].ch, soon(), &f), ERR_BAD_STATE);
    int32_t got = 1;
    CHECK_ST(mixer_set_volume(&s[0], 60, soon(), &got), OK);
    CHECK_EQ(got, 0);
    CHECK_ST(mixer_set_volume(&s[0], -2000, soon(), &got), OK);
    CHECK_EQ(got, -960);
    CHECK_ST(audioctl_set_volume_until(ctl(), soon(), 0xfffffff0u, -10, &got), ERR_NOT_FOUND);
    CHECK_ST(audioctl_set_volume_until(ctl(), soon(), s[0].id, -55, &got), OK);
    CHECK_EQ(got, -55);
    mixer_close(&s[0]);
    return true;
}

enum { CAP = MIXER_STREAMS_PER_CLIENT, OPENERS = MIXER_MAX_STREAMS / CAP + 1 };

/* The cap's checks on OPENERS openers, each filled up as far as it went:
 * the first ones to their cap (16 in all), the last none; an opener at its
 * cap is refused even with a stream free, another opener isn't. */
static bool cap_checks(const handle_t *o, struct mixer_stream (*s)[CAP], unsigned *held)
{
    for (unsigned i = 0; i + 1 < OPENERS; i++)
        CHECK_EQ(held[i], CAP);   /* (some other program may hold one: then fewer) */
    CHECK_EQ(held[OPENERS - 1], 0u);
    struct mixer_stream x;
    CHECK_ST(mixer_open(o[0], "cap", soon(), &x), ERR_NO_RESOURCES);
    mixer_close(&s[1][--held[1]]);   /* one free in all; opener 0 is still at its cap */
    CHECK_ST(mixer_open(o[0], "cap", soon(), &x), ERR_NO_RESOURCES);
    status_t st;
    uint64_t until = soon();   /* the mixer sees the close in its own time */
    while ((st = mixer_open(o[OPENERS - 1], "cap", soon(), &s[OPENERS - 1][0])) ==
               ERR_NO_RESOURCES && now() < until)
        pause_ms(20);
    CHECK_ST(st, OK);
    held[OPENERS - 1] = 1;
    return true;
}

/* One opener of /svc/audio holds MIXER_STREAMS_PER_CLIENT streams at most;
 * each opener has a budget of its own, up to MIXER_MAX_STREAMS in all. */
static bool t_stream_cap(void)
{
    handle_t o[OPENERS] = { 0 };
    struct mixer_stream s[OPENERS][CAP];
    unsigned held[OPENERS] = { 0 };
    bool ok = true;
    for (unsigned i = 0; ok && i < OPENERS; i++) {
        ok = svc_open(SVC_AUDIO, &o[i]) == OK;
        while (ok && held[i] < CAP && mixer_open(o[i], "cap", soon(), &s[i][held[i]]) == OK)
            held[i]++;
    }
    if (ok)
        ok = cap_checks(o, s, held);
    else
        printf("mixtest: %s: /svc/audio didn't open\n", cur);
    for (unsigned i = 0; i < OPENERS; i++) {
        while (held[i])
            mixer_close(&s[i][--held[i]]);
        if (o[i])
            jam_handle_close(o[i]);
    }
    return ok;
}

/* A request written on ch and not waited for (txid 9): it must be
 * answered on ch, with `want`, while `other` stays empty. */
static bool answered_on(handle_t ch, handle_t other, uint32_t ordinal, status_t want)
{
    struct idl_req_hdr q = { 9, ordinal };
    CHECK_ST(jam_channel_write(ch, &q, sizeof(q), NULL, 0), OK);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(ch, SIG_READABLE, soon(), &seen), OK);
    CHECK_ST(jam_object_wait_one(other, SIG_READABLE, 0, &seen), ERR_TIMED_OUT);
    _Alignas(8) uint8_t r[AUDIOCTL_REP_MAX];
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_read(ch, r, sizeof(r), &n, NULL, 0, &nh), OK);
    const struct idl_rep_hdr *h = (const void *)r;
    CHECK(n >= sizeof(*h) && !nh);
    CHECK_EQ(h->txid, 9u);
    CHECK_ST(h->status, want);
    return true;
}

/* Two openers of /svc/audio, and of /svc/audioctl, get channels of their
 * own (svc.connect): what one asks is answered on its channel alone. */
static bool t_openers(void)
{
    handle_t a, b, c, d;
    CHECK_ST(svc_open(SVC_AUDIO, &a), OK);
    CHECK_ST(svc_open(SVC_AUDIO, &b), OK);
    CHECK_ST(svc_open(SVC_AUDIOCTL, &c), OK);
    CHECK_ST(svc_open(SVC_AUDIOCTL, &d), OK);
    bool ok = answered_on(a, b, AUDIO_STREAM_START, ERR_NOT_SUPPORTED) &&
              answered_on(b, a, AUDIO_STREAM_START, ERR_NOT_SUPPORTED) &&
              answered_on(c, d, AUDIOCTL_STREAMS, OK) && answered_on(d, c, AUDIOCTL_STREAMS, OK);
    jam_handle_close(a);
    jam_handle_close(b);
    jam_handle_close(c);
    jam_handle_close(d);
    return ok;
}

#define QUERY_MAX 8u   /* the driver's query channels at once (hda.idl) */

static bool t_device(void)
{
    handle_t q[QUERY_MAX + 1];
    CHECK_ST(audioctl_device_until(ctl(), now() + LONG, 0, &q[0]), OK);
    uint32_t codec, pin = 0, dac, pcm, formats, amp, jack, count;
    uint8_t nodes[8], text[240];
    CHECK_ST(hda_info_until(q[0], soon(), &codec, &pin, &dac, &pcm, &formats, &amp, &jack,
                            &count, nodes, text), OK);
    CHECK(pin != 0);
    int32_t gain, min, max;
    uint32_t step;
    status_t gst = hda_get_gain_until(q[0], soon(), &gain, &step, &min, &max);
    CHECK(gst == OK || gst == ERR_NOT_SUPPORTED);   /* (QEMU's mixer=off codec has no amp) */
    uint32_t bits, bpcm;
    CHECK_ST(hda_set_bits_until(q[0], soon(), 0, &bits, &bpcm), OK);
    /* What only the mixer may do: never on a query channel. */
    handle_t stream = HANDLE_INVALID, ring = HANDLE_INVALID, other = HANDLE_INVALID;
    uint32_t size, period;
    CHECK_ST(hda_open_output_until(q[0], soon(), 48000, 2, 16, &stream, &ring, &size, &period),
             ERR_ACCESS_DENIED);
    CHECK(!stream && !ring);
    CHECK_ST(hda_query_until(q[0], soon(), &other), ERR_ACCESS_DENIED);
    CHECK_ST(hda_start_until(q[0], soon()), ERR_NOT_SUPPORTED);
    /* Bounded: the driver serves QUERY_MAX at once. */
    unsigned got = 1;
    status_t st = OK;
    while (got <= QUERY_MAX && (st = audioctl_device_until(ctl(), now() + LONG, 0, &q[got])) == OK)
        got++;
    CHECK_EQ(got, QUERY_MAX);
    CHECK_ST(st, ERR_NO_RESOURCES);
    jam_handle_close(q[--got]);
    /* The driver takes the closing in its own time: a free slot soon. */
    uint64_t until = now() + SOON;
    while ((st = audioctl_device_until(ctl(), now() + LONG, 0, &q[got])) == ERR_NO_RESOURCES &&
           now() < until)
        pause_ms(20);
    CHECK_ST(st, OK);
    got++;
    while (got)
        jam_handle_close(q[--got]);
    CHECK_ST(audioctl_device_until(ctl(), now() + LONG, 99, &other), ERR_NOT_FOUND);
    return true;
}

static bool t_two_at_once(void)
{
    handle_t a, b;
    CHECK_ST(tone("tone-a", "440", "1500", "0", NULL, &a), OK);
    CHECK_ST(tone("tone-b", "1000", "1500", "-60", NULL, &b), OK);
    struct mixer_stream_info ea, eb;
    CHECK(find_at("tone-a", 0, 2000, &ea));
    CHECK(find_at("tone-b", -60, 2000, &eb));   /* it sets its volume once open */
    CHECK(ea.id != eb.id);
    CHECK(ea.state == MIXER_STATE_PLAYING || ea.state == MIXER_STATE_STOPPED);
    CHECK_EQ(finish(a), 0);
    CHECK_EQ(finish(b), 0);
    return true;
}

static bool t_client_killed(void)
{
    handle_t a, b;
    CHECK_ST(tone("tone-a", "440", "1500", "0", NULL, &a), OK);
    CHECK_ST(tone("tone-b", "1000", "1500", "-60", NULL, &b), OK);
    struct mixer_stream_info e;
    CHECK(find_stream("tone-b", 2000, &e));
    pause_ms(700);
    CHECK_ST(jam_process_kill(b), OK);
    CHECK_EQ(finish(b), -1000);
    CHECK_EQ(finish(a), 0);
    uint64_t until = now() + NS_PER_S;
    while (find_stream("tone-b", 0, &e) && now() < until)
        pause_ms(20);
    CHECK(!find_stream("tone-b", 0, &e));
    return true;
}

static bool t_master(void)
{
    int32_t got = 1;
    handle_t a;
    CHECK_ST(audioctl_set_master_until(ctl(), soon(), -60, &got), OK);
    CHECK_EQ(got, -60);
    CHECK_ST(tone("tone-a", "440", "600", "0", NULL, &a), OK);
    long code = finish(a);
    CHECK_ST(audioctl_set_master_until(ctl(), soon(), 0, &got), OK);
    CHECK_EQ(code, 0);
    return true;
}

static bool t_ctl_volume(void)
{
    handle_t c;
    CHECK_ST(tone("tone-c", "440", "1200", "0", NULL, &c), OK);
    struct mixer_stream_info e;
    CHECK(find_stream("tone-c", 2000, &e));
    pause_ms(600);
    int32_t got = 0;
    CHECK_ST(audioctl_set_volume_until(ctl(), soon(), e.id, -120, &got), OK);
    CHECK_EQ(got, -120);
    CHECK_EQ(finish(c), 0);
    return true;
}

/* Start "mixtest libtone ..." with /svc/audio only. */
static status_t libtone(const char *name, const char *rate, const char *hz, handle_t *proc)
{
    const char *argv[] = { "bin/mixtest", "libtone", name, rate, hz, "1200", NULL };
    struct spawn_args sa = { .path = "bin/mixtest", .argc = 6, .argv = argv,
                             .job = startup_handle(SR_JOB), .ns = audio_only };
    return spawn(&sa, proc);
}

/* Two programs on <audio.h> at once (as play and beep would be): one at
 * 44.1 kHz, resampled by the library, one at 48 kHz. */
static bool t_library_at_once(void)
{
    handle_t a, b;
    CHECK_ST(libtone("lib-play", "44100", "1000", &a), OK);
    CHECK_ST(libtone("lib-beep", "48000", "440", &b), OK);
    struct mixer_stream_info e;
    CHECK(find_stream("lib-play", 2000, &e));
    CHECK(find_stream("lib-beep", 2000, &e));
    CHECK_EQ(finish(a), 0);
    CHECK_EQ(finish(b), 0);
    return true;
}

static bool t_kill_mid_tone(const char *name, const char *who, const char *opt)
{
    handle_t d;
    CHECK(initctl != 0);
    /* 3 s: still writing when the kill comes at 0.6 s (its ring holds
     * 1.37 s, so a shorter tone would be waiting in its drain by then). */
    CHECK_ST(tone(name, "440", "3000", "0", opt, &d), OK);
    struct mixer_stream_info e;
    CHECK(find_stream(name, 2000, &e));
    pause_ms(600);
    uint8_t n[32] = { 0 };
    memcpy(n, who, strlen(who));
    uint64_t koid = 0;
    CHECK_ST(initctl_kill_until(initctl, soon(), n, &koid), OK);
    CHECK_EQ(finish(d), 0);
    return true;
}

static bool t_mixer_killed(void)
{
    return t_kill_mid_tone("tone-d", "mixer", NULL);
}

static bool t_driver_killed(void)
{
    return t_kill_mid_tone("tone-e", "hda", NULL);
}

static bool t_idle_wakes(void)
{
    handle_t f;
    CHECK_ST(tone("tone-f", "440", "800", "0", "pause", &f), OK);
    struct mixer_stream_info e;
    bool idle = false;
    uint64_t until = now() + 3 * NS_PER_S;
    while (!idle && now() < until) {
        idle = find_stream("tone-f", 0, &e) && e.state == MIXER_STATE_IDLE;
        pause_ms(50);
    }
    CHECK(idle);
    CHECK_EQ(finish(f), 0);
    return true;
}

static bool t_desk(void)
{
    handle_t d = HANDLE_INVALID, h = HANDLE_INVALID;
    CHECK_ST(audioctl_desk_channel_until(ctl(), soon(), &d), OK);
    /* refused on it: the streams, their volumes, the card, another channel */
    uint32_t n = 0;
    int32_t master = 1, cb = 0;
    static uint8_t list[640];
    CHECK_ST(audioctl_streams_until(d, soon(), &n, &master, list), ERR_ACCESS_DENIED);
    CHECK_ST(audioctl_set_volume_until(d, soon(), 1, -60, &cb), ERR_ACCESS_DENIED);
    CHECK_ST(audioctl_device_until(d, soon(), 0, &h), ERR_ACCESS_DENIED);
    CHECK_ST(audioctl_desk_channel_until(d, soon(), &h), ERR_ACCESS_DENIED);
    CHECK(!h);
    /* nothing plays: its master, the output's name */
    uint8_t playing = 9, title[64], output[48];
    CHECK_ST(audioctl_desk_until(d, soon(), &master, &playing, title, output), OK);
    CHECK_EQ(master, 0);
    CHECK_EQ(playing, 0);
    CHECK(!strncmp((const char *)output, "QEMU ", 5));
    /* the master volume set on it is the mixer's */
    CHECK_ST(audioctl_set_master_until(d, soon(), -200, &cb), OK);
    CHECK_ST(audioctl_streams_until(ctl(), soon(), &n, &master, list), OK);
    CHECK_EQ(master, -200);
    /* a stream of silence with a title: what plays */
    struct mixer_stream s;
    CHECK_ST(mixer_open(svc(), "desk", soon(), &s), OK);
    uint8_t t[64] = "Artist - Title";
    CHECK_ST(audio_stream_set_title_until(s.ch, soon(), t), OK);
    static int16_t quiet[2 * CHUNK];
    size_t done = 0;
    for (unsigned k = 0; k < 8; k++)
        CHECK_ST(mixer_write(&s, quiet, CHUNK, now() + LONG, &done), OK);
    CHECK_ST(mixer_start(&s, soon()), OK);
    pause_ms(100);
    CHECK_ST(audioctl_desk_until(d, soon(), &master, &playing, title, output), OK);
    CHECK_EQ(playing, 1);
    CHECK(!strcmp((const char *)title, "Artist - Title"));
    CHECK_EQ(master, -200);
    mixer_close(&s);
    CHECK_ST(audioctl_set_master_until(d, soon(), 0, &cb), OK);
    jam_handle_close(d);
    return true;
}

static bool t_none_left(void)
{
    struct mixer_stream_info e[MIXER_MAX_STREAMS];
    uint32_t n = 99;
    int32_t master = 1;
    CHECK_ST(streams(e, &n, &master), OK);
    CHECK_EQ(n, 0);
    CHECK_EQ(master, 0);
    return true;
}

static const struct {
    const char *name;
    bool (*fn)(void);
    bool sound;          /* a phase with sound: silence after it */
} tests[] = {
    { "protocol", t_protocol, false },
    { "openers", t_openers, false },
    { "stream_cap", t_stream_cap, false },
    { "desk", t_desk, false },
    { "device", t_device, false },
    { "two_at_once", t_two_at_once, true },
    { "client_killed", t_client_killed, true },
    { "master", t_master, true },
    { "ctl_volume", t_ctl_volume, true },
    { "library_at_once", t_library_at_once, true },
    { "mixer_killed", t_mixer_killed, true },
    { "driver_killed", t_driver_killed, true },
    { "idle_wakes", t_idle_wakes, true },
    { "none_left", t_none_left, false },
};

int main(int argc, char **argv)
{
    initctl = svc_get(SVC_INIT);
    if (argc >= 6 && !strcmp(argv[1], "tone"))
        return tone_main(argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "libtone"))
        return libtone_main(argv);
    if (!svc() || !ctl()) {
        printf("mixtest: no /svc/audio or /svc/audioctl: run it with the shell's `mixtest`\n");
        return 1;
    }
    unsigned n = sizeof(tests) / sizeof(tests[0]), passed = 0;
    for (unsigned i = 0; i < n; i++) {
        cur = tests[i].name;
        uint64_t t0 = now();
        if (tests[i].fn()) {
            passed++;
            printf("mixtest: %s ok (%lu ms)\n", cur, (unsigned long)((now() - t0) / NS_PER_MS));
        }
        if (tests[i].sound)
            gap();
    }
    char line[96];
    int len = passed == n ? snprintf(line, sizeof(line), "mixtest: %u passed", passed)
                          : snprintf(line, sizeof(line), "mixtest: %u passed, %u FAILED", passed,
                                     n - passed);
    jam_debug_report(line, (uint64_t)len);
    return passed == n ? 0 : 1;
}
