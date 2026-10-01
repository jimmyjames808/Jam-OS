/* mixtest: the mixer (user/services/mixer, abi/idl/audio.idl and
 * audioctl.idl) from user space. The shell's `mixtest` runs it with the
 * mixer's channels (SR_AUDIO, SR_AUDIO_CTL), devmgr's and init's control
 * channel (SR_USER + 3: it kills the mixer and the hda driver once);
 * tools/mixer-test.sh runs it in QEMU with the codec's samples going to a
 * WAV file and checks the sound there.
 *
 * Every sound comes from a child process, `mixtest tone <name> <hz> <ms>
 * <centibels> [reopen] [pause]`, a separate program with only SR_AUDIO
 * (as the shell gives any program): a sine at a quarter of full scale
 * with 5 ms fades, written ahead with mixer_write, drained, closed.
 * `reopen`: it must see the mixer go away once (ERR_PEER_CLOSED) and
 * then opens a new stream and plays the rest; `pause`: after 300 ms of
 * tone it waits 2 s with nothing written, then plays the rest. Phases,
 * each a sound of its own with silence between (the WAV's segments):
 *   protocol       no sound: formats refused, the stream methods refused
 *                  on SR_AUDIO, drain on a stopped stream, volumes
 *                  clamped, an unknown id, the 17th stream refused
 *   two_at_once    440 Hz and 1000 Hz (the second at -6 dB) from two
 *                  programs at once; both are listed while they play
 *   client_killed  the same, and the 1000 Hz program killed mid-tone: the
 *                  other plays to its end; its stream is dropped
 *   master         440 Hz with the master at -6 dB
 *   ctl_volume     440 Hz, turned to -12 dB through audioctl mid-tone
 *   library_at_once  two programs on <audio.h> (beep's and play's
 *                  library) at once: 1000 Hz at 44.1 kHz (resampled) and
 *                  440 Hz at 48 kHz, each a stream named for `vol`
 *   mixer_killed   init kills the mixer mid-tone: the program sees
 *                  ERR_PEER_CLOSED, opens a new stream and plays the rest
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
#include <idl/initctl.h>
#include <mixer.h>
#include <os.h>

#define AMPLITUDE 8192.0             /* -12 dBFS */
#define FADE      240u               /* frames: 5 ms */
#define CHUNK     1024u              /* frames written at a time */
#define SOON      (5 * NS_PER_S)
#define LONG      (20 * NS_PER_S)    /* a whole tone, a restart included */
#define GAP_MS    400u               /* silence between the phases */

static const char *cur;
static handle_t svc, ctl, initctl;

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
    status_t st = mixer_open(svc, name, now() + LONG, s);
    if (st == OK && cb)
        st = mixer_set_volume(s, cb, now() + SOON, NULL);
    return st;
}

/* "mixtest tone <name> <hz> <ms> <cb> [reopen] [pause]": the exit code
 * says how it went (0 played it all as asked). */
static int tone_main(int argc, char **argv)
{
    const char *name = argv[2];
    bool reopen = false, pause = false, went = false;
    for (int i = 6; i < argc; i++) {
        reopen |= !strcmp(argv[i], "reopen");
        pause |= !strcmp(argv[i], "pause");
    }
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
        if (st == ERR_PEER_CLOSED && reopen && !went) {
            uint64_t t0 = now();
            printf("mixtest: tone %s: the mixer went away after %lu frames\n", name,
                   (unsigned long)t.at);
            mixer_close(&s);
            went = true;
            st = tone_open(name, (int32_t)number(argv[5]), &s);
            if (st == OK)
                st = mixer_write(&s, buf + 2 * done, (size_t)n - done, now() + LONG, &done);
            printf("mixtest: tone %s: a new stream after %lu ms (%s)\n", name,
                   (unsigned long)((now() - t0) / NS_PER_MS), status_str(st));
        }
    }
    if (st == OK)
        st = mixer_drain(&s, now() + LONG);
    printf("mixtest: tone %s: %lu frames, %s\n", name, (unsigned long)t.at, status_str(st));
    mixer_close(&s);
    return st != OK ? 1 : reopen && !went ? 2 : 0;
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

/* Start "mixtest tone ..." with SR_AUDIO only. */
static status_t tone(const char *name, const char *hz, const char *ms, const char *cb,
                     const char *opt, handle_t *proc)
{
    handle_t a;
    status_t st = jam_handle_duplicate(svc, RIGHT_SAME, &a);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_AUDIO, a } };
    const char *argv[] = { "bin/mixtest", "tone", name, hz, ms, cb, opt, NULL };
    struct spawn_args sa = { .path = "bin/mixtest", .argc = opt ? 7 : 6, .argv = argv,
                             .job = startup_handle(SR_JOB), .extra = x, .nextra = 1 };
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
    status_t st = audioctl_streams_until(ctl, soon(), n, master, raw);
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
    CHECK_ST(audio_open_output_until(svc, soon(), 44100, 2, 16, name, &ch, &ring, &ev, &id,
                                     &frames, &lead), ERR_NOT_SUPPORTED);
    CHECK_ST(audio_open_output_until(svc, soon(), 48000, 1, 16, name, &ch, &ring, &ev, &id,
                                     &frames, &lead), ERR_NOT_SUPPORTED);
    CHECK_ST(audio_stream_start_until(svc, soon()), ERR_NOT_SUPPORTED);
    struct mixer_stream s[MIXER_MAX_STREAMS + 1];
    CHECK_ST(mixer_open(svc, "proto", soon(), &s[0]), OK);
    CHECK(s[0].frames >= 4096 && s[0].lead > 0);
    uint64_t f;
    CHECK_ST(audio_stream_drain_until(s[0].ch, soon(), &f), ERR_BAD_STATE);
    int32_t got = 1;
    CHECK_ST(mixer_set_volume(&s[0], 60, soon(), &got), OK);
    CHECK_EQ(got, 0);
    CHECK_ST(mixer_set_volume(&s[0], -2000, soon(), &got), OK);
    CHECK_EQ(got, -960);
    CHECK_ST(audioctl_set_volume_until(ctl, soon(), 0xfffffff0u, -10, &got), ERR_NOT_FOUND);
    CHECK_ST(audioctl_set_volume_until(ctl, soon(), s[0].id, -55, &got), OK);
    CHECK_EQ(got, -55);
    unsigned opened = 1;
    status_t st = OK;
    while (opened <= MIXER_MAX_STREAMS && (st = mixer_open(svc, "proto", soon(), &s[opened])) == OK)
        opened++;
    for (unsigned i = 0; i < opened; i++)
        mixer_close(&s[i]);
    CHECK_EQ(opened, MIXER_MAX_STREAMS);   /* (some other program may hold one: then fewer) */
    CHECK_ST(st, ERR_NO_RESOURCES);
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
    CHECK_ST(audioctl_set_master_until(ctl, soon(), -60, &got), OK);
    CHECK_EQ(got, -60);
    CHECK_ST(tone("tone-a", "440", "600", "0", NULL, &a), OK);
    long code = finish(a);
    CHECK_ST(audioctl_set_master_until(ctl, soon(), 0, &got), OK);
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
    CHECK_ST(audioctl_set_volume_until(ctl, soon(), e.id, -120, &got), OK);
    CHECK_EQ(got, -120);
    CHECK_EQ(finish(c), 0);
    return true;
}

/* Start "mixtest libtone ..." with SR_AUDIO only. */
static status_t libtone(const char *name, const char *rate, const char *hz, handle_t *proc)
{
    handle_t a;
    status_t st = jam_handle_duplicate(svc, RIGHT_SAME, &a);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_AUDIO, a } };
    const char *argv[] = { "bin/mixtest", "libtone", name, rate, hz, "1200", NULL };
    struct spawn_args sa = { .path = "bin/mixtest", .argc = 6, .argv = argv,
                             .job = startup_handle(SR_JOB), .extra = x, .nextra = 1 };
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
    return t_kill_mid_tone("tone-d", "mixer", "reopen");
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
    svc = startup_handle(SR_AUDIO);
    ctl = startup_handle(SR_AUDIO_CTL);
    initctl = startup_handle(SR_USER + 3);
    if (argc >= 6 && !strcmp(argv[1], "tone"))
        return tone_main(argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "libtone"))
        return libtone_main(argv);
    if (!svc || !ctl) {
        printf("mixtest: no SR_AUDIO or SR_AUDIO_CTL: run it with the shell's `mixtest`\n");
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
