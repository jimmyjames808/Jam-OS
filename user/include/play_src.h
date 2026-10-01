/* play_src: a sound file as 16-bit frames, whatever the file holds
 * (libos, user/lib/play_src.c): what the shell's `play` and the music
 * player (bin/music) read their samples from. A caller opens a source on
 * a file, prints its description, and reads 16-bit frames from it until it
 * ends; the source reads the file a chunk at a time (never whole) and
 * turns its samples into 16-bit frames of `channels` samples at `rate` Hz,
 * which <audio.h> then makes 48 kHz stereo. */
#pragma once

#include <os.h>

struct play_src_ops;

struct play_src {
    uint32_t rate;          /* Hz */
    uint16_t channels;      /* 1 or 2 */
    uint64_t frames;        /* how many it holds (0: unknown) */
    char     desc[64];      /* for play's first line: "48000 Hz, 16-bit, 2 ch, 0:02" */
    /* The source's own. */
    const struct play_src_ops *ops;
    struct jfile *f;
    void         *state;
};

/* Open the file f of `size` bytes as a source: chosen by what the file
 * starts with, not its name. OK; or an error with *why saying what, as a
 * short phrase ("not a WAV file (no RIFF/WAVE header)"). */
status_t play_src_open(struct play_src *s, struct jfile *f, uint64_t size, const char **why);
/* Up to `frames` frames into out (room for frames * channels samples).
 * Returns how many (0: the end), or a negative status. */
long play_src_read(struct play_src *s, int16_t *out, size_t frames);
void play_src_close(struct play_src *s);

/* "m:ss" of frames at rate, rounded to the nearest second. */
const char *play_mss(uint64_t frames, uint32_t rate, char *buf, size_t size);
