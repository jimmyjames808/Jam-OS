/* The mixer's arithmetic: see <mixmath.h>. */
#include <mixmath.h>
#include <os.h>

int32_t mix_clamp_volume(int32_t cb)
{
    return cb > MIX_VOLUME_MAX ? MIX_VOLUME_MAX : cb < MIX_VOLUME_MIN ? MIX_VOLUME_MIN : cb;
}

/* e^x for x in [-2.31, 0] by its series: the 30th term is below 1e-20
 * there, so the sum is as good as a double gets. */
static double exp_small(double x)
{
    double term = 1, sum = 1;
    for (int k = 1; k < 30; k++) {
        term *= x / k;
        sum += term;
    }
    return sum;
}

uint32_t mix_gain(int32_t cb)
{
    cb = mix_clamp_volume(cb);
    if (cb <= MIX_VOLUME_MIN)
        return 0;
    /* 10^(cb / 200): a factor of 0.1 per whole -20 dB, then the rest,
     * which is in (-20 dB, 0], through e^(rest * ln 10 / 200). */
    double v = 1;
    for (; cb <= -200; cb += 200)
        v *= 0.1;
    v *= exp_small(cb * (2.302585092994045684 / 200));
    return (uint32_t)(v * MIX_UNITY + 0.5);
}

void mix_add(int32_t *acc, const int16_t *in, uint32_t frames, uint32_t gain)
{
    /* |in * gain| <= 32768 * 32768 = 2^30: no overflow before the shift,
     * which is arithmetic (gcc on x86) and so rounds halves up. Q15 times
     * a sample, down to Q8: 7 bits. */
    int32_t g = (int32_t)gain;
    for (uint32_t i = 0; i < 2 * frames; i++)
        acc[i] += (in[i] * g + (1 << (14 - MIX_Q))) >> (15 - MIX_Q);
}

void mix_master(int32_t *acc, uint32_t frames, uint32_t master)
{
    if (master >= MIX_UNITY)
        return;
    int64_t m = (int64_t)master;
    for (uint32_t i = 0; i < 2 * frames; i++)
        acc[i] = (int32_t)((acc[i] * m + 0x4000) >> 15);
}

/* ---- the limiter ----------------------------------------------------------------- */

void mix_limit_init(struct mix_limiter *l)
{
    memset(l, 0, sizeof(*l));
    l->gain = 1.0f;
}

/* The gain frame f (two samples) needs to stay inside full scale. */
static float need(const int32_t *f)
{
    float g = 1.0f;
    for (int c = 0; c < 2; c++) {
        float k = f[c] > MIX_FULL ? (float)MIX_FULL / (float)f[c]
                : f[c] < MIX_FLOOR ? (float)MIX_FLOOR / (float)f[c] : 1.0f;
        if (k < g)
            g = k;
    }
    return g;
}

bool mix_limit(struct mix_limiter *l, int32_t *buf, uint32_t frames)
{
    static float g[MIX_LOOKAHEAD + MIX_BLOCK_MAX];   /* one mixer thread; utest too */
    if (frames > MIX_BLOCK_MAX)
        frames = MIX_BLOCK_MAX;   /* a caller bug: the rest passes unlimited */
    const uint32_t n = MIX_LOOKAHEAD + frames;
    memcpy(buf, l->held, sizeof(l->held));
    bool over = false;
    for (uint32_t i = 0; i < 2 * n; i++) {
        int32_t v = buf[i], a = v < 0 ? -v : v;
        if (a > l->peak)
            l->peak = a;
        if (v > MIX_FULL || v < MIX_FLOOR)
            over = true;
    }
    bool limited = false;
    if (over || l->gain < 1.0f) {
        /* Backwards: each frame's gain at most what it needs, and at most
         * 1/MIX_LOOKAHEAD above the next one's, so the gain is already
         * down when a loud frame comes. */
        float next = 1.0f;
        for (uint32_t i = n; i-- > 0;) {
            float k = need(buf + 2 * i), up = next + 1.0f / MIX_LOOKAHEAD;
            next = k < up ? k : up;
            g[i] = next;
        }
        /* Forwards over the block to play: the gain rises no faster than
         * the release, from where the last block left it. */
        float cur = l->gain;
        for (uint32_t i = 0; i < frames; i++) {
            float up = cur + 1.0f / MIX_RELEASE_FRAMES;
            cur = g[i] < up ? g[i] : up;
            if (cur >= 1.0f) {
                cur = 1.0f;
                continue;
            }
            limited = true;
            l->limited++;
            for (int c = 0; c < 2; c++) {
                double v = (double)buf[2 * i + c] * cur;
                buf[2 * i + c] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
            }
        }
        l->gain = cur;
    }
    for (uint32_t i = 0; i < 2 * frames; i++)   /* rounding can't pass them, but be sure */
        buf[i] = buf[i] > MIX_FULL ? MIX_FULL : buf[i] < MIX_FLOOR ? MIX_FLOOR : buf[i];
    memcpy(l->held, buf + 2 * frames, sizeof(l->held));
    return limited;
}

/* ---- out to the device ------------------------------------------------------------ */

void mix_out32(int32_t *out, const int32_t *acc, uint32_t frames)
{
    for (uint32_t i = 0; i < 2 * frames; i++) {
        int32_t v = acc[i] > MIX_FULL ? MIX_FULL : acc[i] < MIX_FLOOR ? MIX_FLOOR : acc[i];
        out[i] = (int32_t)((uint32_t)v << (16 - MIX_Q));
    }
}

/* xorshift32: a fast generator, plenty for dither. */
static uint32_t next_random(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

void mix_out16(int16_t *out, const int32_t *acc, uint32_t frames, uint32_t *seed)
{
    const int32_t frac = (1 << MIX_Q) - 1, half = 1 << (MIX_Q - 1);
    for (uint32_t f = 0; f < frames; f++) {
        int32_t d = 0;
        if ((acc[2 * f] | acc[2 * f + 1]) & frac) {
            /* TPDF: the difference of two uniform values in [0, 255]:
             * triangular over (-1, +1) of a 16-bit step. */
            uint32_t r = next_random(seed);
            d = (int32_t)(r & 0xff) - (int32_t)(r >> 8 & 0xff);
        }
        for (int c = 0; c < 2; c++) {
            int32_t v = acc[2 * f + c];
            int64_t w = (int64_t)v + ((v & frac) ? d : 0) + half;
            w >>= MIX_Q;   /* floor of (v + d) / 256 + 1/2: rounded to nearest */
            out[2 * f + c] = (int16_t)(w > 32767 ? 32767 : w < -32768 ? -32768 : w);
        }
    }
}
