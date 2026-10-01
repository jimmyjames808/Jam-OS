/* audio: a program's sound output (libos, user/lib/audio.c).
 *
 * A program opens an output with the rate and channel count of the
 * samples it has, writes 16-bit frames (blocking), drains, and closes.
 * The library converts to what the device plays: mono to stereo, any rate
 * from AUDIO_RATE_MIN to AUDIO_RATE_MAX to AUDIO_RATE with the polyphase
 * resampler below (48000 Hz passes through untouched).
 *
 * The backend: a stream on the mixer (user/services/mixer, <mixer.h>),
 * reached through the program's SR_AUDIO startup handle (the shell gives
 * every program it runs one). The mixer plays every program's streams at
 * once through the hda driver's one output, each at its own volume
 * (`vol`); the library writes 48 kHz stereo frames into the stream's
 * ring. All of the mixer-specific code is in audio.c.
 *
 * The status values are libos's (<jam/status.h>): 0 is OK, errors are
 * negative. */
#pragma once

#include <mixer.h>
#include <os.h>

#define AUDIO_RATE      48000u   /* what the device plays */
#define AUDIO_RATE_MIN  8000u
#define AUDIO_RATE_MAX  192000u

/* The resampler: polyphase windowed sinc. Output frame o is the input
 * at time o * in_rate / out_rate (the first output frame is at the first
 * input frame's time; the position is kept exactly as a whole input frame
 * and a phase in 1/L of one, L = out_rate / gcd, so there is no drift
 * however long the stream is). Each output is a dot product of `taps`
 * input frames with a Kaiser-windowed sinc low-pass sampled at its phase:
 * passband to 0.4535 of the lower rate (20.0 kHz for 44.1 kHz input),
 * stopband from half the lower rate (22.05 kHz: the images of upsampling
 * and anything that would alias when downsampling), at least 100 dB down;
 * the ripple is under 0.001 dB. 138 taps for upsampling, more for
 * downsampling (276 from 96 kHz). The table holds L + 1 phases (at most
 * AUDIO_RS_PHASES + 1: a rate whose L is larger interpolates between
 * neighbouring phases). The arithmetic is float (SSE); each output sample
 * is rounded to 16 bits with TPDF dither unless it is a whole number
 * (digital silence stays silent). The same rate copies the frames as they
 * are (no table, bit-exact). The filter needs taps / 2 input frames past
 * an output's time: audio_rs_flush gives the last outputs (the input's
 * end followed by silence). */
#define AUDIO_RS_PHASES   1024u
#define AUDIO_RS_TAPS_MAX 1024u
struct audio_rs {
    uint32_t in_rate, out_rate;
    unsigned channels;    /* of the input: 1 or 2 */
    uint32_t L, M;        /* out_rate / g and in_rate / g, g their gcd */
    uint32_t P;           /* phases in the table (L, or AUDIO_RS_PHASES when L is larger) */
    uint32_t taps;        /* per phase: a multiple of 4 */
    float   *coef;        /* (P + 1) * taps, malloc'd; NULL: the same rate */
    float   *hist;        /* per channel 2 * taps: the last `taps` input samples, twice */
    uint64_t base;        /* the next output's input frame ... */
    uint32_t phase;       /* ... and its phase, in 1/L of a frame */
    uint64_t n_in;        /* input frames taken */
    uint32_t tail;        /* audio_rs_flush: zero frames still to feed */
    bool     flushing;
    uint32_t seed;        /* the dither's */
};

/* For in_rate frames of `channels` to out_rate stereo frames. Allocates
 * the table (ERR_NO_MEMORY) unless the rates are equal. */
int    audio_rs_init(struct audio_rs *rs, unsigned in_rate, unsigned out_rate, unsigned channels);
/* Back to the start, the table kept (after a flush, to go on). */
void   audio_rs_reset(struct audio_rs *rs);
void   audio_rs_free(struct audio_rs *rs);
/* Up to `cap` stereo output frames into out from in_frames input frames
 * (interleaved, rs->channels each); *used gets how many input frames were
 * taken (all of them unless out filled up first). Returns the output
 * frames made. Call again with the rest. */
size_t audio_rs_run(struct audio_rs *rs, const int16_t *in, size_t in_frames, size_t *used,
                    int16_t *out, size_t cap);
/* The outputs still owed for the input so far (as if silence followed):
 * up to cap of them; call until it returns 0. Then audio_rs_reset to use
 * it again. */
size_t audio_rs_flush(struct audio_rs *rs, int16_t *out, size_t cap);

/* PCM samples (not frames) of other sizes to 16-bit, as WAV files hold
 * them: 8-bit unsigned (128 is zero); 24- and 32-bit signed little-endian,
 * rounded to the nearest 16-bit value. */
void audio_s16_from_u8(int16_t *out, const uint8_t *in, size_t samples);
void audio_s16_from_s24le(int16_t *out, const uint8_t *in, size_t samples);
void audio_s16_from_s32le(int16_t *out, const uint8_t *in, size_t samples);

/* An open output. The fields are the library's own. */
struct audio_out {
    struct mixer_stream s;          /* the mixer stream */
    bool                open;
    unsigned            rate, channels;   /* of what the caller writes */
    struct audio_rs     rs;
    uint64_t            ring_min;   /* the fewest frames queued in the ring at a write once
                                     * started (UINT64_MAX: none yet) */
};

/* How an output's playing has gone (audio_stats), for `play -s`. */
struct audio_stats {
    uint32_t underruns;   /* mixer periods it ran short in (the writes fell behind) */
    uint32_t late;        /* periods the mixer itself was late for meanwhile */
    uint32_t min_lead;    /* frames: the least the mixer was ahead of the speaker
                           * (UINT32_MAX: no period yet) */
    uint32_t limited;     /* periods the mixer's limiter turned the sum down */
    uint32_t bits;        /* the device's sample size now (0: the output is closed) */
    uint64_t played;      /* frames heard (48 kHz) */
    uint64_t ring_min;    /* frames: the fewest queued in our own ring at a write once
                           * started (UINT64_MAX: none yet) */
    uint32_t ring_frames; /* its size */
};

/* Open the output for frames of `rate` Hz and `channels` (1 or 2)
 * 16-bit samples: a mixer stream, named "audio" for `vol`.
 * ERR_NOT_FOUND: no audio device (no mixer, or no hda driver with a path
 * to a jack); ERR_NO_RESOURCES: the mixer has all the streams it takes;
 * ERR_NOT_SUPPORTED: a rate or channel count outside the ranges above;
 * ERR_NO_MEMORY: no room for the resampler's table.
 * Nothing plays until the stream's ring is full or audio_drain. */
int  audio_open(struct audio_out *a, unsigned rate, unsigned channels);
/* The same, with the stream named `name` for `vol` (15 characters). */
int  audio_open_as(struct audio_out *a, unsigned rate, unsigned channels, const char *name);
/* Write nframes interleaved frames, blocking while the stream's ring is
 * full (until the mixer takes a period, 43 ms). Returns nframes, or a
 * negative status (ERR_PEER_CLOSED: the mixer died, so the stream is gone:
 * close and open again; ERR_TIMED_OUT: nothing was taken for 5 s). Write a
 * few thousand frames at a time to stay responsive (Ctrl+C) and far
 * enough ahead: if the writes fall behind, the mixer plays silence for
 * this stream meanwhile and counts an underrun (`vol`). */
long audio_write(struct audio_out *a, const void *frames, size_t nframes);
/* Wait until everything written has played, and then a period of
 * silence more (85 ms, so whatever records or plays the device's output
 * gets past the sound's end). The output can be written again
 * afterwards. */
int  audio_drain(struct audio_out *a);
/* From now on the caller writes frames of `rate` Hz and `channels`
 * instead (a player's next file): what the old resampler still owes is
 * written first, so the two meet without a gap. The same rate and
 * channels: nothing changes (the resampler carries on). ERR_NOT_SUPPORTED:
 * out of range (the output stays as it was). */
int  audio_set_input(struct audio_out *a, unsigned rate, unsigned channels);
/* Drop what was written and the mixer has not taken yet, with the 5 ms
 * fade (as audio_close), and keep the output open for what comes next (a
 * player skipping a track). Returns once the fade has been heard. */
int  audio_discard(struct audio_out *a);
/* The output's volume in centibels (tenths of a dB, 0 at most):
 * audio_set_volume sets this stream's own (in the mixer: -96 dB and below
 * is silence; other programs are not affected); audio_get_volume answers
 * the level it is heard at: its own volume, the mixer's master volume and
 * the device's gain (`hda gain`) added up (audio.idl's stream_levels). */
int  audio_set_volume(struct audio_out *a, int centibels);
int  audio_get_volume(struct audio_out *a, int *centibels);
/* How it has gone so far (the mixer's audio.stream_stats, and ours). */
int  audio_stats(struct audio_out *a, struct audio_stats *st);
/* Stop and close. What was written and the mixer has not taken yet is
 * dropped with a 5 ms fade (no click); what it took plays out (its lead,
 * 171 ms at most). audio_drain first to hear it all. */
void audio_close(struct audio_out *a);
