/* wav: the header of a RIFF/WAVE file (libos, user/lib/wav.c), for `play`.
 *
 * Reads the chunks from the start of the file through a read callback (so
 * the file is never loaded whole): "RIFF" <size> "WAVE", then chunks of
 * <id> <u32 size> <bytes> (padded to an even size), skipping every chunk
 * but "fmt " and "data". Accepted: PCM (format 1) and
 * WAVE_FORMAT_EXTENSIBLE (0xFFFE) with the PCM subformat; 8-bit unsigned,
 * 16-, 24- and 32-bit signed samples; 1 or 2 channels; 8000 to 192000 Hz.
 * A data chunk longer than the file (a cut-off download) is played as far
 * as the file goes. Pure apart from the callback. */
#pragma once

#include <os.h>

struct wav_info {
    uint32_t rate;          /* frames per second */
    uint16_t channels;      /* 1 or 2 */
    uint16_t bits;          /* 8, 16, 24 or 32 */
    uint16_t frame_bytes;   /* channels * bits / 8 */
    uint16_t format;        /* 1, or 0xFFFE (extensible) */
    uint64_t data_offset;   /* where the samples start in the file */
    uint64_t frames;        /* how many whole frames the file holds */
};

/* Read up to n bytes at offset into dst; *got gets how many (short only at
 * the end of the file). */
typedef status_t (*wav_read_fn)(void *ctx, uint64_t offset, void *dst, size_t n, size_t *got);

/* Parse the file of file_size bytes. OK, or: ERR_WRONG_TYPE (not a WAV
 * file), ERR_NOT_SUPPORTED (a WAV file this can't play), ERR_OUT_OF_RANGE
 * (cut off before its samples), or the callback's error; *why then says
 * what, as a short phrase ("8 channels: only mono and stereo"). */
status_t wav_parse(struct wav_info *w, wav_read_fn read, void *ctx, uint64_t file_size,
                   const char **why);
