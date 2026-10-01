/* utest: the mixer's arithmetic (user/lib/mixmath.c, <mixmath.h>), which
 * user/services/mixer runs on every period: volumes as Q15 gains, frames
 * scaled and summed, the master gain and the saturation. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <mixmath.h>
#include "utest.h"

bool t_mix_gains(void)
{
    CHECK_EQ(mix_gain(0), MIX_UNITY);
    CHECK_EQ(mix_gain(60), MIX_UNITY);              /* no boost: clamped to 0 dB */
    CHECK_EQ(mix_gain(-60), 16423);                 /* -6 dB: 32768 * 10^(-0.3), rounded */
    CHECK_EQ(mix_gain(-200), 3277);                 /* -20 dB: a tenth */
    CHECK_EQ(mix_gain(-120), 8231);                 /* -12 dB */
    CHECK_EQ(mix_gain(-959), 1);                    /* the quietest that isn't silence */
    CHECK_EQ(mix_gain(-960), 0);
    CHECK_EQ(mix_gain(-100000), 0);
    CHECK_EQ(mix_clamp_volume(5), 0);
    CHECK_EQ(mix_clamp_volume(-2000), MIX_VOLUME_MIN);
    CHECK_EQ(mix_clamp_volume(-61), -61);
    for (int32_t cb = -950; cb < 0; cb += 10)       /* quieter is never louder */
        CHECK(mix_gain(cb) <= mix_gain(cb + 10));
    return true;
}

bool t_mix_unity_is_exact(void)
{
    static int16_t in[2 * 256], out[2 * 256];
    static int32_t acc[2 * 256];
    for (unsigned i = 0; i < 2 * 256; i++)
        in[i] = (int16_t)(i * 257 - 32768);         /* -32768 .. 32767 and between */
    in[0] = -32768;
    in[1] = 32767;
    memset(acc, 0, sizeof(acc));
    mix_add(acc, in, 256, MIX_UNITY);
    mix_out(out, acc, 256, MIX_UNITY);
    for (unsigned i = 0; i < 2 * 256; i++)
        CHECK_EQ(out[i], in[i]);
    return true;
}

bool t_mix_volume_and_master(void)
{
    int16_t in[4] = { 8192, -8192, 1, -1 }, out[4];
    int32_t acc[4] = { 0 };
    mix_add(acc, in, 2, mix_gain(-60));             /* -6 dB */
    CHECK_EQ(acc[0], 4106);                          /* 8192 * 16423 / 32768 = 4105.75 */
    CHECK_EQ(acc[1], -4106);
    CHECK_EQ(acc[2], 1);                             /* 0.501 to the nearest */
    CHECK_EQ(acc[3], -1);                            /* -0.501 too */
    mix_out(out, acc, 2, mix_gain(-60));             /* the master at -6 dB as well */
    CHECK_EQ(out[0], 2058);
    CHECK_EQ(out[1], -2058);
    mix_out(out, acc, 2, 0);                         /* master silent */
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(out[i], 0);
    memset(acc, 0, sizeof(acc));
    mix_add(acc, in, 2, 0);                          /* a silent stream adds nothing */
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(acc[i], 0);
    return true;
}

bool t_mix_saturates(void)
{
    int16_t loud[4] = { 30000, -30000, 32767, -32768 }, out[4];
    int32_t acc[4] = { 0 };
    for (unsigned k = 0; k < MIX_MAX_INPUTS; k++)    /* the most a sum may hold */
        mix_add(acc, loud, 2, MIX_UNITY);
    CHECK_EQ(acc[2], 32767 * MIX_MAX_INPUTS);
    CHECK_EQ(acc[3], -32768 * MIX_MAX_INPUTS);
    mix_out(out, acc, 2, MIX_UNITY);
    CHECK_EQ(out[0], 32767);
    CHECK_EQ(out[1], -32768);
    CHECK_EQ(out[2], 32767);
    CHECK_EQ(out[3], -32768);
    /* Two streams that just fit pass through unchanged. */
    int16_t a[2] = { 16000, -16000 }, b[2] = { 16767, -16768 };
    int32_t two[2] = { 0, 0 };
    mix_add(two, a, 1, MIX_UNITY);
    mix_add(two, b, 1, MIX_UNITY);
    mix_out(out, two, 1, MIX_UNITY);
    CHECK_EQ(out[0], 32767);
    CHECK_EQ(out[1], -32768);
    return true;
}
