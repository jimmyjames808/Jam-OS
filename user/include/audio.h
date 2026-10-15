/* audio: a program's sound output (libos, user/lib/audio.c).
 *
 * A program opens an output with the rate, channel count and sample
 * format of what it has (16-bit or float), writes frames, drains, and
 * closes. Two ways to write: audio_write blocks while the ring is full (a
 * player that reads a file as fast as the device takes it), audio_try_write
 * never does (a program that is asked for sound on a clock, as a browser's
 * audio back end is: it asks audio_position how far ahead it is, writes
 * what it needs, and waits for room with audio_wait_room or its own timer).
 * audio_start and audio_pause give it the stream's clock.
 * The library converts to what the device plays: mono to stereo, float to
 * 16 bits, any rate from AUDIO_RATE_MIN to AUDIO_RATE_MAX to AUDIO_RATE
 * with the polyphase resampler below (48000 Hz passes through untouched).
 *
 * One thread uses an output at a time (as with any libos object that has
 * state of its own); audio_room_event's handle may be waited on from
 * anywhere.
 *
 * The backend: a stream on the mixer (user/services/mixer, <mixer.h>),
 * reached through the service /svc/audio (<os.h> SVC_AUDIO: a program
 * whose list asks for `svc audio` has it). The mixer plays every program's streams at
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
/* Float samples (1.0 is full scale: x * 32768) to 16-bit: rounded to the
 * nearest, with TPDF dither (as the resampler's) unless x * 32768 is a
 * whole number, so a float made from 16-bit samples comes back exactly and
 * digital silence stays silent. Beyond [-1, 1) the value is clipped; a NaN
 * is 0. *seed: the dither's generator, any nonzero start; keep it between
 * calls. */
void audio_s16_from_f32(int16_t *out, const float *in, size_t samples, uint32_t *seed);

/* What the caller's samples are. */
enum audio_format {
    AUDIO_S16 = 0,   /* int16_t, the channels of a frame side by side */
    AUDIO_F32 = 1,   /* float, the same way */
};

/* An open output. The fields are the library's own. */
struct audio_out {
    struct mixer_stream s;          /* the mixer stream */
    bool                open;
    unsigned            rate, channels;   /* of what the caller writes */
    enum audio_format   fmt;
    bool                held;       /* audio_pause was the last word: a full ring is not
                                     * started by a write */
    bool                armed;      /* audio_arm_room asked the mixer for a wake-up */
    struct audio_rs     rs;
    uint32_t            seed;       /* the float conversion's dither */
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
/* The same, for samples of the format `fmt` (ERR_NOT_SUPPORTED for one
 * that is not in the enum). The two above take AUDIO_S16. The mixer takes
 * at most 4 streams from a program (and 16 in all): audio_close gives the
 * slot back. */
int  audio_open_fmt(struct audio_out *a, unsigned rate, unsigned channels,
                    enum audio_format fmt, const char *name);
/* Write nframes interleaved frames, blocking while the stream's ring is
 * full (until the mixer takes a period, 43 ms). Returns nframes, or a
 * negative status (ERR_PEER_CLOSED: the mixer died, so the stream is gone:
 * close and open again; ERR_TIMED_OUT: nothing was taken for 5 s). Write a
 * few thousand frames at a time to stay responsive (Ctrl+C) and far
 * enough ahead: if the writes fall behind, the mixer plays silence for
 * this stream meanwhile and counts an underrun (`vol`). A full ring on a
 * stream that is not started is started (it would never empty), unless
 * audio_pause stopped it: then the write waits for audio_start (from
 * another thread) and times out. */
long audio_write(struct audio_out *a, const void *frames, size_t nframes);
/* The same without waiting: writes as many of the frames as the ring has
 * room for and returns how many (0: it is full; the caller waits for room
 * with audio_wait_room, or until its own timer, and writes the rest). A
 * negative status as audio_write's. Never blocks, except for the call
 * that starts a full ring which isn't started (and isn't paused). */
long audio_try_write(struct audio_out *a, const void *frames, size_t nframes);
/* The frames audio_try_write is sure to take now: none if the ring is full,
 * otherwise a little under what it has room for after the conversion to
 * 48 kHz. Never a call into the mixer. */
size_t audio_room(const struct audio_out *a);
/* The most audio_room can be: the empty ring's. (65536 device frames, 1.37 s,
 * at the time of writing; a stream asks the mixer for its size.) */
size_t audio_room_max(const struct audio_out *a);
/* Frames (48 kHz, the device's) written and not yet taken by the mixer.
 * Never a call. The mixer holds a `lead` more (audio_position) that
 * it has taken and not yet played. */
uint64_t audio_queued(const struct audio_out *a);
/* Wait until audio_room is at least min_frames (1 to audio_room_max) or
 * the absolute deadline (now() + ns) passes. OK, ERR_TIMED_OUT,
 * ERR_PEER_CLOSED (the mixer is gone), ERR_INVALID_ARGS. Starts a ring too
 * full for min_frames and not started, unless paused (then it waits for
 * audio_start). The wait is in slices of 100 ms, so a dead mixer ends it. */
int  audio_wait_room(struct audio_out *a, size_t min_frames, uint64_t deadline);
/* For a thread that waits for room on something else as well (its stop
 * request): audio_arm_room asks the mixer to raise MIXER_SIG_SPACE on
 * audio_room_event's handle when it has taken frames, then looks. OK: there
 * is room for min_frames already. ERR_SHOULD_WAIT: wait on the handle for
 * MIXER_SIG_SPACE (jam_object_wait_one, or with the other handles),
 * and arm again after it wakes (it may wake with too little room). The
 * ask lasts until the next arm, audio_wait_room or write. Same
 * arguments and starting rule as audio_wait_room. */
int  audio_arm_room(struct audio_out *a, size_t min_frames);
handle_t audio_room_event(const struct audio_out *a);
/* Wait until everything written has played, and then a period of
 * silence more (85 ms, so whatever records or plays the device's output
 * gets past the sound's end). The output can be written again
 * afterwards. Starts a stream that was paused. */
int  audio_drain(struct audio_out *a);
/* Let the mixer take frames now, though the ring is not full: for a
 * stream that has written what it wants heard first (a browser's pre-roll)
 * and for one audio_pause stopped. A started stream whose ring runs dry
 * plays silence and counts underruns (`vol`), and the mixer closes the
 * device after a second of it (the next write wakes it). OK if started
 * already. */
int  audio_start(struct audio_out *a);
/* The mixer takes no more frames from this stream; what is in the ring
 * waits for audio_start (or audio_discard). Writes still go to the ring
 * (it holds 1.37 s). What the mixer had already taken ahead of the
 * speaker (the stream's lead, 171 ms at most) is played out if another
 * stream keeps the device open, and dropped if this was the only one (the
 * mixer closes the device at once): a pause then is a stop that resumes
 * at the next frame the mixer had not taken, skipping up to the lead
 * (audio_position's `played` at the pause says where the sound stopped).
 * A program that pauses to throw its sound away calls audio_discard next.
 * OK if stopped already. A drain in progress on another thread:
 * ERR_BAD_STATE. */
int  audio_pause(struct audio_out *a);
/* Where the playing is. */
struct audio_pos {
    uint64_t written;      /* frames put in the ring so far (48 kHz; audio_discard takes
                            * back the ones it drops) */
    uint64_t played;       /* frames heard (48 kHz, estimated from the driver's play
                            * position: moves smoothly within a period) */
    uint64_t latency_ns;   /* how long after `at` a frame written then would be heard: the
                            * ring ahead of it, the resampler's delay and nothing else;
                            * meaningful while the stream is started (before that it is
                            * the time from the start) */
    uint64_t at;           /* now() when the mixer was asked */
    uint32_t lead;         /* frames: the mixer takes this far ahead of what is heard, so
                            * the least latency_ns an empty ring has */
};
/* Ask the mixer for the stream's position (a call to it: tens of
 * microseconds, once a buffer is fine). */
int  audio_position(struct audio_out *a, struct audio_pos *pos);
/* From now on the caller writes frames of `rate` Hz and `channels`
 * instead (a player's next file): what the old resampler still owes is
 * written first, so the two meet without a gap. The same rate and
 * channels: nothing changes (the resampler carries on). ERR_NOT_SUPPORTED:
 * out of range (the output stays as it was). */
int  audio_set_input(struct audio_out *a, unsigned rate, unsigned channels);
/* Drop what was written and the mixer has not taken yet, with the 5 ms
 * fade (as audio_close), and keep the output open for what comes next (a
 * player skipping a track). Returns once the fade has been heard. On a
 * stream that is not started (paused, or never started) the mixer is
 * hearing nothing of the ring, so it is emptied at once, with no fade. */
int  audio_discard(struct audio_out *a);
/* The output's volume in centibels (tenths of a dB, 0 at most):
 * audio_set_volume sets this stream's own (in the mixer: -96 dB and below
 * is silence; other programs are not affected); audio_get_volume answers
 * the level it is heard at: its own volume, the mixer's master volume and
 * the device's gain (`hda gain`) added up (audio.idl's stream_levels). */
int  audio_set_volume(struct audio_out *a, int centibels);
/* A volume as typed ("-20", "-20.5", "+3", "0": dB, one decimal kept) into
 * centibels; false if it isn't one, or is more than 100 dB either way. */
bool audio_parse_db(const char *s, int *centibels);
int  audio_get_volume(struct audio_out *a, int *centibels);
/* How it has gone so far (the mixer's audio.stream_stats, and ours). */
int  audio_stats(struct audio_out *a, struct audio_stats *st);
/* Stop and close. What was written and the mixer has not taken yet is
 * dropped with a 5 ms fade (no click); what it took plays out (its lead,
 * 171 ms at most). audio_drain first to hear it all. */
void audio_close(struct audio_out *a);
