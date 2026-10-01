/* audio: a program's sound output (libos, user/lib/audio.c).
 *
 * A program opens an output with the rate and channel count of the
 * samples it has, writes 16-bit frames (blocking), drains, and closes.
 * The library converts to what the device plays: mono to stereo, any rate
 * from AUDIO_RATE_MIN to AUDIO_RATE_MAX to AUDIO_RATE with the linear
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

/* The resampler: linear interpolation between two input frames. The
 * position is kept exactly, as a count of 1/out_rate input frames, so
 * there is no drift however long the stream is. Pure: no handles, no
 * allocation. The same rate copies the frames as they are. The first
 * output frame is the first input frame; output frame o is input frame
 * o * in_rate / out_rate, interpolated and rounded to nearest, and the
 * output has (in_frames - 1) * out_rate / in_rate + 1 frames. Linear
 * interpolation is clean for upsampling (44100 -> 48000); downsampling
 * (96000 -> 48000) has no low-pass filter, so content above 24 kHz
 * aliases (inaudible in ordinary recordings). */
struct audio_rs {
    uint32_t in_rate, out_rate;
    uint64_t pos;         /* the next output frame's position past `prev`, in 1/out_rate */
    int16_t  prev[2];     /* the last input frame taken (stereo) */
    bool     primed;      /* prev holds a frame */
    unsigned channels;    /* of the input: 1 or 2 */
};

void   audio_rs_init(struct audio_rs *rs, unsigned in_rate, unsigned out_rate, unsigned channels);
/* Up to `cap` stereo output frames into out from in_frames input frames
 * (interleaved, rs->channels each); *used gets how many input frames were
 * taken (all of them unless out filled up first). Returns the output
 * frames made. Call again with the rest. */
size_t audio_rs_run(struct audio_rs *rs, const int16_t *in, size_t in_frames, size_t *used,
                    int16_t *out, size_t cap);

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
};

/* Open the output for frames of `rate` Hz and `channels` (1 or 2)
 * 16-bit samples: a mixer stream, named "audio" for `vol`.
 * ERR_NOT_FOUND: no audio device (no mixer, or no hda driver with a path
 * to a jack); ERR_NO_RESOURCES: the mixer has all the streams it takes;
 * ERR_NOT_SUPPORTED: a rate or channel count outside the ranges above.
 * Nothing plays until the stream's ring is full or audio_drain. */
int  audio_open(struct audio_out *a, unsigned rate, unsigned channels);
/* The same, with the stream named `name` for `vol` (15 characters). */
int  audio_open_as(struct audio_out *a, unsigned rate, unsigned channels, const char *name);
/* Write nframes interleaved frames, blocking while the stream's ring is
 * full (until the mixer takes a period, 85 ms). Returns nframes, or a
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
/* The output's volume in centibels (tenths of a dB, 0 at most):
 * audio_set_volume sets this stream's own (in the mixer: -96 dB and below
 * is silence; other programs are not affected); audio_get_volume answers
 * the level it is heard at: its own volume, the mixer's master volume and
 * the device's gain (`hda gain`) added up (audio.idl's stream_levels). */
int  audio_set_volume(struct audio_out *a, int centibels);
int  audio_get_volume(struct audio_out *a, int *centibels);
/* Stop and close. What was written and the mixer has not taken yet is
 * dropped with a 5 ms fade (no click); what it took plays out (its lead,
 * 170 ms at most). audio_drain first to hear it all. */
void audio_close(struct audio_out *a);
