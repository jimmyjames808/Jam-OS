/* The mixer's arithmetic: see <mixmath.h>. */
#include <mixmath.h>

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
     * which is arithmetic (gcc on x86) and so rounds halves up. */
    int32_t g = (int32_t)gain;
    for (uint32_t i = 0; i < 2 * frames; i++)
        acc[i] += (in[i] * g + 0x4000) >> 15;
}

void mix_out(int16_t *out, const int32_t *acc, uint32_t frames, uint32_t master)
{
    int64_t m = (int64_t)master;
    for (uint32_t i = 0; i < 2 * frames; i++) {
        int64_t v = (acc[i] * m + 0x4000) >> 15;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
