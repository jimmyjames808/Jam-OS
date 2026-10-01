/* music: what is heard, in sixteen frequency bands (music.idl's `levels`),
 * for a view such as jamjar to draw.
 *
 * The player hands every chunk it writes to spec_feed with the stream
 * frame (48 kHz) its first sample lands on. The chunk is mixed to mono and
 * kept in a window of SPEC_N samples; every hop (SPEC_HOP samples at
 * 48 kHz, more at higher rates so a hop is always about 11 ms) the window
 * gets a Hann taper and a 1024-point FFT, and the power of its bins is
 * summed into sixteen bands log-spaced from 40 Hz to 16 kHz (a band too
 * narrow to hold a bin centre takes the nearest bin). Each band's sum is
 * in dB against a full-scale sine's, plus a tilt of 3 dB an octave from
 * 1 kHz (music has much more energy low than high), and -70..-10 dB maps
 * to 0..255. The hop's RMS, -60..0 dBFS, is the overall `level`.
 *
 * The player writes up to 1.37 s ahead of what is heard, so each result
 * goes into a ring with the stream frame of its window's middle, and
 * spec_at answers the newest result at or before the frame the mixer says
 * is heard. A skip drops what was written ahead (spec_cut); a new stream
 * starts the ring again (spec_reset).
 *
 * Plain float arithmetic, no libm: the twiddles come from a rotation
 * whose step is a Taylor series, the logarithm from the float's exponent
 * and a short series. About 50 kFLOP a hop, under 5 MFLOP a second. */
#include "music.h"

#define FULL_SCALE 32767.0f
#define DB_LOW     (-70.0f)   /* a band at 0 */
#define DB_HIGH    (-10.0f)   /* ... and at 255 */
#define LEVEL_LOW  (-60.0f)   /* the level at 0 (0 dBFS is 255) */
#define TILT_DB    3.0f       /* per octave above 1 kHz (below: less) */
#define F_LOW      40.0
#define F_HIGH     16000.0

/* log2 of x > 0, to about 1e-6: the exponent, and ln of the mantissa m
 * (1 <= m < 2) from the series 2 (y + y^3/3 + ... + y^9/9), y = (m-1)/(m+1). */
static float log2_approx(float x)
{
    union { float f; uint32_t u; } v = { x };
    int e = (int)((v.u >> 23) & 0xff) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float y = (v.f - 1.0f) / (v.f + 1.0f), y2 = y * y;
    float ln = 2.0f * y * (1.0f + y2 * (1.0f / 3 + y2 * (1.0f / 5 + y2 * (1.0f / 7 + y2 / 9))));
    return (float)e + ln * 1.4426950f;
}

static float db10(float power)
{
    return power > 1e-30f ? 3.0103f * log2_approx(power) : -300.0f;
}

static uint8_t to_byte(float x, float lo, float hi)
{
    float t = (x - lo) * 255.0f / (hi - lo);
    return t <= 0.0f ? 0 : t >= 255.0f ? 255 : (uint8_t)(t + 0.5f);
}

/* cos and sin of a small angle a (|a| < 0.01) by their series. */
static void small_angle(double a, double *c, double *s)
{
    double a2 = a * a;
    *c = 1.0 - a2 / 2.0 * (1.0 - a2 / 12.0 * (1.0 - a2 / 30.0));
    *s = a * (1.0 - a2 / 6.0 * (1.0 - a2 / 20.0 * (1.0 - a2 / 42.0)));
}

void spec_init(struct spectrum *s)
{
    memset(s, 0, sizeof(*s));
    double c1, s1, c = 1.0, sn = 0.0;
    small_angle(2.0 * 3.14159265358979323846 / SPEC_N, &c1, &s1);
    for (uint32_t k = 0; k < SPEC_N / 2; k++) {
        s->cosv[k] = (float)c;
        s->sinv[k] = (float)sn;
        double nc = c * c1 - sn * s1;
        sn = sn * c1 + c * s1;
        c = nc;
    }
    /* Hann: 0.5 - 0.5 cos(2 pi n / N), the cosines from the twiddles. */
    for (uint32_t n = 0; n < SPEC_N; n++) {
        float cs = n < SPEC_N / 2 ? s->cosv[n] : -s->cosv[n - SPEC_N / 2];
        s->win[n] = 0.5f - 0.5f * cs;
    }
    uint32_t bits = 0;
    while ((1u << bits) < SPEC_N)
        bits++;
    for (uint32_t i = 0; i < SPEC_N; i++) {
        uint32_t r = 0;
        for (uint32_t b = 0; b < bits; b++)
            r |= ((i >> b) & 1u) << (bits - 1 - b);
        s->rev[i] = (uint16_t)r;
    }
}

/* The bins each band sums, for this rate. */
static void set_rate(struct spectrum *s, uint32_t rate)
{
    s->rate = rate;
    s->hop = SPEC_HOP * (rate > AUDIO_RATE ? (rate + AUDIO_RATE - 1) / AUDIO_RATE : 1);
    double bw = (double)rate / SPEC_N, ratio = 1.0, step = 1.0;
    /* step = (F_HIGH / F_LOW) ^ (1 / SPEC_BANDS), by bisection on its power. */
    double lo = 1.0, hi = 2.0;
    for (int it = 0; it < 60; it++) {
        double mid = (lo + hi) / 2, p = 1.0;
        for (uint32_t b = 0; b < SPEC_BANDS; b++)
            p *= mid;
        if (p * F_LOW < F_HIGH)
            lo = mid;
        else
            hi = mid;
    }
    step = lo;
    for (uint32_t b = 0; b < SPEC_BANDS; b++) {
        double f0 = F_LOW * ratio, f1 = f0 * step, mid = (f0 + f1) / 2;
        ratio *= step;
        uint32_t k0 = (uint32_t)(f0 / bw + 0.999999), k1 = (uint32_t)(f1 / bw);
        if (k1 >= SPEC_N / 2)
            k1 = SPEC_N / 2 - 1;
        if (k0 > k1) {   /* no bin centre inside: the nearest one */
            k0 = k1 = (uint32_t)(mid / bw + 0.5);
            k0 = k1 = k0 >= SPEC_N / 2 ? SPEC_N / 2 - 1 : k0 ? k0 : 1;
        }
        s->bin0[b] = (uint16_t)k0;
        s->bin1[b] = (uint16_t)k1;
        /* The tilt, from the band's middle (geometric) in octaves from 1 kHz. */
        s->tilt[b] = TILT_DB * (log2_approx((float)(f0 / 1000.0)) +
                                0.5f * log2_approx((float)step));
    }
}

/* In-place radix-2 FFT of s->re/s->im (bit-reversed copy already made). */
static void fft(struct spectrum *s)
{
    for (uint32_t len = 2; len <= SPEC_N; len <<= 1) {
        uint32_t half = len / 2, stride = SPEC_N / len;
        for (uint32_t i = 0; i < SPEC_N; i += len)
            for (uint32_t j = 0; j < half; j++) {
                float wr = s->cosv[j * stride], wi = -s->sinv[j * stride];
                float *ar = &s->re[i + j], *ai = &s->im[i + j];
                float *br = &s->re[i + j + half], *bi = &s->im[i + j + half];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr;
                *bi = *ai - ti;
                *ar += tr;
                *ai += ti;
            }
    }
}

static void push(struct spectrum *s, const struct spec_entry *e)
{
    s->ring[s->head] = *e;
    s->head = (s->head + 1) % SPEC_RING;
    if (s->count < SPEC_RING)
        s->count++;
}

/* The window as it is now (ending at the sample just taken), analysed. */
static void analyse(struct spectrum *s, int64_t at)
{
    for (uint32_t n = 0; n < SPEC_N; n++) {
        uint32_t r = s->rev[n];
        s->re[r] = s->in[(s->pos + n) % SPEC_N] * s->win[n];
        s->im[r] = 0.0f;
    }
    fft(s);
    /* A full-scale sine's bins sum to 3/32 N^2 A^2 (Hann, one side). */
    const float ref = 3.0f / 32.0f * (float)SPEC_N * (float)SPEC_N * FULL_SCALE * FULL_SCALE;
    struct spec_entry e = { .at = at };
    for (uint32_t b = 0; b < SPEC_BANDS; b++) {
        float sum = 0.0f;
        for (uint32_t k = s->bin0[b]; k <= s->bin1[b]; k++)
            sum += s->re[k] * s->re[k] + s->im[k] * s->im[k];
        e.band[b] = to_byte(db10(sum / ref) + s->tilt[b], DB_LOW, DB_HIGH);
    }
    float rms2 = s->sq / (float)(s->nsq ? s->nsq : 1) / (FULL_SCALE * FULL_SCALE);
    e.level = to_byte(db10(rms2), LEVEL_LOW, 0.0f);
    s->sq = 0.0f;
    s->nsq = 0;
    push(s, &e);
}

void spec_feed(struct spectrum *s, const int16_t *pcm, size_t frames, unsigned channels,
               uint32_t rate, int64_t at)
{
    if (!rate || (channels != 1 && channels != 2))
        return;
    if (rate != s->rate)
        set_rate(s, rate);
    for (size_t k = 0; k < frames; k++) {
        float x = channels == 2 ? ((float)pcm[2 * k] + (float)pcm[2 * k + 1]) * 0.5f
                                : (float)pcm[k];
        s->in[s->pos] = x;
        s->pos = (s->pos + 1) % SPEC_N;
        s->sq += x * x;
        s->nsq++;
        if (s->fill < SPEC_N)
            s->fill++;
        if (++s->since < s->hop || s->fill < SPEC_N)
            continue;
        s->since = 0;
        /* The window's middle: SPEC_N / 2 samples before this one. */
        int64_t mid = (int64_t)k + 1 - SPEC_N / 2;
        analyse(s, at + mid * (int64_t)AUDIO_RATE / (int64_t)rate);
    }
}

bool spec_at(const struct spectrum *s, int64_t heard, struct spec_entry *out)
{
    memset(out, 0, sizeof(*out));
    /* The newest entry at or before `heard`, searched from the newest. */
    for (uint32_t i = 0; i < s->count; i++) {
        const struct spec_entry *e = &s->ring[(s->head + SPEC_RING - 1 - i) % SPEC_RING];
        if (e->at > heard)
            continue;
        if (heard - e->at > SPEC_STALE)
            return false;   /* nothing near what is heard: silence */
        *out = *e;
        return true;
    }
    return false;
}

void spec_cut(struct spectrum *s, int64_t from)
{
    while (s->count && s->ring[(s->head + SPEC_RING - 1) % SPEC_RING].at >= from) {
        s->head = (s->head + SPEC_RING - 1) % SPEC_RING;
        s->count--;
    }
    s->fill = s->since = 0;   /* the window held what was dropped */
    s->sq = 0.0f;
    s->nsq = 0;
}

void spec_reset(struct spectrum *s)
{
    s->head = s->count = 0;
    s->fill = s->since = 0;
    s->sq = 0.0f;
    s->nsq = 0;
}
