/* utest: libos's sound output API as a program that writes on a clock of
 * its own uses it (<audio.h>: float samples, audio_try_write, the room
 * and position queries, audio_start and audio_pause, audio_wait_room and
 * audio_arm_room). The conversions and the argument checks need nothing;
 * the rest needs the mixer and a sound card, so a case that finds no audio
 * output ("svc audio" not on the list, no mixer, no card) says so and
 * skips: init's run has none. tools/audio-api-test.sh runs them from the
 * shell on a QEMU sound card whose samples go to a WAV file, and checks
 * that file: what these cases wrote is there, frame for frame where it was
 * made of exact samples, in the order the cases run. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <audio.h>
#include <check.h>
#include "utest.h"

#define TWO_PI 6.283185307179586

/* sin by its series after range reduction (utest has no libm). */
static double osin(double x)
{
    x -= TWO_PI * (double)(int64_t)(x / TWO_PI);
    if (x > TWO_PI / 2)
        x -= TWO_PI;
    else if (x < -TWO_PI / 2)
        x += TWO_PI;
    double term = x, sum = x;
    for (int k = 1; k < 20; k++) {
        term *= -x * x / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

/* The exact patterns the script knows (tools/audio-api-test.sh has the
 * same three): frame k of set `which`, channel ch: never 0, so the sound's
 * start and end are where the first and last frames are. */
static int16_t pat(unsigned which, unsigned ch, uint32_t k)
{
    static const uint32_t mul[3][2] = { { 7919, 104729 }, { 6007, 7001 }, { 3001, 5003 } };
    static const uint32_t mod[3][2] = { { 30011, 27449 }, { 29021, 26003 }, { 31013, 25013 } };
    static const int32_t off[3][2] = { { 15005, 13724 }, { 14510, 13001 }, { 15506, 12506 } };
    int32_t v = (int32_t)(((uint64_t)k * mul[which][ch]) % mod[which][ch]) - off[which][ch];
    return (int16_t)(v ? v : (ch ? -1 : 1));
}

static bool skipped(void)
{
    printf("utest: %s: no audio output here (not the shell's utest, no mixer or no sound card): "
           "skipped\n", utest_cur);
    return true;
}

#define NS_MS NS_PER_MS

/* ---- no device ------------------------------------------------------------------ */

bool t_audio_out_f32(void)
{
    uint32_t seed = 0x2545f491u;
    /* Whole 16-bit values come back as they are, and silence stays silent. */
    float in[8];
    int16_t out[8];
    static const int16_t want[8] = { 0, 1, -1, 12345, -12345, 32767, -32768, 16384 };
    for (unsigned i = 0; i < 8; i++)
        in[i] = (float)want[i] / 32768.0f;
    audio_s16_from_f32(out, in, 8, &seed);
    for (unsigned i = 0; i < 8; i++)
        CHECK_EQ(out[i], want[i]);

    /* All of the 16-bit values, a few thousand of them at random. */
    static float all[4096];
    static int16_t back[4096];
    for (unsigned i = 0; i < 4096; i++)
        all[i] = (float)(int16_t)(i * 16 + 7 - 32768) / 32768.0f;
    audio_s16_from_f32(back, all, 4096, &seed);
    for (unsigned i = 0; i < 4096; i++)
        CHECK_EQ(back[i], (int16_t)(i * 16 + 7 - 32768));

    /* Clipping; a NaN is silence; infinities clip. */
    float big[6] = { 1.0f, 2.5f, -1.0f, -7.0f, 1e30f, -1e30f };
    int16_t bout[6];
    audio_s16_from_f32(bout, big, 6, &seed);
    CHECK_EQ(bout[0], 32767);
    CHECK_EQ(bout[1], 32767);
    CHECK_EQ(bout[2], -32768);
    CHECK_EQ(bout[3], -32768);
    CHECK_EQ(bout[4], 32767);
    CHECK_EQ(bout[5], -32768);
    float odd[3] = { __builtin_nanf(""), __builtin_inff(), -__builtin_inff() };
    int16_t oout[3];
    audio_s16_from_f32(oout, odd, 3, &seed);
    CHECK_EQ(oout[0], 0);
    CHECK_EQ(oout[1], 32767);
    CHECK_EQ(oout[2], -32768);

    /* Between two values: the nearest, or one of its neighbours when the
     * dither tips it; never further, and on average right. */
    float mid[1] = { 100.3f / 32768.0f };
    int64_t sum = 0;
    for (unsigned i = 0; i < 4000; i++) {
        int16_t o;
        audio_s16_from_f32(&o, mid, 1, &seed);
        CHECK(o >= 99 && o <= 101);
        sum += o;
    }
    CHECK(sum > 4000 * 100 && sum < 4000 * 100 + 4000 * 6 / 10);   /* mean 100.3, +- 0.1 */
    return true;
}

bool t_audio_out_args(void)
{
    struct audio_out a;
    CHECK_EQ(audio_open_fmt(&a, 48000, 2, (enum audio_format)7, "x"), ERR_NOT_SUPPORTED);
    CHECK_EQ(audio_open_fmt(&a, 48000, 3, AUDIO_F32, "x"), ERR_NOT_SUPPORTED);
    CHECK_EQ(audio_open_fmt(&a, 7999, 2, AUDIO_F32, "x"), ERR_NOT_SUPPORTED);
    CHECK_EQ(audio_open_fmt(&a, 192001, 1, AUDIO_S16, "x"), ERR_NOT_SUPPORTED);
    /* An output that isn't open answers everything with ERR_BAD_STATE. */
    memset(&a, 0, sizeof(a));
    static const int16_t one[2] = { 1, 2 };
    struct audio_pos pos;
    CHECK_EQ(audio_try_write(&a, one, 1), ERR_BAD_STATE);
    CHECK_EQ(audio_write(&a, one, 1), ERR_BAD_STATE);
    CHECK_EQ(audio_start(&a), ERR_BAD_STATE);
    CHECK_EQ(audio_pause(&a), ERR_BAD_STATE);
    CHECK_EQ(audio_position(&a, &pos), ERR_BAD_STATE);
    CHECK_EQ(audio_wait_room(&a, 1, now()), ERR_BAD_STATE);
    CHECK_EQ(audio_arm_room(&a, 1), ERR_BAD_STATE);
    CHECK_EQ(audio_room(&a), 0);
    CHECK_EQ(audio_room_max(&a), 0);
    CHECK_EQ(audio_queued(&a), 0);
    CHECK(audio_room_event(&a) == HANDLE_INVALID);
    return true;
}

/* ---- the streams ------------------------------------------------------------------ */

/* Four streams of the program's opener, and a fifth refused there but
 * taken on an opener of our own; a closed one gives its place back. */
bool t_audio_out_slots(void)
{
    struct audio_out s[5];
    int st = audio_open_fmt(&s[0], 48000, 2, AUDIO_S16, "ut-slot");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (unsigned i = 1; i < 4; i++)
        CHECK_ST(audio_open_fmt(&s[i], i % 2 ? 44100 : 48000, 1 + i % 2, AUDIO_F32, "ut-slot"), OK);
    CHECK_EQ(audio_open_fmt(&s[4], 48000, 2, AUDIO_S16, "ut-slot"), ERR_NO_RESOURCES);
    /* A fifth on an opener of our own (audio_open_on), which counts its own. */
    handle_t own = HANDLE_INVALID;
    CHECK_ST(svc_open(SVC_AUDIO, &own), OK);
    CHECK_ST(audio_open_on(&s[4], own, 48000, 2, AUDIO_F32, "ut-slot"), OK);
    audio_close(&s[4]);
    jam_handle_close(own);
    CHECK_EQ(audio_open_on(&s[4], HANDLE_INVALID, 48000, 2, AUDIO_S16, "ut-slot"), ERR_NOT_FOUND);
    audio_close(&s[2]);
    CHECK_ST(audio_open_fmt(&s[2], 48000, 2, AUDIO_F32, "ut-slot"), OK);
    CHECK_EQ(audio_open_fmt(&s[4], 48000, 2, AUDIO_S16, "ut-slot"), ERR_NO_RESOURCES);
    for (unsigned i = 0; i < 4; i++)
        audio_close(&s[i]);
    /* All four free again. */
    for (unsigned i = 0; i < 4; i++)
        CHECK_ST(audio_open_fmt(&s[i], 48000, 2, AUDIO_S16, "ut-slot"), OK);
    for (unsigned i = 0; i < 4; i++)
        audio_close(&s[i]);
    return true;
}

static int16_t s16buf[2 * 98000];
static float   f32buf[2 * 24000];

/* 16-bit stereo at 48 kHz: the exact frames reach the mixer, in pieces
 * that cross the library's blocks. */
bool t_audio_out_s16(void)
{
    enum { N = 24000 };
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-s16");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (uint32_t k = 0; k < N; k++) {
        s16buf[2 * k] = pat(0, 0, k);
        s16buf[2 * k + 1] = pat(0, 1, k);
    }
    for (size_t at = 0; at < N;) {
        size_t n = N - at < 1777 ? N - at : 1777;
        CHECK_EQ(audio_write(&a, s16buf + 2 * at, n), (long)n);
        at += n;
    }
    CHECK_ST(audio_drain(&a), OK);
    struct audio_pos p;
    CHECK_ST(audio_position(&a, &p), OK);
    CHECK_EQ(p.written, N + 4096);    /* the drain's tail of silence */
    CHECK(p.played >= p.written);
    CHECK(p.latency_ns < 50 * NS_MS);
    audio_close(&a);
    return true;
}

/* Float samples that are 16-bit values come out as those values, stereo
 * and then mono, in pieces. */
bool t_audio_out_f32_play(void)
{
    enum { N = 24000 };
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_F32, "ut-f32");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (uint32_t k = 0; k < N; k++) {
        f32buf[2 * k] = (float)pat(1, 0, k) / 32768.0f;
        f32buf[2 * k + 1] = (float)pat(1, 1, k) / 32768.0f;
    }
    for (size_t at = 0; at < N;) {
        size_t n = N - at < 977 ? N - at : 977;
        CHECK_EQ(audio_write(&a, f32buf + 2 * at, n), (long)n);
        at += n;
    }
    CHECK_ST(audio_drain(&a), OK);
    struct audio_pos p;
    CHECK_ST(audio_position(&a, &p), OK);
    CHECK_EQ(p.written, N + 4096);
    CHECK(p.played >= p.written);
    audio_close(&a);
    return true;
}

/* Mono float at 44.1 kHz (the resampler and the mono to stereo copy):
 * a 1000 Hz tone a quarter of full scale, half a second. */
bool t_audio_out_mono44(void)
{
    enum { N = 22050 };
    struct audio_out a;
    int st = audio_open_fmt(&a, 44100, 1, AUDIO_F32, "ut-mono44");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (uint32_t k = 0; k < N; k++)
        f32buf[k] = 0.25f * (float)osin(TWO_PI * 1000.0 * (double)k / 44100.0);
    for (size_t at = 0; at < N;) {
        size_t n = N - at < 1500 ? N - at : 1500;
        CHECK_EQ(audio_write(&a, f32buf + at, n), (long)n);
        at += n;
    }
    CHECK_ST(audio_drain(&a), OK);
    struct audio_pos p;
    CHECK_ST(audio_position(&a, &p), OK);
    /* 22050 frames at 44.1 kHz are 24000 at 48, and the drain flushes the
     * resampler (a few dozen more), then its tail. */
    CHECK(p.written >= 24000 + 4096 && p.written < 24000 + 4096 + 400);
    CHECK(p.played >= p.written);
    audio_close(&a);
    return true;
}

/* The ring's room, its edges and the waits, on a paused stream so nothing
 * is heard (and nothing is taken): the ring fills to exactly its size,
 * a wait for room times out, and a discard empties it at once. */
bool t_audio_out_room(void)
{
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-room");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    size_t max = audio_room_max(&a);
    memset(s16buf, 0, sizeof(s16buf));
    CHECK(max >= 32768 && max <= 70000);
    CHECK_EQ(audio_room(&a), max);
    CHECK_EQ(audio_queued(&a), 0);
    CHECK_ST(audio_pause(&a), OK);
    CHECK_EQ(audio_wait_room(&a, 0, now() + 10 * NS_MS), ERR_INVALID_ARGS);
    CHECK_EQ(audio_wait_room(&a, max + 1, now() + 10 * NS_MS), ERR_INVALID_ARGS);
    CHECK_ST(audio_wait_room(&a, max, now() + 10 * NS_MS), OK);   /* all of it, now */
    /* Silence in, more than the ring holds: it takes its size and no more. */
    long n = audio_try_write(&a, s16buf, 70000);
    CHECK_EQ(n, (long)max);
    CHECK_EQ(audio_room(&a), 0);
    CHECK_EQ(audio_queued(&a), max);
    CHECK_EQ(audio_try_write(&a, s16buf, 100), 0);
    /* Paused: the full ring is not started, so no room comes. */
    uint64_t t0 = now();
    CHECK_EQ(audio_wait_room(&a, 1, t0 + 100 * NS_MS), ERR_TIMED_OUT);
    CHECK(now() >= t0 + 100 * NS_MS);
    CHECK_EQ(audio_arm_room(&a, 1), ERR_SHOULD_WAIT);
    signals_t seen = 0;
    CHECK_EQ(jam_object_wait_one(audio_room_event(&a), MIXER_SIG_SPACE, now() + 100 * NS_MS, &seen),
             ERR_TIMED_OUT);
    /* The position of a stream that has never started: all of it ahead. */
    struct audio_pos p;
    CHECK_ST(audio_position(&a, &p), OK);
    CHECK_EQ(p.written, max);
    CHECK_EQ(p.played, 0);
    CHECK_EQ(p.latency_ns, max * NS_PER_S / 48000);
    CHECK(p.lead > 0 && p.lead <= 16384);
    CHECK(p.at >= t0 && p.at <= now());
    /* Discard: nothing is heard of a stopped ring, so it is empty at once. */
    CHECK_ST(audio_discard(&a), OK);
    CHECK_EQ(audio_queued(&a), 0);
    CHECK_EQ(audio_room(&a), max);
    CHECK_ST(audio_arm_room(&a, max), OK);   /* room for all of it: nothing to wait for */
    audio_close(&a);
    return true;
}

/* The wake-up for room: a full ring that is started again is emptied a
 * period at a time, and the event says so. */
bool t_audio_out_arm(void)
{
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-arm");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    size_t max = audio_room_max(&a);
    memset(s16buf, 0, sizeof(s16buf));
    CHECK_ST(audio_pause(&a), OK);
    CHECK_EQ(audio_try_write(&a, s16buf, 70000), (long)max);   /* silence, a full ring */
    CHECK_EQ(audio_arm_room(&a, 1024), ERR_SHOULD_WAIT);
    CHECK_ST(audio_start(&a), OK);
    bool got = false;
    for (unsigned i = 0; i < 40 && !got; i++) {
        signals_t seen = 0;
        status_t w = jam_object_wait_one(audio_room_event(&a), MIXER_SIG_SPACE,
                                         now() + 250 * NS_MS, &seen);
        CHECK(w == OK || w == ERR_TIMED_OUT);
        got = audio_arm_room(&a, 1024) == OK;   /* the mixer took a period or two */
    }
    CHECK(got);
    CHECK(audio_room(&a) >= 1024);
    /* Written again, the room is used up (the mixer may have taken more
     * since it was looked at) and waited for again. */
    size_t room = audio_room(&a);
    CHECK(audio_try_write(&a, s16buf, 70000) >= (long)room);
    CHECK_ST(audio_wait_room(&a, 2048, now() + 3 * NS_PER_S), OK);
    CHECK_ST(audio_discard(&a), OK);   /* the silence still queued: faded and gone */
    audio_close(&a);
    return true;
}

/* A tone written on a clock: pre-rolled, started, then fed by
 * audio_try_write with a wait for room whenever the ring is full (it
 * holds less than the tone and what the mixer takes at once when it
 * starts, so it fills). 440 Hz at a quarter of full scale, two seconds;
 * the script checks the capture has it
 * whole, without a gap. */
bool t_audio_out_paced(void)
{
    enum { TOTAL = 96000, PRE = 14400, PIECE = 480 };
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-paced");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (uint32_t k = 0; k < TOTAL; k++) {
        double v = 8192.0 * osin(TWO_PI * 440.0 * (double)k / 48000.0);
        s16buf[2 * k] = s16buf[2 * k + 1] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
    }
    CHECK_EQ(audio_try_write(&a, s16buf, PRE), PRE);
    struct audio_pos p;
    CHECK_ST(audio_position(&a, &p), OK);
    CHECK_EQ(p.latency_ns, (uint64_t)PRE * NS_PER_S / 48000);   /* not started: all ahead */
    CHECK_ST(audio_start(&a), OK);
    size_t done = PRE;
    unsigned full = 0, calls = 0;
    uint64_t worst_latency = 0;
    while (done < TOTAL) {
        long w = audio_try_write(&a, s16buf + 2 * done, TOTAL - done < PIECE ? TOTAL - done : PIECE);
        CHECK(w >= 0);
        calls++;
        if (w == 0) {
            full++;
            CHECK_ST(audio_wait_room(&a, PIECE, now() + 3 * NS_PER_S), OK);
            continue;
        }
        done += (size_t)w;
        if (calls % 16 == 0) {
            CHECK_ST(audio_position(&a, &p), OK);
            if (p.latency_ns > worst_latency)
                worst_latency = p.latency_ns;
        }
    }
    CHECK(full > 0);   /* the tone is longer than the ring */
    /* Never more ahead than a ring and the mixer's lead. */
    CHECK(worst_latency <= (uint64_t)(audio_room_max(&a) + p.lead + 4096) * NS_PER_S / 48000);
    CHECK_ST(audio_drain(&a), OK);
    CHECK_ST(audio_position(&a, &p), OK);
    CHECK_EQ(p.written, TOTAL + 4096);
    CHECK(p.played >= p.written);
    audio_close(&a);
    return true;
}

/* Two streams at once: 440 Hz in 16-bit stereo and 1000 Hz in float mono,
 * each a quarter of full scale for 0.6 s, started one after the other
 * and drained one after the other. */
bool t_audio_out_two(void)
{
    enum { N = 28800 };
    struct audio_out a, b;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-two-a");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    CHECK_ST(audio_open_fmt(&b, 48000, 1, AUDIO_F32, "ut-two-b"), OK);
    for (uint32_t k = 0; k < N; k++) {
        double v = 8192.0 * osin(TWO_PI * 440.0 * (double)k / 48000.0);
        s16buf[2 * k] = s16buf[2 * k + 1] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        f32buf[k] = 0.25f * (float)osin(TWO_PI * 1000.0 * (double)k / 48000.0);
    }
    CHECK_EQ(audio_try_write(&a, s16buf, N), N);
    CHECK_EQ(audio_try_write(&b, f32buf, N), N);
    CHECK_ST(audio_start(&a), OK);
    CHECK_ST(audio_start(&b), OK);
    CHECK_ST(audio_drain(&a), OK);
    CHECK_ST(audio_drain(&b), OK);
    struct audio_pos pa, pb;
    CHECK_ST(audio_position(&a, &pa), OK);
    CHECK_ST(audio_position(&b, &pb), OK);
    CHECK_EQ(pa.written, N + 4096);
    CHECK_EQ(pb.written, N + 4096);
    CHECK(pa.played >= pa.written && pb.played >= pb.written);
    audio_close(&a);
    audio_close(&b);
    return true;
}

/* Pause and resume: 1.25 s of an exact pattern is written, started,
 * paused 0.3 s in for half a second (the play position stops), resumed,
 * and drained. The script checks the capture: the pattern up to the
 * pause, then the rest of it in order from a later frame (the lead the
 * mixer had taken when it stopped the device is not played), no frame
 * twice. */
bool t_audio_out_pause(void)
{
    enum { N = 60000 };
    struct audio_out a;
    int st = audio_open_fmt(&a, 48000, 2, AUDIO_S16, "ut-pause");
    if (st == ERR_NOT_FOUND)
        return skipped();
    CHECK_ST(st, OK);
    for (uint32_t k = 0; k < N; k++) {
        s16buf[2 * k] = pat(2, 0, k);
        s16buf[2 * k + 1] = pat(2, 1, k);
    }
    CHECK_EQ(audio_try_write(&a, s16buf, N), N);
    CHECK_ST(audio_start(&a), OK);
    jam_sleep_until(now() + 300 * NS_MS);
    CHECK_ST(audio_pause(&a), OK);
    CHECK_ST(audio_pause(&a), OK);   /* again: nothing */
    jam_sleep_until(now() + 500 * NS_MS);   /* longer than the mixer's lead */
    struct audio_pos p1, p2;
    CHECK_ST(audio_position(&a, &p1), OK);
    jam_sleep_until(now() + 200 * NS_MS);
    CHECK_ST(audio_position(&a, &p2), OK);
    CHECK(p1.played > 0 && p1.played < N);
    CHECK((int64_t)(p2.played - p1.played) <= 960);   /* stopped (20 ms is slack for the estimate) */
    CHECK_EQ(p2.written, N);
    CHECK_ST(audio_start(&a), OK);
    CHECK_ST(audio_start(&a), OK);   /* again: nothing */
    CHECK_ST(audio_drain(&a), OK);
    CHECK_ST(audio_position(&a, &p2), OK);
    CHECK_EQ(p2.written, N + 4096);
    CHECK(p2.played >= p2.written);
    audio_close(&a);
    return true;
}
