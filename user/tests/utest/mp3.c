/* utest: <mp3.h> over MPEG audio files built here in memory: frame headers,
 * the sniffing play uses to tell an MP3 from anything else (ID3v2 tags at
 * the start, ID3v1 and APEv2 tags at the end, a frame confirmed by the
 * next), and decoding through dr_mp3 (frames of silence: every bit after
 * the header zero is a valid Layer III frame that decodes to 1152 zero
 * frames), with Xing and VBRI headers and a read error. No device:
 * tools/mp3-test.sh plays real files. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <mp3.h>
#include "utest.h"

#define FRAME   417u     /* MPEG-1 Layer III, 128 kbps, 44100 Hz, no padding */
#define NFRAMES 20u
#define BIG     400u     /* frames: more than dr_mp3's 64 KiB first read */
#define MAXLEN  (BIG * FRAME + 1024)

static uint8_t file[MAXLEN];
static size_t len;

struct mem {
    const uint8_t *p;
    size_t n;
    uint64_t fail_at;   /* a read reaching past this fails (ERR_IO); 0: none */
};

static status_t mem_read(void *ctx, uint64_t off, void *dst, size_t n, size_t *got)
{
    struct mem *m = ctx;
    if (m->fail_at && off + n > m->fail_at)
        return ERR_IO;
    *got = 0;
    if (off >= m->n)
        return OK;
    if (n > m->n - off)
        n = (size_t)(m->n - off);
    memcpy(dst, m->p + off, n);
    *got = n;
    return OK;
}

static void put(const void *p, size_t n)
{
    memcpy(file + len, p, n);
    len += n;
}

/* n silent frames: the header FF FB 90 00 (MPEG-1 Layer III, no CRC,
 * 128 kbps, 44100 Hz, no padding, stereo; C0 as its last byte: mono),
 * then zeros. */
static void frames(unsigned n, bool stereo)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t h[4] = { 0xff, 0xfb, 0x90, stereo ? 0x00 : 0xc0 };
        memset(file + len, 0, FRAME);
        memcpy(file + len, h, 4);
        len += FRAME;
    }
}

static bool header(const uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, struct mp3_header *h)
{
    const uint8_t p[4] = { b0, b1, b2, b3 };
    return mp3_header_parse(p, h);
}

bool t_mp3_header(void)
{
    struct mp3_header h;
    CHECK(header(0xff, 0xfb, 0x90, 0x00, &h));
    CHECK_EQ(h.version, 10);
    CHECK_EQ(h.layer, 3);
    CHECK_EQ(h.rate, 44100);
    CHECK_EQ(h.kbps, 128);
    CHECK_EQ(h.channels, 2);
    CHECK_EQ(h.samples, 1152);
    CHECK_EQ(h.bytes, 417);
    CHECK(!h.crc);
    CHECK(header(0xff, 0xfa, 0x92, 0xc0, &h));   /* padded, CRC, mono */
    CHECK_EQ(h.bytes, 418);
    CHECK(h.crc);
    CHECK_EQ(h.channels, 1);
    /* MPEG-2 Layer III, 64 kbps, 22050 Hz: 576 samples, 72 * 64000 / 22050. */
    CHECK(header(0xff, 0xf3, 0x80, 0xc0, &h));
    CHECK_EQ(h.version, 20);
    CHECK_EQ(h.rate, 22050);
    CHECK_EQ(h.kbps, 64);
    CHECK_EQ(h.samples, 576);
    CHECK_EQ(h.bytes, 208);
    /* MPEG-2.5 Layer III, 8 kbps, 8000 Hz. */
    CHECK(header(0xff, 0xe3, 0x18, 0xc0, &h));
    CHECK_EQ(h.version, 25);
    CHECK_EQ(h.rate, 8000);
    CHECK_EQ(h.kbps, 8);
    /* MPEG-1 Layer II, 192 kbps, 48000 Hz (ffmpeg's mp2); Layer I, 288 kbps. */
    CHECK(header(0xff, 0xfd, 0xa4, 0x04, &h));
    CHECK_EQ(h.layer, 2);
    CHECK_EQ(h.rate, 48000);
    CHECK_EQ(h.kbps, 192);
    CHECK_EQ(h.bytes, 576);
    CHECK(header(0xff, 0xff, 0x90, 0x00, &h));
    CHECK_EQ(h.layer, 1);
    CHECK_EQ(h.samples, 384);
    CHECK_EQ(h.bytes, 312);
    /* Free format: no length. */
    CHECK(header(0xff, 0xfb, 0x00, 0x00, &h));
    CHECK_EQ(h.kbps, 0);
    CHECK_EQ(h.bytes, 0);
    /* Not headers: bitrate 15, rate 3, version 01, layer 00, no sync. */
    CHECK(!header(0xff, 0xfb, 0xf0, 0x00, &h));
    CHECK(!header(0xff, 0xfb, 0x9c, 0x00, &h));
    CHECK(!header(0xff, 0xeb, 0x90, 0x00, &h));
    CHECK(!header(0xff, 0xf9, 0x90, 0x00, &h));
    CHECK(!header(0xfe, 0xfb, 0x90, 0x00, &h));
    return true;
}

static status_t sniff(struct mp3_sniffed *s)
{
    struct mem m = { file, len, 0 };
    return mp3_sniff(mem_read, &m, len, s);
}

bool t_mp3_sniff(void)
{
    struct mp3_sniffed s;
    len = 0;
    frames(NFRAMES, true);
    CHECK_ST(sniff(&s), OK);
    CHECK_EQ(s.offset, 0);
    CHECK_EQ(s.end, len);
    CHECK_EQ(s.first.kbps, 128);
    CHECK(!s.vbri);

    /* An ID3v2 tag of 300 bytes (+ a 10-byte footer, flag 0x10), a second
     * tag of 20, 5 bytes of junk, the frames, an APEv2 tag with a header
     * (32 + 40 + 32 bytes) and an ID3v1 tag. */
    len = 0;
    put("ID3\x04\x00\x10\x00\x00\x02\x2c", 10);   /* 2 * 128 + 44 = 300 */
    memset(file + len, 0, 310);
    len += 310;
    put("ID3\x03\x00\x00\x00\x00\x00\x14", 10);
    memset(file + len, 'x', 20);
    len += 20;
    put("\x01\x02\x03\x04\x05", 5);
    size_t audio = len;
    frames(NFRAMES, true);
    size_t end = len;
    uint8_t ape[32] = "APETAGEX";
    ape[12] = 72;      /* size: 40 of items + the footer */
    ape[23] = 0x80;    /* flags: has a header */
    put(ape, 32);
    memset(file + len, 'i', 40);
    len += 40;
    put(ape, 32);
    uint8_t v1[128] = "TAGA title";
    put(v1, 128);
    CHECK_ST(sniff(&s), OK);
    CHECK_EQ(s.offset, audio);
    CHECK_EQ(s.end, end);

    /* One frame alone; one cut off in its second frame. */
    len = 0;
    frames(1, false);
    CHECK_ST(sniff(&s), OK);
    CHECK_EQ(s.first.channels, 1);
    frames(1, false);
    len -= 100;
    CHECK_ST(sniff(&s), OK);

    /* Refused: a second header that doesn't follow (another version), a
     * header with nothing after it but zeros, an ID3v2 tag alone, noise,
     * a WAV file's start, nothing. */
    len = 0;
    frames(1, true);
    put("\xff\xf3\x80\xc0", 4);
    memset(file + len, 0, 600);
    len += 600;
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    len = 0;
    put("\xff\xfb\x90\x00", 4);
    memset(file + len, 0, 1000);
    len += 1000;
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    len = 0;
    put("ID3\x04\x00\x00\x00\x00\x00\x14", 10);
    memset(file + len, 0xff, 20);   /* sync bytes, but inside the tag */
    len += 20;
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    len = 0;
    for (unsigned i = 0; i < 8000; i++)
        file[len++] = (uint8_t)(i * 37 + 11);
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    len = 0;
    put("RIFF\x24\x00\x00\x00WAVEfmt ", 16);
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    len = 0;
    CHECK_ST(sniff(&s), ERR_WRONG_TYPE);
    return true;
}

/* Decode the whole file: its frames, or a negative status. */
static long decode_all(struct mp3 *m, bool *silent)
{
    static int16_t pcm[4096 * 2];
    long total = 0, n;
    *silent = true;
    while ((n = mp3_decode(m, pcm, 4096)) > 0) {
        for (long i = 0; i < n * m->info.channels; i++)
            *silent = *silent && pcm[i] == 0;
        total += n;
    }
    return n < 0 ? n : total;
}

bool t_mp3_decode(void)
{
    struct mp3 m;
    const char *why;
    bool silent;

    /* Plain CBR: the length estimated from the size. */
    len = 0;
    frames(NFRAMES, true);
    struct mem mm = { file, len, 0 };
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), OK);
    CHECK_EQ(m.info.rate, 44100);
    CHECK_EQ(m.info.channels, 2);
    CHECK_EQ(m.info.layer, 3);
    CHECK_EQ(m.info.kbps, 128);
    CHECK(!m.info.vbr && !m.info.exact);
    CHECK_EQ(m.info.frames, (uint64_t)NFRAMES * FRAME * 8 * 44100 / 128000);
    CHECK_EQ(decode_all(&m, &silent), (long)(NFRAMES * 1152));
    CHECK(silent);
    CHECK_EQ(mp3_decode(&m, (int16_t[2]){ 0 }, 1), 0);   /* still the end */
    mp3_close(&m);

    /* A Xing frame first (flags 1: a frame count of 19): VBR, exact, and
     * not played. "Xing" sits after the side info: 4 + 32 bytes in. */
    memcpy(file + 36, "Xing\x00\x00\x00\x01\x00\x00\x00\x13", 12);
    mm.n = len;
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), OK);
    CHECK(m.info.vbr && m.info.exact);
    CHECK_EQ(m.info.frames, 19u * 1152);
    CHECK_EQ(decode_all(&m, &silent), 19 * 1152);
    mp3_close(&m);

    /* VBRI (Fraunhofer) at the same place: VBR, its count of 30 frames. */
    memset(file + 36, 0, 12);
    memcpy(file + 36, "VBRI\x00\x01\x00\x00\x00\x00\x00\x00\x20\x00\x00\x00\x00\x1e", 18);
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), OK);
    CHECK(m.info.vbr && !m.info.exact);
    CHECK_EQ(m.info.frames, 30u * 1152);
    mp3_close(&m);
    memset(file + 36, 0, 18);

    /* Mono, with tags at both ends: they are not decoded. */
    len = 0;
    put("ID3\x04\x00\x00\x00\x00\x00\x0a", 10);
    memset(file + len, 0, 10);
    len += 10;
    frames(NFRAMES, false);
    uint8_t v1[128] = "TAG";
    memset(v1 + 3, 0xff, 125);   /* would be sync bytes, if decoded */
    put(v1, 128);
    mm.n = len;
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), OK);
    CHECK_EQ(m.info.channels, 1);
    CHECK_EQ(decode_all(&m, &silent), (long)(NFRAMES * 1152));
    CHECK(silent);
    mp3_close(&m);

    /* A read error halfway (past the first 64 KiB dr_mp3 reads at the
     * open): decoding stops with it. */
    len = 0;
    frames(BIG, true);
    mm.n = len;
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), OK);
    mm.fail_at = len / 2;
    CHECK_EQ(decode_all(&m, &silent), ERR_IO);
    mp3_close(&m);
    mm.fail_at = 0;

    /* Not MPEG audio. */
    len = 0;
    for (unsigned i = 0; i < 4000; i++)
        file[len++] = (uint8_t)(i * 37 + 11);
    mm.n = len;
    CHECK_ST(mp3_open(&m, mem_read, &mm, len, &why), ERR_WRONG_TYPE);
    CHECK(strcmp(why, "no MPEG audio frames") == 0);
    mp3_close(&m);   /* harmless after a failed open */
    return true;
}
