/* mp3: MPEG audio files (<mp3.h>). The frame header is written from the
 * format's description (ISO/IEC 11172-3 and 13818-3, and the MPEG-2.5
 * extension); the decoding is dr_mp3's (third_party/dr_mp3, compiled in
 * user/lib/mp3port/dr_mp3_impl.c). */
#include <mp3.h>

#include "dr_mp3.h"

#define SNIFF_BYTES 8192u   /* looked through for the first frame after the ID3v2 tags */
#define MAX_ID3     8u      /* ID3v2 tags skipped one after another at most */

/* kbps by [MPEG-1?][layer - 1][index]; index 0 is free format, 15 is bad. */
static const uint16_t kbps_table[2][3][15] = {
    { /* MPEG-2 and 2.5 */
      { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
      { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
      { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } },
    { /* MPEG-1 */
      { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
      { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
      { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
};
static const uint32_t rate_table[3] = { 44100, 48000, 32000 };   /* MPEG-1's */

bool mp3_header_parse(const uint8_t *p, struct mp3_header *h)
{
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0)
        return false;
    unsigned ver = (p[1] >> 3) & 3, lay = (p[1] >> 1) & 3;
    unsigned bri = p[2] >> 4, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || lay == 0 || bri == 15 || sri == 3)
        return false;
    bool mpeg1 = ver == 3;
    h->version = mpeg1 ? 10 : ver == 2 ? 20 : 25;
    h->layer = (uint8_t)(4 - lay);
    h->channels = (p[3] >> 6) == 3 ? 1 : 2;
    h->crc = !(p[1] & 1);
    h->rate = rate_table[sri] >> (mpeg1 ? 0 : ver == 2 ? 1 : 2);
    h->kbps = kbps_table[mpeg1][h->layer - 1][bri];
    h->samples = h->layer == 1 ? 384 : h->layer == 2 || mpeg1 ? 1152 : 576;
    if (!h->kbps)
        h->bytes = 0;
    else if (h->layer == 1)
        h->bytes = (12 * h->kbps * 1000 / h->rate + pad) * 4;
    else
        h->bytes = h->samples / 8 * h->kbps * 1000 / h->rate + pad;
    return true;
}

static bool same_stream(const struct mp3_header *a, const struct mp3_header *b)
{
    return a->version == b->version && a->layer == b->layer && a->rate == b->rate &&
           !a->kbps == !b->kbps;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

status_t mp3_sniff(mp3_read_fn read, void *ctx, uint64_t file_size, struct mp3_sniffed *s)
{
    uint8_t *buf = malloc(SNIFF_BYTES);
    if (!buf)
        return ERR_NO_MEMORY;
    memset(s, 0, sizeof(*s));
    uint64_t off = 0;
    size_t n = 0;
    status_t st = OK;
    /* ID3v2: "ID3", version, revision, flags, a 28-bit size in 4 bytes of
     * 7 bits; a footer of 10 more if flags has 0x10. */
    for (unsigned t = 0; t < MAX_ID3; t++) {
        if ((st = read(ctx, off, buf, 10, &n)) != OK)
            goto out;
        if (n < 10 || memcmp(buf, "ID3", 3) || buf[3] == 0xff || buf[4] == 0xff ||
            ((buf[6] | buf[7] | buf[8] | buf[9]) & 0x80))
            break;
        uint32_t size = (uint32_t)buf[6] << 21 | buf[7] << 14 | buf[8] << 7 | buf[9];
        off += 10 + size + (buf[5] & 0x10 ? 10 : 0);
    }
    /* The tags at the end: ID3v1, 128 bytes from "TAG"; before it an APEv2
     * tag's 32-byte footer: "APETAGEX", version, the tag's size with the
     * footer, items, flags (bit 31: a 32-byte header too), all LE. */
    uint64_t end = file_size;
    if (end >= off + 128) {
        if ((st = read(ctx, end - 128, buf, 3, &n)) != OK)
            goto out;
        if (n == 3 && !memcmp(buf, "TAG", 3))
            end -= 128;
    }
    if (end >= off + 32) {
        if ((st = read(ctx, end - 32, buf, 32, &n)) != OK)
            goto out;
        if (n == 32 && !memcmp(buf, "APETAGEX", 8)) {
            uint64_t size = le32(buf + 12) + (le32(buf + 20) & 0x80000000u ? 32u : 0u);
            if (size >= 32 && size <= end - off)
                end -= size;
        }
    }
    s->end = end;
    if (off >= end || (st = read(ctx, off, buf, SNIFF_BYTES, &n)) != OK) {
        st = st == OK ? ERR_WRONG_TYPE : st;
        goto out;
    }
    if (n > end - off)
        n = (size_t)(end - off);
    st = ERR_WRONG_TYPE;
    for (size_t i = 0; i + 4 <= n; i++) {
        struct mp3_header h, h2;
        if (!mp3_header_parse(buf + i, &h))
            continue;
        bool ok = false;
        if (!h.bytes) {
            /* Free format: the frame's length isn't in its header, so
             * any header like it later in the window will do. */
            for (size_t j = i + 4; j + 4 <= n && !ok; j++)
                ok = mp3_header_parse(buf + j, &h2) && same_stream(&h, &h2);
        } else if (i + h.bytes + 4 <= n) {
            ok = mp3_header_parse(buf + i + h.bytes, &h2) && same_stream(&h, &h2);
        } else {
            /* A one-frame file, or one cut off in its second frame: the
             * file ends before where the next header would be. */
            ok = off + i + h.bytes >= end;
        }
        if (!ok)
            continue;
        s->first = h;
        s->offset = off + i;
        /* VBRI: 32 bytes after the header: "VBRI", version, delay,
         * quality (2 bytes each), bytes, frames (4 each, big-endian). */
        if (h.layer == 3 && i + 4 + 32 + 18 <= n && !memcmp(buf + i + 36, "VBRI", 4)) {
            s->vbri = true;
            s->vbri_frames = be32(buf + i + 36 + 14);
        }
        st = OK;
        break;
    }
out:
    free(buf);
    return st;
}

/* dr_mp3's callbacks: the file through m->read, at m->pos. */
static size_t on_read(void *ud, void *dst, size_t n)
{
    struct mp3 *m = ud;
    size_t got = 0;
    if (m->pos >= m->size)
        return 0;
    if (n > m->size - m->pos)
        n = (size_t)(m->size - m->pos);
    status_t st = m->read(m->ctx, m->pos, dst, n, &got);
    if (st != OK) {
        m->err = st;
        return 0;
    }
    m->pos += got;
    return got;
}

static drmp3_bool32 on_seek(void *ud, int offset, drmp3_seek_origin origin)
{
    struct mp3 *m = ud;
    int64_t base = origin == DRMP3_SEEK_SET ? 0 :
                   origin == DRMP3_SEEK_CUR ? (int64_t)m->pos : (int64_t)m->size;
    int64_t to = base + offset;
    if (to < 0 || to > (int64_t)m->size)
        return DRMP3_FALSE;
    m->pos = (uint64_t)to;
    return DRMP3_TRUE;
}

static drmp3_bool32 on_tell(void *ud, drmp3_int64 *cursor)
{
    *cursor = (drmp3_int64)((struct mp3 *)ud)->pos;
    return DRMP3_TRUE;
}

static void *on_malloc(size_t n, void *ud)
{
    (void)ud;
    return malloc(n);
}

static void on_free(void *p, void *ud)
{
    (void)ud;
    free(p);
}

status_t mp3_open(struct mp3 *m, mp3_read_fn read, void *ctx, uint64_t file_size,
                  const char **why)
{
    memset(m, 0, sizeof(*m));
    m->read = read;
    m->ctx = ctx;
    struct mp3_sniffed s;
    status_t st = mp3_sniff(read, ctx, file_size, &s);
    if (st != OK) {
        *why = st == ERR_WRONG_TYPE ? "no MPEG audio frames" : "can't read it";
        return st;
    }
    /* dr_mp3 sees the file without its tags at the end. It looks for them
     * itself, but after a Xing/Info frame it sets its stream cursor back to
     * that frame's end although it has read ahead of it, so its clamp lets
     * it read the ID3v1 tag after the last frame, which then fails the
     * check that the frame ends where the data does: the last frame is
     * lost (an LAME file with an ID3v1 tag played 26 ms short). */
    m->size = s.end;
    drmp3 *d = malloc(sizeof(*d));
    if (!d) {
        *why = "out of memory";
        return ERR_NO_MEMORY;
    }
    /* No realloc: dr_mp3 grows its buffer with malloc, a copy and free. */
    drmp3_allocation_callbacks cb = { NULL, on_malloc, NULL, on_free };
    if (!drmp3_init(d, on_read, on_seek, on_tell, NULL, m, &cb)) {
        free(d);
        st = m->err;
        *why = st != OK ? "can't read it" : "no MPEG audio frame it can decode";
        return st != OK ? st : ERR_NOT_SUPPORTED;
    }
    m->dec = d;
    struct mp3_info *i = &m->info;
    i->rate = d->sampleRate;
    i->channels = (uint16_t)d->channels;
    i->layer = s.first.layer;
    i->version = s.first.version;
    i->kbps = s.first.kbps;
    i->vbr = d->isVBR || s.vbri;
    if (d->totalPCMFrameCount != DRMP3_UINT64_MAX) {
        /* A Xing/Info header (dr_mp3 read it): its frames, less the
         * encoder's delay and padding (LAME's tag), as dr_mp3 plays them. */
        uint64_t f = d->totalPCMFrameCount, cut = (uint64_t)d->delayInPCMFrames +
                                                  d->paddingInPCMFrames;
        i->frames = f > cut ? f - cut : f;
        i->exact = true;
    } else if (s.vbri && s.vbri_frames) {
        i->frames = (uint64_t)s.vbri_frames * s.first.samples;
    } else if (i->kbps) {
        /* Constant bitrate: the audio's bytes at that rate. dr_mp3's
         * streamLength ends before any ID3v1/APE tag. */
        uint64_t bytes = s.end - s.offset;
        i->frames = bytes * 8 * i->rate / (i->kbps * 1000ull);
    }
    return OK;
}

long mp3_decode(struct mp3 *m, int16_t *out, size_t frames)
{
    if (!m->dec)
        return ERR_BAD_STATE;
    uint64_t got = drmp3_read_pcm_frames_s16(m->dec, frames, out);
    if (!got && m->err != OK)
        return m->err;
    return (long)got;
}

void mp3_close(struct mp3 *m)
{
    if (m->dec) {
        drmp3_uninit(m->dec);
        free(m->dec);
        m->dec = NULL;
    }
}
