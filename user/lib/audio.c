/* audio: a program's sound output (<audio.h>). The format conversions and
 * the resampler at the top are pure; the rest is the backend, a stream on
 * the mixer (<mixer.h>: its ring is written through our own mapping, no
 * call per write), and is the only place that knows it. The mixer takes
 * only 48 kHz stereo 16-bit frames, so everything is converted here
 * first. */
#include <audio.h>
#include <idl/audio.h>
#include <mixer.h>

#define BLOCK      1024u          /* output frames converted at a time */
#define FADE       240u           /* frames: 5 ms of fade-out at close */
#define TAIL       4096u          /* frames of silence after a drain: a period */
#define OPEN_WAIT  (15 * NS_PER_S)   /* a mixer init is restarting answers late */
#define SOON       (5 * NS_PER_S)
#define DRAIN_WAIT (10 * NS_PER_S)

/* ---- conversions (pure) -------------------------------------------------------- */

static int16_t clamp16(int64_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void audio_s16_from_u8(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++)
        out[i] = (int16_t)(((int)in[i] - 128) * 256);
}

void audio_s16_from_s24le(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++, in += 3) {
        int32_t v = (int32_t)((uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16);
        v = (v ^ 0x800000) - 0x800000;            /* sign-extend bit 23 */
        out[i] = clamp16(((int64_t)v + 128) >> 8);  /* to nearest; >> floors */
    }
}

void audio_s16_from_s32le(int16_t *out, const uint8_t *in, size_t samples)
{
    for (size_t i = 0; i < samples; i++, in += 4) {
        int32_t v = (int32_t)((uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
                              (uint32_t)in[3] << 24);
        out[i] = clamp16(((int64_t)v + 32768) >> 16);
    }
}

/* ---- the resampler (pure) ---------------------------------------------------------- */

#define RS_ATTEN   100.0     /* dB: the stopband (and 1e-5 of ripple in the passband) */
#define RS_PASS    0.4535    /* the passband's edge, of the lower rate */
#define PI         3.14159265358979323846

typedef float v4f __attribute__((vector_size(16), aligned(4)));   /* unaligned loads */

/* No libm here: what the table needs, in double. */
static double rs_sin(double x)
{
    x -= 2 * PI * (double)(int64_t)(x / (2 * PI));   /* (-2pi, 2pi) */
    if (x > PI)
        x -= 2 * PI;
    else if (x < -PI)
        x += 2 * PI;
    double term = x, sum = x, x2 = x * x;
    for (int k = 1; k < 20; k++) {   /* |x| <= pi: the 20th term is below 1e-25 */
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

static double rs_sqrt(double x)
{
    if (x <= 0)
        return 0;
    double r = x > 1 ? x : 1;
    for (int k = 0; k < 60; k++)
        r = 0.5 * (r + x / r);
    return r;
}

/* The modified Bessel function I0, by its series. */
static double rs_i0(double x)
{
    double term = 1, sum = 1, q = x * x / 4;
    for (int k = 1; k < 200 && term > sum * 1e-17; k++) {
        term *= q / ((double)k * k);
        sum += term;
    }
    return sum;
}

static uint32_t rs_gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* The kernel at tau input frames from the output's time: a sinc with its
 * cutoff at fc (of the input rate), under a Kaiser window `half` frames
 * wide either side. */
static double rs_kernel(double tau, double fc, double half, double beta, double i0b)
{
    double r = tau / half;
    if (r <= -1 || r >= 1)
        return 0;
    double x = 2 * fc * tau, s = x == 0 ? 1 : rs_sin(PI * x) / (PI * x);
    return 2 * fc * s * rs_i0(beta * rs_sqrt(1 - r * r)) / i0b;
}

int audio_rs_init(struct audio_rs *rs, unsigned in_rate, unsigned out_rate, unsigned channels)
{
    memset(rs, 0, sizeof(*rs));
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->channels = channels;
    rs->seed = 0x2545f491u;
    if (in_rate == out_rate)
        return OK;
    uint32_t g = rs_gcd(in_rate, out_rate);
    rs->L = out_rate / g;
    rs->M = in_rate / g;
    rs->P = rs->L <= AUDIO_RS_PHASES ? rs->L : AUDIO_RS_PHASES;
    double low = in_rate < out_rate ? in_rate : out_rate;
    double pass = RS_PASS * low / in_rate, stop = 0.5 * low / in_rate;   /* of the input rate */
    /* Kaiser's estimate of the length for RS_ATTEN over (stop - pass). */
    double n = (RS_ATTEN - 7.95) / (14.36 * (stop - pass));
    uint32_t taps = ((uint32_t)n + 4) & ~3u;
    if (taps > AUDIO_RS_TAPS_MAX)
        taps = AUDIO_RS_TAPS_MAX;
    rs->taps = taps;
    rs->coef = malloc((size_t)(rs->P + 1) * taps * sizeof(float));
    rs->hist = malloc((size_t)2 * 2 * taps * sizeof(float));
    if (!rs->coef || !rs->hist) {
        audio_rs_free(rs);
        return ERR_NO_MEMORY;
    }
    double beta = 0.1102 * (RS_ATTEN - 8.7), i0b = rs_i0(beta), half = taps / 2.0;
    double fc = (pass + stop) / 2;
    for (uint32_t q = 0; q <= rs->P; q++) {
        /* Output at q/P past input frame `base` uses frames base + 1 -
         * taps/2 + k (k = 0 .. taps - 1): tau = q/P + taps/2 - 1 - k. */
        float *c = rs->coef + (size_t)q * taps;
        double sum = 0;
        for (uint32_t k = 0; k < taps; k++) {
            double v = rs_kernel((double)q / rs->P + half - 1 - k, fc, half, beta, i0b);
            c[k] = (float)v;
            sum += v;
        }
        for (uint32_t k = 0; k < taps; k++)   /* each phase passes DC exactly */
            c[k] = (float)(c[k] / sum);
    }
    audio_rs_reset(rs);
    return OK;
}

void audio_rs_reset(struct audio_rs *rs)
{
    rs->base = rs->n_in = 0;
    rs->phase = 0;
    rs->tail = 0;
    rs->flushing = false;
    if (rs->hist)
        memset(rs->hist, 0, (size_t)2 * 2 * rs->taps * sizeof(float));
}

void audio_rs_free(struct audio_rs *rs)
{
    free(rs->coef);
    free(rs->hist);
    rs->coef = rs->hist = NULL;
}

static float rs_dot(const float *a, const float *b, uint32_t n)
{
    v4f s0 = { 0, 0, 0, 0 }, s1 = s0;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 += *(const v4f *)(a + i) * *(const v4f *)(b + i);
        s1 += *(const v4f *)(a + i + 4) * *(const v4f *)(b + i + 4);
    }
    for (; i < n; i += 4)
        s0 += *(const v4f *)(a + i) * *(const v4f *)(b + i);
    s0 += s1;
    return s0[0] + s0[1] + s0[2] + s0[3];
}

/* xorshift32 (the dither's generator). */
static uint32_t rs_random(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* y to 16 bits: as it is if it is whole, else with TPDF dither (d, in
 * (-1, 1)), rounded to nearest. */
static int16_t rs_out(float y, float d)
{
    int32_t w = (int32_t)y;
    if ((float)w != y)
        y += d;
    float r = y < 0 ? y - 0.5f : y + 0.5f;
    if (r >= 32767.0f)
        return 32767;
    if (r <= -32768.0f)
        return -32768;
    return (int16_t)(int32_t)r;
}

/* The output at (base, phase) from the history: both channels into o. */
static void rs_emit(struct audio_rs *rs, int16_t *o)
{
    uint32_t T = rs->taps, off = (uint32_t)(rs->n_in % T);
    float y[2];
    uint64_t q = (uint64_t)rs->phase * rs->P;
    uint32_t qi = (uint32_t)(q / rs->L);
    float frac = (float)(q % rs->L) / (float)rs->L;
    const float *c0 = rs->coef + (size_t)qi * T;
    for (unsigned ch = 0; ch < rs->channels; ch++) {
        const float *h = rs->hist + (size_t)ch * 2 * T + off;   /* the last T, oldest first */
        y[ch] = rs_dot(c0, h, T);
        if (frac != 0.0f)   /* L > AUDIO_RS_PHASES: between two table phases */
            y[ch] += frac * (rs_dot(c0 + T, h, T) - y[ch]);
    }
    uint32_t r = rs_random(&rs->seed);
    float d = ((float)(r & 0xffff) - (float)(r >> 16)) / 65536.0f;   /* TPDF, (-1, 1) */
    o[0] = rs_out(y[0], d);
    o[1] = rs->channels == 2 ? rs_out(y[1], d) : o[0];
}

size_t audio_rs_run(struct audio_rs *rs, const int16_t *in, size_t in_frames, size_t *used,
                    int16_t *out, size_t cap)
{
    unsigned ch = rs->channels;
    size_t i = 0, o = 0;
    if (!rs->coef) {   /* the same rate: the frames as they are */
        for (; i < in_frames && o < cap; i++, o++) {
            out[2 * o] = in[i * ch];
            out[2 * o + 1] = in[i * ch + ch - 1];
        }
        *used = i;
        return o;
    }
    uint32_t T = rs->taps, half = T / 2;
    for (;;) {
        /* Every output whose last input frame (base + taps/2) is in. */
        while (rs->base + half < rs->n_in) {
            if (o == cap)
                goto done;
            rs_emit(rs, out + 2 * o);
            o++;
            rs->phase += rs->M;
            rs->base += rs->phase / rs->L;
            rs->phase %= rs->L;
        }
        if (i == in_frames)
            break;
        uint32_t at = (uint32_t)(rs->n_in % T);
        for (unsigned c = 0; c < ch; c++) {
            float v = in[i * ch + c];
            rs->hist[(size_t)c * 2 * T + at] = v;
            rs->hist[(size_t)c * 2 * T + at + T] = v;
        }
        rs->n_in++;
        i++;
    }
done:
    *used = i;
    return o;
}

size_t audio_rs_flush(struct audio_rs *rs, int16_t *out, size_t cap)
{
    static const int16_t zero[2];
    if (!rs->coef)
        return 0;
    if (!rs->flushing) {
        rs->flushing = true;
        rs->tail = rs->taps / 2 + 1;
    }
    size_t o = 0, used = 0;
    while (o < cap) {
        size_t n = audio_rs_run(rs, zero, rs->tail ? 1 : 0, &used, out + 2 * o, cap - o);
        o += n;
        if (used)
            rs->tail--;
        else if (!n)
            break;
    }
    return o;
}

/* ---- the backend: a mixer stream ------------------------------------------------- */

int audio_open_as(struct audio_out *a, unsigned rate, unsigned channels, const char *name)
{
    memset(a, 0, sizeof(*a));
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX || (channels != 1 && channels != 2))
        return ERR_NOT_SUPPORTED;
    handle_t svc = svc_get(SVC_AUDIO);
    if (svc == HANDLE_INVALID)
        return ERR_NOT_FOUND;   /* /svc/audio isn't ours (not on our list) */
    status_t st = mixer_open(svc, name, now() + OPEN_WAIT, &a->s);
    if (st != OK)
        return st;
    st = audio_rs_init(&a->rs, rate, AUDIO_RATE, channels);
    if (st != OK) {
        mixer_close(&a->s);
        return st;
    }
    a->open = true;
    a->ring_min = UINT64_MAX;
    a->rate = rate;
    a->channels = channels;
    return OK;
}

int audio_open(struct audio_out *a, unsigned rate, unsigned channels)
{
    return audio_open_as(a, rate, channels, "audio");
}

long audio_write(struct audio_out *a, const void *frames, size_t nframes)
{
    if (!a->open)
        return ERR_BAD_STATE;
    const int16_t *in = frames;
    size_t left = nframes;
    int16_t out[2 * BLOCK];
    while (left) {
        size_t used = 0, done = 0;
        size_t n = audio_rs_run(&a->rs, in, left, &used, out, BLOCK);
        if (!n && !used)
            return ERR_INTERNAL;
        /* Blocks while the ring is full: the mixer takes a period at a
         * time, so room comes within 43 ms while it plays. */
        if (a->s.started) {
            uint64_t r = __atomic_load_n(&a->s.hdr->read, __ATOMIC_ACQUIRE);
            uint64_t queued = a->s.write > r ? a->s.write - r : 0;
            if (queued < a->ring_min)
                a->ring_min = queued;
        }
        status_t st = mixer_write(&a->s, out, n, now() + SOON, &done);
        if (st != OK)
            return st;
        in += used * a->channels;
        left -= used;
    }
    return (long)nframes;
}

int audio_drain(struct audio_out *a)
{
    if (!a->open)
        return ERR_BAD_STATE;
    /* The resampler's last outputs (it runs taps/2 input frames behind). */
    int16_t out[2 * BLOCK];
    size_t n;
    status_t st = OK;
    while (st == OK && (n = audio_rs_flush(&a->rs, out, BLOCK)) > 0) {
        size_t done = 0;
        st = mixer_write(&a->s, out, n, now() + SOON, &done);
    }
    audio_rs_reset(&a->rs);
    if (st != OK)
        return st;
    if (!a->s.write)
        return OK;   /* nothing was ever written */
    st = mixer_drain(&a->s, now() + DRAIN_WAIT);
    static const int16_t quiet[2 * BLOCK];
    for (unsigned k = 0; st == OK && k < TAIL / BLOCK; k++) {
        size_t done = 0;
        st = mixer_write(&a->s, quiet, BLOCK, now() + SOON, &done);
    }
    if (st == OK)
        st = mixer_drain(&a->s, now() + DRAIN_WAIT);
    return st;
}

/* Drop what the mixer has not taken yet: the FADE frames after its `read`
 * faded down to nothing, `write` moved back to their end, and the fade
 * waited for. (The mixer may take a frame or two of them unfaded while we
 * write: one period's worth of race in a 5 ms fade, at worst a click.) */
static void fade_out(struct audio_out *a)
{
    struct mixer_stream *s = &a->s;
    uint64_t r = __atomic_load_n(&s->hdr->read, __ATOMIC_ACQUIRE);
    if (s->write <= r)
        return;   /* nothing queued: it ended where the writes did */
    uint64_t f = s->write - r < FADE ? s->write - r : FADE;
    for (uint64_t i = 0; i < f; i++) {
        int16_t *x = s->data + 2 * ((r + i) % s->frames);
        int32_t g = (int32_t)(f - i);
        x[0] = (int16_t)(x[0] * g / (int32_t)(f + 1));
        x[1] = (int16_t)(x[1] * g / (int32_t)(f + 1));
    }
    s->write = r + f;
    __atomic_store_n(&s->hdr->write, s->write, __ATOMIC_RELEASE);
    (void)mixer_drain(s, now() + SOON);
}

int audio_set_input(struct audio_out *a, unsigned rate, unsigned channels)
{
    if (!a->open)
        return ERR_BAD_STATE;
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX || (channels != 1 && channels != 2))
        return ERR_NOT_SUPPORTED;
    if (rate == a->rate && channels == a->channels)
        return OK;   /* the resampler goes on as it is: no seam */
    struct audio_rs rs;
    status_t st = audio_rs_init(&rs, rate, AUDIO_RATE, channels);
    if (st != OK)
        return st;
    /* The old resampler's last outputs first (it runs taps/2 frames behind). */
    int16_t out[2 * BLOCK];
    size_t n;
    while (st == OK && (n = audio_rs_flush(&a->rs, out, BLOCK)) > 0) {
        size_t done = 0;
        st = mixer_write(&a->s, out, n, now() + SOON, &done);
    }
    if (st != OK) {
        audio_rs_free(&rs);
        return st;
    }
    audio_rs_free(&a->rs);
    a->rs = rs;
    a->rate = rate;
    a->channels = channels;
    return OK;
}

int audio_discard(struct audio_out *a)
{
    if (!a->open)
        return ERR_BAD_STATE;
    if (a->s.started)
        fade_out(a);
    audio_rs_reset(&a->rs);
    return OK;
}

int audio_get_volume(struct audio_out *a, int *centibels)
{
    if (!a->open)
        return ERR_BAD_STATE;
    int32_t vol = 0, master = 0, device = 0;
    status_t st = audio_stream_levels_until(a->s.ch, now() + SOON, &vol, &master, &device);
    if (st == OK)
        *centibels = vol + master + device;
    return st;
}

int audio_set_volume(struct audio_out *a, int centibels)
{
    if (!a->open)
        return ERR_BAD_STATE;
    return mixer_set_volume(&a->s, centibels, now() + SOON, NULL);
}

int audio_stats(struct audio_out *a, struct audio_stats *st)
{
    if (!a->open)
        return ERR_BAD_STATE;
    *st = (struct audio_stats){ .ring_min = a->ring_min, .ring_frames = a->s.frames };
    return audio_stream_stats_until(a->s.ch, now() + SOON, &st->underruns, &st->late,
                                    &st->min_lead, &st->limited, &st->bits, &st->played);
}

void audio_close(struct audio_out *a)
{
    if (a->open && a->s.started)
        fade_out(a);
    if (a->open)
        mixer_close(&a->s);   /* the mixer drops the stream */
    audio_rs_free(&a->rs);
    memset(a, 0, sizeof(*a));
}

bool audio_parse_db(const char *s, int *centibels)
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
    *centibels = neg ? -v : v;
    return true;
}
