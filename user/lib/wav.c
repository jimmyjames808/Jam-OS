/* wav: the header of a RIFF/WAVE file (<wav.h>). Written from the format's
 * description (Microsoft's RIFF and WAVEFORMATEXTENSIBLE documentation);
 * all values are little-endian. */
#include <wav.h>

#define WAVE_PCM        1u
#define WAVE_FLOAT      3u
#define WAVE_EXTENSIBLE 0xfffeu
#define MAX_CHUNKS      64u   /* chunks looked at before giving up on finding "data" */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Exactly n bytes at offset, or ERR_OUT_OF_RANGE (the file ends first). */
static status_t read_all(wav_read_fn read, void *ctx, uint64_t offset, void *dst, size_t n)
{
    size_t got = 0;
    status_t st = read(ctx, offset, dst, n, &got);
    if (st != OK)
        return st;
    return got == n ? OK : ERR_OUT_OF_RANGE;
}

/* The "fmt " chunk's body (size bytes, at most 40 looked at) into w. */
static status_t parse_fmt(struct wav_info *w, const uint8_t *b, uint32_t size, const char **why)
{
    if (size < 16) {
        *why = "its format chunk is too short";
        return ERR_WRONG_TYPE;
    }
    uint16_t format = le16(b), channels = le16(b + 2), align = le16(b + 12), bits = le16(b + 14);
    uint32_t rate = le32(b + 4);
    uint16_t sub = format;
    if (format == WAVE_EXTENSIBLE) {
        /* cbSize 22, then valid bits, the channel mask and the subformat
         * GUID, whose first two bytes are the format code. */
        if (size < 40 || le16(b + 16) < 22) {
            *why = "its extensible format chunk is too short";
            return ERR_WRONG_TYPE;
        }
        sub = le16(b + 24);
    }
    if (sub == WAVE_FLOAT) {
        *why = "floating-point samples: only PCM (convert it to 16-bit PCM)";
        return ERR_NOT_SUPPORTED;
    }
    if (sub != WAVE_PCM) {
        *why = "compressed audio: only PCM (convert it to 16-bit PCM)";
        return ERR_NOT_SUPPORTED;
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        *why = "a sample size other than 8, 16, 24 or 32 bits";
        return ERR_NOT_SUPPORTED;
    }
    if (channels != 1 && channels != 2) {
        *why = channels ? "more than two channels: only mono and stereo" : "no channels";
        return ERR_NOT_SUPPORTED;
    }
    if (rate < 8000 || rate > 192000) {
        *why = "a rate outside 8000-192000 Hz";
        return ERR_NOT_SUPPORTED;
    }
    if (align != channels * bits / 8) {
        *why = "its frame size doesn't match its channels and sample size";
        return ERR_WRONG_TYPE;
    }
    w->format = format;
    w->rate = rate;
    w->channels = channels;
    w->bits = bits;
    w->frame_bytes = align;
    return OK;
}

status_t wav_parse(struct wav_info *w, wav_read_fn read, void *ctx, uint64_t file_size,
                   const char **why)
{
    memset(w, 0, sizeof(*w));
    *why = "";
    uint8_t b[40];
    status_t st = read_all(read, ctx, 0, b, 12);
    if (st == ERR_OUT_OF_RANGE || (st == OK && (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)))) {
        *why = "not a WAV file (no RIFF/WAVE header)";
        return ERR_WRONG_TYPE;
    }
    if (st != OK) {
        *why = "can't read it";
        return st;
    }
    bool have_fmt = false;
    uint64_t at = 12;
    for (unsigned n = 0; n < MAX_CHUNKS; n++) {
        st = read_all(read, ctx, at, b, 8);
        if (st == ERR_OUT_OF_RANGE) {
            *why = have_fmt ? "no samples (no data chunk)" : "no format chunk";
            return have_fmt ? ERR_OUT_OF_RANGE : ERR_WRONG_TYPE;
        }
        if (st != OK) {
            *why = "can't read it";
            return st;
        }
        uint32_t size = le32(b + 4);
        uint64_t body = at + 8;
        if (!memcmp(b, "fmt ", 4)) {
            uint32_t want = size < sizeof(b) ? size : sizeof(b);
            st = read_all(read, ctx, body, b, want);
            if (st == ERR_OUT_OF_RANGE) {
                *why = "cut off in its format chunk";
                return ERR_OUT_OF_RANGE;
            }
            if (st != OK) {
                *why = "can't read it";
                return st;
            }
            if ((st = parse_fmt(w, b, size, why)) != OK)
                return st;
            have_fmt = true;
        } else if (!memcmp(b, "data", 4)) {
            if (!have_fmt) {
                *why = "its samples come before its format chunk";
                return ERR_WRONG_TYPE;
            }
            uint64_t bytes = size;
            if (body > file_size || bytes > file_size - body)
                bytes = body > file_size ? 0 : file_size - body;   /* cut off: what is there */
            w->data_offset = body;
            w->frames = bytes / w->frame_bytes;
            return OK;
        }
        at = body + size + (size & 1);
    }
    *why = "no data chunk among its first chunks";
    return ERR_WRONG_TYPE;
}
