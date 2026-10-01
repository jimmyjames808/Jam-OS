/* The mixer's arithmetic (user/lib/mixmath.c), apart from the service so
 * utest can check it: volumes in centibels turned into Q15 gains, frames
 * scaled and summed into 32-bit accumulators, and the sum scaled by the
 * master gain and saturated to 16 bits.
 *
 * A gain is Q15: 32768 is 0 dB (the samples unchanged), 16423 is -6 dB,
 * 0 is silence. A frame is two s16 samples (left, right); the
 * accumulators hold two int32 per frame. Each scaled sample is rounded to
 * the nearest (halves up), so at 0 dB a sample passes through exactly,
 * and the sum of up to MIX_MAX_INPUTS full-scale samples can't overflow
 * an int32. No floating point here except mix_gain, which runs once per
 * volume change. */
#pragma once

#include <stdint.h>

#define MIX_UNITY      32768u   /* Q15 gain of 0 dB */
#define MIX_VOLUME_MAX 0        /* centibels: no boost */
#define MIX_VOLUME_MIN (-960)   /* centibels: -96 dB; this and below is silence */
#define MIX_MAX_INPUTS 64       /* streams summed into one accumulator at most */

/* cb clamped to [MIX_VOLUME_MIN, MIX_VOLUME_MAX]. */
int32_t  mix_clamp_volume(int32_t cb);
/* The Q15 gain of a volume in centibels (clamped first): 10^(cb / 200),
 * rounded; 0 at MIX_VOLUME_MIN. */
uint32_t mix_gain(int32_t cb);
/* acc[0 .. 2 * frames) += in scaled by gain (Q15, at most MIX_UNITY). */
void     mix_add(int32_t *acc, const int16_t *in, uint32_t frames, uint32_t gain);
/* out[0 .. 2 * frames) = acc scaled by master (Q15, at most MIX_UNITY),
 * saturated to [-32768, 32767]. */
void     mix_out(int16_t *out, const int32_t *acc, uint32_t frames, uint32_t master);
