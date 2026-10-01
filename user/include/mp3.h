/* mp3: MPEG audio files, decoded (libos, user/lib/mp3.c), for `play`.
 *
 * The decoder is dr_mp3 (third_party/dr_mp3, public domain or MIT-0, a
 * fork of minimp3): MPEG-1, MPEG-2 and MPEG-2.5, Layers I, II and III,
 * CBR, VBR and free format, mono and stereo, 8000 to 48000 Hz, into
 * 16-bit samples. It skips ID3v2 tags at the start, ID3v1 and APE tags at
 * the end, a Xing/Info/LAME frame (and the encoder's delay and padding it
 * names), and frames it can't decode (it looks for the next one). The
 * file is read through a callback, 64 KiB at a time, never whole.
 *
 * mp3_sniff is pure apart from its read callback and decides whether a
 * file is MPEG audio at all, so a file that isn't is refused before the
 * decoder searches it end to end for a frame. */
#pragma once

#include <os.h>

/* Read up to n bytes at offset into dst; *got gets how many (short only at
 * the end of the file). The same shape as <wav.h>'s wav_read_fn. */
typedef status_t (*mp3_read_fn)(void *ctx, uint64_t offset, void *dst, size_t n, size_t *got);

/* One MPEG audio frame header (the 4 bytes every frame starts with). */
struct mp3_header {
    uint8_t  version;   /* 10: MPEG-1, 20: MPEG-2, 25: MPEG-2.5 */
    uint8_t  layer;     /* 1, 2 or 3 */
    uint8_t  channels;  /* 1 or 2 */
    bool     crc;       /* a 16-bit CRC follows the header */
    uint32_t rate;      /* Hz */
    uint32_t kbps;      /* 0: free format (the frame's length isn't in its header) */
    uint32_t samples;   /* per channel in the frame: 384, 576 or 1152 */
    uint32_t bytes;     /* the frame's length with its header; 0 for free format */
};

/* The 4 bytes at p as a frame header: false if they are not one (no sync,
 * a reserved version, layer, bitrate or rate). */
bool mp3_header_parse(const uint8_t *p, struct mp3_header *h);

/* What mp3_sniff found. */
struct mp3_sniffed {
    struct mp3_header first;   /* the first frame's header */
    uint64_t offset;           /* where that frame starts (after any ID3v2 tags) */
    bool     vbri;             /* it is a Fraunhofer VBRI frame (a VBR file) */
    uint32_t vbri_frames;      /* the frames VBRI counts (0: none) */
    uint64_t end;              /* where the audio ends: before an ID3v1 tag
                                * ("TAG", the last 128 bytes) and an APEv2
                                * tag (its "APETAGEX" footer) before that */
};

/* Is the file of file_size bytes MPEG audio? It skips ID3v2 tags at the
 * start and ID3v1/APEv2 tags at the end, then looks in the first 8 KiB
 * left for a frame header followed by a second one where the first frame
 * ends (same version, layer and rate), or by the end of the audio. OK and
 * *s filled in; ERR_WRONG_TYPE: no such frame; or the callback's error. */
status_t mp3_sniff(mp3_read_fn read, void *ctx, uint64_t file_size, struct mp3_sniffed *s);

/* An open file being decoded. The fields are the library's own except
 * these, which mp3_open fills in. */
struct mp3_info {
    uint32_t rate;         /* Hz, of the frames mp3_decode gives */
    uint16_t channels;     /* 1 or 2 */
    uint8_t  layer;        /* 1, 2 or 3 */
    uint8_t  version;      /* 10, 20 or 25, as in mp3_header */
    uint32_t kbps;         /* the first audio frame's bitrate; 0: free format */
    bool     vbr;          /* a Xing or VBRI header says the bitrate varies */
    uint64_t frames;       /* how many frames it holds: exact from a Xing/Info
                            * header, else estimated from the size and the
                            * bitrate; 0: unknown (free format, no header) */
    bool     exact;        /* frames came from a header */
};

struct mp3 {
    struct mp3_info info;
    void           *dec;       /* dr_mp3's decoder (struct drmp3, ~33 KiB, and its 64 KiB buffer) */
    mp3_read_fn     read;
    void           *ctx;
    uint64_t        size, pos; /* where the audio ends (the file as dr_mp3
                                * sees it), and dr_mp3's cursor in it */
    status_t        err;       /* a read error the decoder ran into */
};

/* Sniff and open the file of file_size bytes for decoding. OK; or
 * ERR_WRONG_TYPE (not MPEG audio: *why "no MPEG audio frames"),
 * ERR_NOT_SUPPORTED (the decoder found no frame it can decode),
 * ERR_NO_MEMORY, or the callback's error. */
status_t mp3_open(struct mp3 *m, mp3_read_fn read, void *ctx, uint64_t file_size,
                  const char **why);
/* Up to `frames` interleaved 16-bit frames (info.channels samples each)
 * into out. Returns how many (0: the end of the audio: the file's end,
 * the length its Xing/Info header gives, or a cut-off last frame), or a
 * negative status (a read error). Frames it can't decode are skipped. */
long mp3_decode(struct mp3 *m, int16_t *out, size_t frames);
void mp3_close(struct mp3 *m);
