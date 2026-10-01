/* audio: a program's sound output (libos, user/lib/audio.c).
 *
 * A program opens an output with the rate and channel count of the
 * samples it has, writes 16-bit frames (blocking), drains, and closes.
 * The library converts to what the device plays: mono to stereo, any rate
 * from AUDIO_RATE_MIN to AUDIO_RATE_MAX to AUDIO_RATE with the linear
 * resampler below (48000 Hz passes through untouched).
 *
 * The backend, today: the hda driver's one output stream (abi/idl/hda.idl:
 * a 64 KiB ring of 48 kHz 16-bit stereo, 4 periods of 85 ms), found
 * through devmgr, written ahead of the play position. So one program
 * plays at a time; a second audio_open is ERR_BAD_STATE. A2's mixer
 * (docs/A2-PLAN.md) replaces the backend behind these same calls; all
 * hda-specific code is in audio.c.
 *
 * The status values are libos's (<jam/status.h>): 0 is OK, errors are
 * negative. */
#pragma once

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
    handle_t        dev;            /* the hda driver's channel */
    handle_t        stream;         /* the stream channel open_output gave */
    handle_t        vmo;            /* the ring */
    int16_t        *ring;           /* ... mapped */
    uint32_t        ring_bytes;
    uint32_t        ring_frames, period_frames;
    uint64_t        written;        /* the next device frame to write */
    uint64_t        played;         /* the device's position as last told */
    bool            started, drained;
    bool            gain_set;       /* audio_set_volume changed the device's gain */
    int32_t         gain_before;    /* ... from this, put back at close */
    unsigned        rate, channels; /* of what the caller writes */
    uint32_t        underruns;      /* times the writes fell behind the device */
    struct audio_rs rs;
};

/* Open the output for frames of `rate` Hz and `channels` (1 or 2)
 * 16-bit samples. ERR_NOT_FOUND: no audio device (no hda driver with a
 * path to a jack); ERR_BAD_STATE: another program is playing;
 * ERR_NOT_SUPPORTED: a rate or channel count outside the ranges above.
 * Nothing plays until the ring is full or audio_drain. */
int  audio_open(struct audio_out *a, unsigned rate, unsigned channels);
/* Write nframes interleaved frames, blocking while the device's ring is
 * full (at most a period, 85 ms, per wait). Returns nframes, or a negative
 * status (ERR_PEER_CLOSED: the driver died; ERR_TIMED_OUT: it stalled).
 * Write a few thousand frames at a time to stay responsive (Ctrl+C) and
 * far enough ahead: if the writes fall behind the device it plays silence
 * meanwhile and `underruns` counts it. */
long audio_write(struct audio_out *a, const void *frames, size_t nframes);
/* Wait until everything written has played, and then one period of
 * silence more (the device's own buffers empty). The output can be
 * written again afterwards. */
int  audio_drain(struct audio_out *a);
/* The output's volume in centibels (tenths of a dB, 0 at most). Today:
 * the device's gain (hda set_gain, rounded to its amp's step), put back
 * as it was at audio_close; with the mixer it becomes the stream's own.
 * audio_get_volume answers what is set now. */
int  audio_set_volume(struct audio_out *a, int centibels);
int  audio_get_volume(struct audio_out *a, int *centibels);
/* Stop and close. What was written and has not played yet is dropped
 * with a 5 ms fade (no click): audio_drain first to hear it all. */
void audio_close(struct audio_out *a);

/* The channel to devmgr the library finds the device through: libos's
 * default is the SR_DEVMGR startup handle; a program that gets devmgr
 * some other way (the shell: init sends it each new one) defines its own
 * audio_devmgr. Not closed by the library. */
handle_t audio_devmgr(void);
