/* splash: the sound, and the media clock (splash_int.h).
 *
 * The animation starts at once, on the timer; its sound joins when the
 * audio is up. A thread of its own decodes the file's whole MP2 track into
 * 48 kHz stereo frames (6.5 s: 1.2 MiB) while the first frames are shown,
 * then opens a stream on the mixer (<audio.h>), which on a boot answers
 * only once devmgr has bound the hda driver and init has started the
 * mixer. The sound then starts where the animation is by then, plus the
 * time it takes to be heard (HEARD_IN), not from the beginning: the
 * file's sound frame f belongs to media time f / 48000 s (sound_decode
 * lines them up by the file's time stamps).
 *
 * The clock: before the sound is heard, media time is the timer since the
 * first frame. Once it is heard, the sound is the master: media time is
 * the frame being heard (the start frame plus the stream's `played`
 * position, audio.idl's stream_position, which the mixer interpolates from
 * the driver's position), polled every POLL and moved on by the timer in
 * between. The video shows the frame that matches it, dropping frames when
 * it is behind and waiting when it is ahead, so it follows the sound and
 * never drifts from it. If the sound can't come up before the animation
 * ends (no audio device, no mixer), the animation plays silently.
 *
 * The thread publishes (media time, uptime) pairs through a sequence lock:
 * one writer, the video loop reads. */
#include <audio.h>
#include <idl/audio.h>
#include "splash_int.h"

#define MAX_SECONDS 120u                 /* the most sound kept */
#define CHUNK      2048u                 /* frames a write: 43 ms (a stop is seen between) */
#define POLL       (20 * NS_PER_MS)      /* how often the position is asked */
#define STACK      (256u << 10)          /* the thread's (pl_mpeg's audio decoder is small) */
#define HEARD_IN   (50 * NS_PER_MS)      /* from the open to the first frame heard (a guess) */
#define AWAIT_MAX  (500 * NS_PER_MS)     /* the clock holds at 0 for the first sound this long */
#define FADE_IN    1200u                 /* frames: 25 ms raised cosine on a late join */


static uint64_t t0;                   /* uptime at media time 0 (0: not started) */
static bool live;                     /* the sound is heard: the clock is its position */
static uint32_t seq;                  /* the sequence lock: odd while being written */
static uint64_t pub_media, pub_at;    /* media ns heard at uptime pub_at */
static bool stop_asked, ended = true;
static bool await_sound;              /* the clock started for the sound: held at 0 until heard */
static int state = SOUND_NONE;        /* SOUND_*: the stream's, for the video's start */

static const uint8_t *file;           /* the video file (bootfs) */
static size_t file_len;

void clock_start(bool with_sound)
{
    __atomic_store_n(&await_sound, with_sound, __ATOMIC_RELAXED);
    __atomic_store_n(&t0, now(), __ATOMIC_RELEASE);
}

int sound_state(void)
{
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

uint64_t clock_now(void)
{
    uint64_t start = __atomic_load_n(&t0, __ATOMIC_ACQUIRE), t = now();
    if (!start)
        return 0;
    if (!__atomic_load_n(&live, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&await_sound, __ATOMIC_RELAXED))
            return t - start;
        /* The sound starts at 0 and is about to be heard: hold the first
         * frame for it, but not for ever (then the timer, from 0). */
        return t - start < AWAIT_MAX ? 0 : t - start - AWAIT_MAX;
    }
    for (;;) {
        uint32_t s1 = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
        uint64_t m = __atomic_load_n(&pub_media, __ATOMIC_RELAXED);
        uint64_t at = __atomic_load_n(&pub_at, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(s1 & 1) && __atomic_load_n(&seq, __ATOMIC_RELAXED) == s1)
            return m + (t > at ? t - at : 0);
    }
}

/* The sound's frame f is heard now. */
static void publish(uint64_t f)
{
    uint64_t m = f * NS_PER_S / SPLASH_RATE;
    __atomic_store_n(&seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&pub_media, m, __ATOMIC_RELAXED);
    __atomic_store_n(&pub_at, now(), __ATOMIC_RELAXED);
    __atomic_store_n(&seq, seq + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&live, true, __ATOMIC_RELEASE);
}

/* pl_mpeg's float sample (-1..1) to 16 bits, rounded, clamped. */
static int16_t s16(float v)
{
    float x = v * 32767.0f;
    if (x >= 32767.0f)
        return 32767;
    if (x <= -32768.0f)
        return -32768;
    return (int16_t)(x < 0 ? x - 0.5f : x + 0.5f);
}

/* The first time stamp of the file's packets of `type` (s; negative: none). */
static double first_pts(const uint8_t *mpg, size_t len, int type)
{
    plm_buffer_t *b = plm_buffer_create_with_memory((uint8_t *)(uintptr_t)mpg, len, 0);
    plm_demux_t *d = b ? plm_demux_create(b, 1) : NULL;   /* d destroys b */
    if (!d) {
        if (b)
            plm_buffer_destroy(b);
        return -1;
    }
    double t = plm_demux_get_start_time(d, type);
    plm_demux_destroy(d);
    return t;
}

/* How many decoded frames come before the first video frame's time: the
 * MP2 decoder's delay (481 samples), which the encoder made up for by
 * starting the sound's time stamps that much earlier (10 ms, in
 * tools/mksplash.sh's files). pl_mpeg starts each stream's times at 0, so
 * the stamps are read here. Negative: the sound starts later. */
static long sound_shift(const uint8_t *mpg, size_t len)
{
    double v0 = first_pts(mpg, len, PLM_DEMUX_PACKET_VIDEO_1);
    double a0 = first_pts(mpg, len, PLM_DEMUX_PACKET_AUDIO_1);
    if (v0 < 0 || a0 < 0 || v0 - a0 > 1 || a0 - v0 > 1)
        return 0;   /* no stamps, or nonsense: start together */
    double f = (v0 - a0) * SPLASH_RATE;
    return (long)(f < 0 ? f - 0.5 : f + 0.5);
}

status_t sound_decode(const uint8_t *mpg, size_t len, int16_t **out, size_t *frames)
{
    long shift = sound_shift(mpg, len);
    plm_t *p = plm_create_with_memory((uint8_t *)(uintptr_t)mpg, len, 0);
    if (!p)
        return ERR_NO_MEMORY;
    plm_set_video_enabled(p, 0);
    status_t st = plm_has_headers(p) && plm_get_num_audio_streams(p) > 0 &&
                  plm_get_samplerate(p) == SPLASH_RATE ? OK : ERR_NOT_SUPPORTED;
    double secs = st == OK ? plm_get_duration(p) + 1 : 0;
    uint64_t max = (uint64_t)((secs < MAX_SECONDS ? secs : MAX_SECONDS) * SPLASH_RATE);
    int16_t *pcm = st == OK ? big_alloc(max * 4) : NULL;
    if (st == OK && !pcm)
        st = ERR_NO_MEMORY;
    /* Decoded frame k is the sound at media frame k - shift (the buffer
     * starts zeroed: a later start is silence first). */
    long k = 0, n = 0;
    plm_samples_t *smp;
    while (st == OK && (smp = plm_decode_audio(p))) {
        for (unsigned i = 0; i < smp->count; i++, k++) {
            long at = k - shift;
            if (at < 0 || at >= (long)max)
                continue;
            pcm[2 * at] = s16(smp->interleaved[2 * i]);
            pcm[2 * at + 1] = s16(smp->interleaved[2 * i + 1]);
            n = at + 1;
        }
    }
    plm_destroy(p);
    if (st != OK)
        return st;
    *out = pcm;
    *frames = (size_t)n;
    return OK;
}

/* Write frames from..to of pcm into the open stream, starting it, polling
 * the position (the clock) and stopping early if asked. */
static void play(struct audio_out *a, const int16_t *pcm, uint64_t from, uint64_t to)
{
    uint64_t f = from, last_poll = 0;
    while (!__atomic_load_n(&stop_asked, __ATOMIC_ACQUIRE)) {
        if (f < to) {
            size_t n = to - f < CHUNK ? (size_t)(to - f) : CHUNK;
            long w = audio_write(a, pcm + 2 * f, n);
            if (w < 0) {
                printf("splash: the sound stopped (%s)\n", status_str((status_t)w));
                return;
            }
            f += n;
        } else if (!a->s.started && mixer_start(&a->s, now() + NS_PER_S) != OK) {
            return;   /* less than a ring left: audio_write never started it */
        } else {
            jam_nanosleep(now() + POLL / 2);
        }
        if (now() - last_poll < POLL)
            continue;
        last_poll = now();
        uint64_t written = 0, consumed = 0, played = 0;
        if (audio_stream_position_until(a->s.ch, now() + NS_PER_S, &written, &consumed,
                                        &played) != OK)
            return;
        if (played && !__atomic_load_n(&live, __ATOMIC_ACQUIRE)) {
            uint64_t timer = now() - __atomic_load_n(&t0, __ATOMIC_ACQUIRE);
            uint64_t heard = (from + played) * NS_PER_S / SPLASH_RATE;
            printf("splash: the sound is heard from %lu ms: the clock now (the timer was %s by "
                   "%lu ms)\n", (unsigned long)(heard / NS_PER_MS),
                   timer > heard ? "ahead" : "behind",
                   (unsigned long)((timer > heard ? timer - heard : heard - timer) / NS_PER_MS));
        }
        if (played)
            publish(from + played);
        if (f >= to && played >= to - from)
            return;   /* heard to the end */
    }
}

/* A late join starts mid-sound: frames from..from + FADE_IN of pcm faded
 * in (a raised cosine), so it doesn't start with a step. */
void sound_fade_in(int16_t *pcm, size_t frames, size_t from)
{
    for (size_t i = 0; i < FADE_IN && from + i < frames; i++) {
        double g = 0.5 - 0.5 * cosd(3.141592653589793 * (double)i / FADE_IN);
        for (int c = 0; c < 2; c++)
            pcm[2 * (from + i) + c] = (int16_t)(pcm[2 * (from + i) + c] * g);
    }
}

static void sound_main(void *arg)
{
    (void)arg;
    int16_t *pcm = NULL;
    size_t frames = 0;
    status_t st = sound_decode(file, file_len, &pcm, &frames);
    struct audio_out a;
    if (st == OK)
        st = audio_open_as(&a, SPLASH_RATE, 2, "splash");
    if (st != OK) {
        printf("splash: playing without sound (%s)\n", status_str(st));
        __atomic_store_n(&state, SOUND_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&ended, true, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&state, SOUND_READY, __ATOMIC_RELEASE);
    /* The video starts the clock. */
    while (!__atomic_load_n(&t0, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&stop_asked, __ATOMIC_ACQUIRE))
        jam_nanosleep(now() + NS_PER_MS);
    /* With the sound ready in time, video and sound start together: from
     * frame 0, the clock held at 0 until it is heard. Late, the sound joins
     * where the animation is by the time it is heard: at boot the splash's
     * is the only stream, and the mixer opens the output with its first
     * frames first, heard as soon as the driver's stream runs (15 ms in
     * QEMU; HEARD_IN); a wrong guess costs one jump of the video when the
     * clock becomes the sound's position. A late join fades in. */
    uint64_t from = 0;
    if (!__atomic_load_n(&await_sound, __ATOMIC_RELAXED)) {
        from = (clock_now() + HEARD_IN) * SPLASH_RATE / NS_PER_S;
        sound_fade_in(pcm, frames, from);
    }
    if (from < frames) {
        printf("splash: sound joins at %lu ms of the animation%s\n",
               (unsigned long)(from * 1000 / SPLASH_RATE), from ? ", faded in" : "");
        play(&a, pcm, from, frames);
    } else {
        printf("splash: the sound came too late: playing without it\n");
    }
    audio_close(&a);   /* a stop: what the mixer hasn't taken fades out */
    __atomic_store_n(&ended, true, __ATOMIC_RELEASE);
}

void sound_start(const uint8_t *mpg, size_t len, handle_t audio)
{
    if (!audio) {   /* <audio.h> opens its stream on /svc/audio */
        printf("splash: no audio channel: playing without sound\n");
        return;
    }
    file = mpg;
    file_len = len;
    void *stack = big_alloc(STACK);
    handle_t th;
    __atomic_store_n(&ended, false, __ATOMIC_RELEASE);
    __atomic_store_n(&state, SOUND_PENDING, __ATOMIC_RELEASE);
    if (!stack || thread_spawn("sound", sound_main, NULL, stack, STACK, &th) != OK) {
        printf("splash: no thread for the sound: playing without it\n");
        __atomic_store_n(&state, SOUND_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&ended, true, __ATOMIC_RELEASE);
        return;
    }
    jam_handle_close(th);
}

void sound_stop(void)
{
    __atomic_store_n(&stop_asked, true, __ATOMIC_RELEASE);
}

void sound_wait(uint64_t deadline)
{
    while (!__atomic_load_n(&ended, __ATOMIC_ACQUIRE) && now() < deadline)
        jam_nanosleep(now() + 10 * NS_PER_MS);
}
