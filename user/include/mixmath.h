/* The mixer's arithmetic (user/lib/mixmath.c), apart from the service so
 * utest can check it: volumes in centibels turned into Q15 gains, frames
 * scaled and summed into 32-bit accumulators with 8 bits below the
 * 16-bit LSB, the master gain, a peak limiter, and the sum turned into
 * what the device plays: 32-bit samples (24 significant bits, for a DAC
 * of 20, 24 or 32 bits) or 16-bit ones, dithered where a fraction is
 * dropped.
 *
 * A gain is Q15: 32768 is 0 dB (the samples unchanged), 16423 is -6 dB,
 * 0 is silence. A frame is two s16 samples (left, right); the
 * accumulators hold two int32 per frame in "Q8": a 16-bit sample s is
 * s * 256. Each scaled sample is rounded to the nearest Q8 step (halves
 * up), so at 0 dB a sample passes through exactly, and the sum of up to
 * MIX_MAX_INPUTS full-scale samples can't overflow an int32. Volume
 * changes keep 8 bits below the 16-bit LSB (24-bit resolution), all of
 * which a 32-bit output keeps; a 16-bit output rounds them away with TPDF
 * dither (two uniform random values, +-1 LSB, triangular), but only on
 * samples that have a fraction: a sample that is a whole 16-bit value
 * (every sample at 0 dB, and digital silence) passes untouched.
 *
 * The limiter: when the sum of the streams would go past full scale
 * (two loud streams at once: music near 0 dBFS and a beep), the gain
 * comes down smoothly instead of the samples clipping: the gain needed
 * for each frame, ramped down over MIX_LOOKAHEAD frames before the loud
 * one (the limiter delays the output by that much: 1 ms) and back up at
 * MIX_RELEASE_FRAMES per unit of gain (100 ms from silence to unity).
 * While the sum stays at or below full scale and the gain is back at
 * unity it does nothing at all: one stream at 0 dB is bit-exact, only
 * delayed. No floating point is used on a sample that isn't limited. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MIX_UNITY      32768u   /* Q15 gain of 0 dB */
#define MIX_VOLUME_MAX 0        /* centibels: no boost */
#define MIX_VOLUME_MIN (-960)   /* centibels: -96 dB; this and below is silence */
#define MIX_MAX_INPUTS 64       /* streams summed into one accumulator at most */
#define MIX_Q          8        /* bits below the 16-bit LSB in an accumulator */
#define MIX_FULL       ((int32_t)32767 << MIX_Q)    /* full scale, Q8: the most ... */
#define MIX_FLOOR      (-((int32_t)32768 << MIX_Q)) /* ... and the least */
#define MIX_BLOCK_MAX  4096u    /* frames mix_limit takes at a time, at most */
#define MIX_LOOKAHEAD  48u      /* frames: the limiter's delay and attack (1 ms) */
#define MIX_RELEASE_FRAMES 4800u   /* frames for the gain to rise by 1 (100 ms) */

/* cb clamped to [MIX_VOLUME_MIN, MIX_VOLUME_MAX]. */
int32_t  mix_clamp_volume(int32_t cb);
/* The Q15 gain of a volume in centibels (clamped first): 10^(cb / 200),
 * rounded; 0 at MIX_VOLUME_MIN. */
uint32_t mix_gain(int32_t cb);
/* acc[0 .. 2 * frames) += in scaled by gain (Q15, at most MIX_UNITY), in
 * Q8. */
void     mix_add(int32_t *acc, const int16_t *in, uint32_t frames, uint32_t gain);
/* acc[0 .. 2 * frames) scaled by master (Q15, at most MIX_UNITY), in
 * place, rounded to Q8. Does nothing at MIX_UNITY. */
void     mix_master(int32_t *acc, uint32_t frames, uint32_t master);

/* The limiter's state between blocks: the MIX_LOOKAHEAD frames it holds
 * back, the gain it ended the last block at, and counts for the log. */
struct mix_limiter {
    int32_t  held[2 * MIX_LOOKAHEAD];
    float    gain;               /* 1: not limiting */
    uint64_t limited;            /* frames played below unity gain */
    int32_t  peak;               /* the largest |sample| it was given, Q8 */
};
/* A fresh limiter: unity, nothing held (MIX_LOOKAHEAD frames of
 * silence come out first). */
void     mix_limit_init(struct mix_limiter *l);
/* Limit one block. buf holds MIX_LOOKAHEAD + frames frames: the caller
 * mixes the new block into buf + 2 * MIX_LOOKAHEAD; the limiter puts its
 * held frames in front, and afterwards buf[0 .. 2 * frames) is the block
 * to play (every sample in [MIX_FLOOR, MIX_FULL]) and the last MIX_LOOKAHEAD frames
 * are held for the next call. Returns true if any frame of the block
 * was played below unity gain. */
bool     mix_limit(struct mix_limiter *l, int32_t *buf, uint32_t frames);

/* The block as the device's samples: 32-bit, the Q8 value in the top 24
 * bits (exactly the 16-bit sample << 16 when there is no fraction),
 * clamped to [MIX_FLOOR, MIX_FULL] first. */
void     mix_out32(int32_t *out, const int32_t *acc, uint32_t frames);
/* As 16-bit samples, rounded to nearest; a sample with a fraction gets
 * TPDF dither first (one random value per frame, the same for both
 * channels, so a mono sound stays mono). *seed: the generator's state
 * (any value but 0). */
void     mix_out16(int16_t *out, const int32_t *acc, uint32_t frames, uint32_t *seed);
