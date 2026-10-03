/* mixramp: a ramp played through the mixer while the mixer is killed
 * again and again (docs/M11.6-PLAN.md, "The demonstration and its
 * tests"); tools/mixer-restart-test.sh runs it in QEMU and checks the WAV
 * QEMU captured.
 *
 *     mixramp [kills [interval_ms]]     (20 kills, 300 ms apart by default)
 *
 * The ramp is 16-bit stereo at 48 kHz, both channels the same, every
 * frame one more than the last (frame k is k + 1, wrapping at 16 bits):
 * at 0 dB the mixer passes it bit for bit, so the capture must be the
 * ramp with no frame missing, repeated or replaced by silence. While the
 * main thread writes it (mixer_write, as `play` does), a thread of its own
 * asks init to kill the mixer (initctl.kill, which returns once it is
 * dead) every interval and at once makes one cheap call on a second
 * stream of ours (stream_position): the time from the kill to that answer
 * is kill-to-first-answer as a client feels it. A third thread keeps the
 * mixer busy with requests meanwhile, so that kills land while one is in
 * progress (its restart finishes it: the log line says how): the ramp
 * stream's volume set to 0 dB (it must answer 0) and its position, and a
 * stream of another opener of /svc/audio opened and closed (a reply that
 * carries handles). Every call any thread makes must succeed: a restarted
 * mixer is not seen by its clients.
 *
 * It prints the ramp's length, the kills, kill-to-first-answer (median,
 * p99, worst) and the stream's own counters (underruns, late periods, the
 * least written ahead: kept in the mixer's state, so they count across
 * the restarts), and exits 0 if every call succeeded. */
#include <idl/audio.h>
#include <idl/initctl.h>
#include <mixer.h>
#include <os.h>
#include <wants.h>

JAM_WANTS("svc audio\n"
          "svc init\n");

#define CHUNK      1024u               /* frames written at a time */
#define LONG       (20 * NS_PER_S)     /* any one call, a restart included */
#define FIRST_MS   500u                /* ramp played before the first kill */
#define TAIL_MS    1500u               /* ramp played after the last kill */
#define PAD_FRAMES 24000u              /* silence after the ramp (500 ms): QEMU's WAV
                                        * gets what its codec still buffered only
                                        * while the stream runs */
#define KILLS_MAX  200u

static struct mixer_stream probe;              /* the killer's stream (never started) */
static handle_t initctl;
static unsigned kills = 20, interval_ms = 300;
static uint64_t took_us[KILLS_MAX];            /* kill to first answer, each kill */
static unsigned done_kills, kill_errors;
static bool started;                           /* the ramp plays (atomic) */
static bool ramp_done;                         /* the ramp is drained (atomic) */
static struct mixer_stream ramp_s;             /* the ramp's stream */
static handle_t churn_svc;                     /* the third thread's opener of /svc/audio */
static unsigned busy_calls, busy_errors, busy_full;   /* the third thread's (read after it ends) */
static _Alignas(16) uint8_t killer_stack[32 << 10];
static _Alignas(16) uint8_t busy_stack[32 << 10];

static unsigned number(const char *s)
{
    unsigned v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (unsigned)(*s - '0');
    return v;
}

/* One kill and the first answer after it. */
static status_t kill_once(uint64_t *us)
{
    uint8_t name[32] = "mixer";
    uint64_t koid = 0, w, c, p;
    uint64_t t0 = now();
    status_t st = initctl_kill_until(initctl, now() + LONG, name, &koid);
    if (st == OK)
        st = audio_stream_position_until(probe.ch, now() + LONG, &w, &c, &p);
    *us = (now() - t0) / NS_PER_US;
    return st;
}

static void killer(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE))   /* main releases it */
        (void)jam_nanosleep(now() + 10 * NS_PER_MS);
    (void)jam_nanosleep(now() + FIRST_MS * NS_PER_MS);
    for (unsigned i = 0; i < kills; i++) {
        uint64_t us = 0;
        status_t st = kill_once(&us);
        if (st != OK) {
            kill_errors++;
            printf("mixramp: kill %u: %s\n", i + 1, status_str(st));
        }
        took_us[done_kills++] = us;
        (void)jam_nanosleep(now() + (uint64_t)interval_ms * NS_PER_MS);
    }
}

/* One of the third thread's calls (call i): OK, or the failure. */
static status_t busy_call(unsigned i)
{
    uint64_t w, c, p;
    int32_t got = 1;
    struct mixer_stream x;
    status_t st;
    switch (i % 3) {
    case 0:
        st = audio_stream_set_volume_until(ramp_s.ch, now() + LONG, 0, &got);
        return st == OK && got != 0 ? ERR_INTERNAL : st;
    case 1:
        return audio_stream_position_until(ramp_s.ch, now() + LONG, &w, &c, &p);
    default:
        st = mixer_open(churn_svc, "ramp-churn", now() + LONG, &x);
        if (st == OK)
            mixer_close(&x);
        return st;
    }
}

static void busy(void *arg)
{
    (void)arg;
    for (unsigned i = 0; !__atomic_load_n(&ramp_done, __ATOMIC_ACQUIRE); i++) {   /* main sets */
        status_t st = busy_call(i);
        busy_calls++;
        if (st == ERR_NO_RESOURCES) {
            /* The churn opener's last close not seen yet (its cap is
             * MIXER_STREAMS_PER_CLIENT): not a restart's doing. */
            busy_full++;
            (void)jam_nanosleep(now() + NS_PER_MS);
        } else if (st != OK) {
            busy_errors++;
            printf("mixramp: a call (kind %u) failed: %s\n", i % 3, status_str(st));
        }
    }
}

/* The next n frames of the ramp from frame *at. */
static void ramp(int16_t *buf, uint32_t n, uint64_t *at)
{
    for (uint32_t i = 0; i < n; i++, (*at)++)
        buf[2 * i] = buf[2 * i + 1] = (int16_t)(uint16_t)(*at + 1);
}

/* Write the whole ramp (frames), then PAD_FRAMES of silence, and drain:
 * OK or the first failure. mixer_write starts the stream once its ring is
 * full, so the ramp never runs short (an underrun would put silence in
 * it). */
static status_t play(struct mixer_stream *s, uint64_t frames)
{
    int16_t buf[2 * CHUNK];
    uint64_t at = 0;
    status_t st = OK;
    while (st == OK && at < frames) {
        uint32_t n = frames - at < CHUNK ? (uint32_t)(frames - at) : CHUNK;
        ramp(buf, n, &at);
        size_t done = 0;
        st = mixer_write(s, buf, n, now() + LONG, &done);
        if (s->started)
            __atomic_store_n(&started, true, __ATOMIC_RELEASE);   /* killer() reads it */
    }
    memset(buf, 0, sizeof(buf));
    for (uint32_t left = PAD_FRAMES; st == OK && left;) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        size_t done = 0;
        st = mixer_write(s, buf, n, now() + LONG, &done);
        left -= n;
    }
    if (st == OK)
        st = mixer_drain(s, now() + LONG);
    return st;
}

/* took_us[0..n) in order (insertion: at most KILLS_MAX). */
static void sort(unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t v = took_us[i];
        unsigned j = i;
        for (; j > 0 && took_us[j - 1] > v; j--)
            took_us[j] = took_us[j - 1];
        took_us[j] = v;
    }
}

static void report_kills(void)
{
    unsigned n = done_kills;
    if (!n)
        return;
    sort(n);
    printf("mixramp: kill to first answer: %u kills, median %lu us, p99 %lu us, worst %lu us\n",
           n, (unsigned long)took_us[(n - 1) / 2], (unsigned long)took_us[(n * 99 + 99) / 100 - 1],
           (unsigned long)took_us[n - 1]);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        kills = number(argv[1]);
    if (argc > 2)
        interval_ms = number(argv[2]);
    if (kills > KILLS_MAX || !interval_ms) {
        printf("usage: mixramp [kills (at most %u) [interval_ms]]\n", KILLS_MAX);
        return 2;
    }
    initctl = svc_get(SVC_INIT);
    handle_t audio = svc_get(SVC_AUDIO);
    struct mixer_stream *s = &ramp_s;
    if (!initctl || !audio || svc_open(SVC_AUDIO, &churn_svc) != OK ||
        mixer_open(audio, "ramp", now() + LONG, s) != OK ||
        mixer_open(audio, "ramp-probe", now() + LONG, &probe) != OK) {
        printf("mixramp: no mixer, or no /svc/init: run it from the shell\n");
        return 1;
    }
    uint64_t frames = ((uint64_t)FIRST_MS + (uint64_t)kills * interval_ms + TAIL_MS) *
                      MIXER_RATE / 1000;
    handle_t th = HANDLE_INVALID, bh = HANDLE_INVALID;
    status_t st = thread_spawn("killer", killer, NULL, killer_stack, sizeof(killer_stack), &th);
    if (st == OK)
        st = thread_spawn("busy", busy, NULL, busy_stack, sizeof(busy_stack), &bh);
    if (st == OK)
        st = play(s, frames);
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);   /* a ramp that failed: kill anyway */
    signals_t seen;
    /* The killer ends once its kills are done (each bounded by LONG). */
    if (th) {
        (void)jam_object_wait_one(th, SIG_TERMINATED, DEADLINE_NEVER, &seen);
        jam_handle_close(th);
    }
    __atomic_store_n(&ramp_done, true, __ATOMIC_RELEASE);   /* busy() reads it */
    if (bh) {
        (void)jam_object_wait_one(bh, SIG_TERMINATED, DEADLINE_NEVER, &seen);
        jam_handle_close(bh);
    }
    uint32_t under = 0, late = 0, lead = 0, limited = 0, bits = 0;
    uint64_t played = 0;
    status_t sst = audio_stream_stats_until(s->ch, now() + LONG, &under, &late, &lead, &limited,
                                            &bits, &played);
    printf("mixramp: ramp of %lu frames (%lu ms) written and drained: %s; %u kills, %u failed\n",
           (unsigned long)frames, (unsigned long)(frames * 1000 / MIXER_RATE), status_str(st),
           done_kills, kill_errors);
    if (sst == OK)
        printf("mixramp: the stream's counters: %u underrun(s), %u late period(s), least ahead "
               "%u frames (%u ms), %u limited, %u-bit output\n", under, late, lead,
               lead * 1000 / MIXER_RATE, limited, bits);
    printf("mixramp: meanwhile %u other calls, %u failed (%u found the churn opener full)\n",
           busy_calls, busy_errors, busy_full);
    report_kills();
    mixer_close(s);
    mixer_close(&probe);
    jam_handle_close(churn_svc);
    bool ok = st == OK && sst == OK && !kill_errors && done_kills == kills && !busy_errors;
    printf("mixramp: %s\n", ok ? "every call succeeded" : "FAILED");
    return ok ? 0 : 1;
}
