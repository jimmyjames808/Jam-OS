/* utest: the mixer's arithmetic (user/lib/mixmath.c, <mixmath.h>), which
 * user/services/mixer runs on every period: volumes as Q15 gains, frames
 * scaled and summed in Q8, the master gain, the limiter, and the 16-bit
 * (dithered) and 32-bit outputs. */
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

/* One block through the whole chain the mixer runs: mix_add, the master,
 * the limiter (whose output is MIX_LOOKAHEAD frames late), then out16 and
 * out32. */
static int32_t buf[2 * (MIX_LOOKAHEAD + MIX_BLOCK_MAX)];

bool t_mix_unity_is_exact(void)
{
    enum { N = 256 };
    static int16_t in[2 * N], out[2 * N];
    static int32_t out32[2 * N];
    for (unsigned i = 0; i < 2 * N; i++)
        in[i] = (int16_t)(i * 257 - 32768);         /* -32768 .. 32767 and between */
    in[0] = -32768;
    in[1] = 32767;
    struct mix_limiter l;
    mix_limit_init(&l);
    uint32_t seed = 1;
    /* The block, then a block of silence to push it through the delay. */
    for (unsigned pass = 0; pass < 2; pass++) {
        memset(buf, 0, sizeof(buf));
        if (!pass)
            mix_add(buf + 2 * MIX_LOOKAHEAD, in, N, MIX_UNITY);
        mix_master(buf + 2 * MIX_LOOKAHEAD, N, MIX_UNITY);
        CHECK(!mix_limit(&l, buf, N));
        mix_out16(out, buf, N, &seed);
        mix_out32(out32, buf, N);
        for (unsigned f = 0; f < N; f++) {
            /* frame f of this block is input frame f - MIX_LOOKAHEAD + N * pass */
            int64_t k = (int64_t)f - MIX_LOOKAHEAD + (int64_t)N * pass;
            for (unsigned c = 0; c < 2; c++) {
                int16_t want = k >= 0 && k < N ? in[2 * k + c] : 0;
                CHECK_EQ(out[2 * f + c], want);
                CHECK_EQ(out32[2 * f + c], (int32_t)((uint32_t)(uint16_t)want << 16));
            }
        }
    }
    CHECK_EQ(l.limited, 0);
    CHECK_EQ(seed, 1);                               /* no fraction anywhere: no dither drawn */
    return true;
}

bool t_mix_volume_and_master(void)
{
    int16_t in[4] = { 8192, -8192, 1, -1 };
    int32_t acc[4] = { 0 }, out32[4];
    mix_add(acc, in, 2, mix_gain(-60));             /* -6 dB */
    CHECK_EQ(acc[0], 1051072);                       /* 8192 * 16423 / 128 = 1051072 (Q8) */
    CHECK_EQ(acc[1], -1051072);
    CHECK_EQ(acc[2], 128);                           /* 0.50119 * 256 = 128.3, to the nearest */
    CHECK_EQ(acc[3], -128);
    mix_master(acc, 2, mix_gain(-60));               /* the master at -6 dB as well */
    CHECK_EQ(acc[0], 526787);                        /* 1051072 * 16423 / 32768 = 526786.97 */
    CHECK_EQ(acc[1], -526787);
    mix_out32(out32, acc, 2);                        /* 24 bits kept: 2057.76 of a 16-bit step */
    CHECK_EQ(out32[0], 526787 << 8);
    CHECK_EQ(out32[1], -526787 * 256);
    mix_master(acc, 2, 0);                           /* master silent */
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(acc[i], 0);
    memset(acc, 0, sizeof(acc));
    mix_add(acc, in, 2, 0);                          /* a silent stream adds nothing */
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(acc[i], 0);
    return true;
}

/* 16-bit out: a whole value passes untouched, a fraction is dithered to
 * one of the two neighbours (rarely +-1 more) with the right mean. */
bool t_mix_dither(void)
{
    enum { N = 4096 };
    static int32_t acc[2 * N];
    static int16_t out[2 * N];
    uint32_t seed = 12345;
    for (unsigned f = 0; f < N; f++) {
        acc[2 * f] = 1000 * 256 + 64;                /* 1000.25 */
        acc[2 * f + 1] = -5 * 256;                   /* -5 exactly */
    }
    mix_out16(out, acc, N, &seed);
    int64_t sum = 0;
    for (unsigned f = 0; f < N; f++) {
        CHECK_EQ(out[2 * f + 1], -5);
        if (out[2 * f] < 999 || out[2 * f] > 1001)
            FAIL("frame %u: %d from 1000.25", f, out[2 * f]);
        sum += out[2 * f];
    }
    int64_t mean_x1000 = sum * 1000 / N;
    if (mean_x1000 < 1000200 || mean_x1000 > 1000300)
        FAIL("the dithered mean is %ld/1000, want 1000250 +-50", (long)mean_x1000);
    /* Both channels of a frame get the same dither: a mono sound stays mono. */
    for (unsigned f = 0; f < N; f++)
        acc[2 * f] = acc[2 * f + 1] = (int32_t)(f * 37 % 2000) - 1000;
    mix_out16(out, acc, N, &seed);
    for (unsigned f = 0; f < N; f++)
        CHECK_EQ(out[2 * f], out[2 * f + 1]);
    return true;
}

/* Two loud streams at once: nothing clips, the gain comes down before the
 * loud part (the lookahead) and goes back up slowly, and one stream after
 * it is untouched again once the gain is back at unity. */
bool t_mix_limits(void)
{
    enum { N = 2048 };
    static int16_t a[2 * N], b[2 * N];
    static int32_t out32[2 * N];
    for (unsigned f = 0; f < N; f++) {
        a[2 * f] = a[2 * f + 1] = (int16_t)(f % 2 ? 30000 : -30000);   /* hot */
        b[2 * f] = b[2 * f + 1] = 0;
    }
    for (unsigned f = N / 2; f < N; f++)
        b[2 * f] = b[2 * f + 1] = (int16_t)(f % 2 ? 8000 : -8000);     /* a beep on top */
    struct mix_limiter l;
    mix_limit_init(&l);
    memset(buf, 0, sizeof(buf));
    mix_add(buf + 2 * MIX_LOOKAHEAD, a, N, MIX_UNITY);
    mix_add(buf + 2 * MIX_LOOKAHEAD, b, N, MIX_UNITY);
    CHECK(mix_limit(&l, buf, N));
    mix_out32(out32, buf, N);
    int32_t prev = 0;
    for (unsigned f = 0; f < N; f++) {
        int32_t v = buf[2 * f], m = v < 0 ? -v : v;
        CHECK(v <= MIX_FULL && v >= MIX_FLOOR);
        /* input frame k = f - LOOKAHEAD: the sum 38000 from N/2 on */
        int64_t k = (int64_t)f - MIX_LOOKAHEAD;
        if (k >= 0 && k < N / 2 - (int64_t)MIX_LOOKAHEAD)
            CHECK_EQ(m, 30000 << MIX_Q);              /* untouched until the ramp starts */
        if (k >= N / 2)
            CHECK(m > (MIX_FULL - (1 << MIX_Q)) / 100 * 99);   /* at full scale, not below */
        /* The gain moves by at most 1/LOOKAHEAD a frame (2 %): |v| too,
         * but where the beep comes in. */
        int32_t d = m > prev ? m - prev : prev - m;
        if (k > 0 && k != N / 2 && d > (30000 << MIX_Q) / (int32_t)MIX_LOOKAHEAD + (1 << MIX_Q))
            FAIL("frame %u: |v| jumps from %d to %d", f, prev, m);
        prev = m;
    }
    CHECK(l.gain < 1.0f);
    /* The beep stops: the gain rises back to unity at the release rate,
     * and from then on a stream passes exactly again. */
    unsigned blocks = 0;
    while (l.gain < 1.0f && blocks++ < 10) {
        memset(buf, 0, sizeof(buf));
        mix_add(buf + 2 * MIX_LOOKAHEAD, a, N, MIX_UNITY);
        (void)mix_limit(&l, buf, N);
    }
    CHECK(blocks >= 1 && blocks <= 4);                /* from ~0.86 back to 1: ~670 frames */
    memset(buf, 0, sizeof(buf));
    mix_add(buf + 2 * MIX_LOOKAHEAD, a, N, MIX_UNITY);
    CHECK(!mix_limit(&l, buf, N));
    for (unsigned f = MIX_LOOKAHEAD; f < N; f++)
        CHECK_EQ(buf[2 * f], a[2 * (f - MIX_LOOKAHEAD)] << MIX_Q);
    CHECK_EQ(l.peak, 38000 << MIX_Q);
    return true;
}
