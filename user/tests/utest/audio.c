/* utest: the pure parts of libos's sound output: <audio.h>'s sample
 * conversions and resampler, and <wav.h>'s header parser (over files
 * built here in memory). No device: tools/play-test.sh plays real files. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <audio.h>
#include <check.h>
#include <wav.h>
#include "utest.h"

bool t_audio_formats(void)
{
    static const uint8_t u8[] = { 0, 128, 255, 129 };
    int16_t out[4];
    audio_s16_from_u8(out, u8, 4);
    CHECK_EQ(out[0], -32768);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 32512);
    CHECK_EQ(out[3], 256);

    /* 24-bit: the top 16 bits, rounded to nearest, clamped at the top. */
    static const uint8_t s24[] = { 0xff, 0xff, 0x7f,   0x00, 0x00, 0x80,   0x00, 0x01, 0x00,
                                   0x80, 0x00, 0x00,   0x7f, 0x00, 0x00,   0xff, 0xff, 0xff,
                                   0x00, 0xff, 0xff };
    int16_t o24[7];
    audio_s16_from_s24le(o24, s24, 7);
    CHECK_EQ(o24[0], 32767);    /* 0x7fffff */
    CHECK_EQ(o24[1], -32768);   /* -0x800000 */
    CHECK_EQ(o24[2], 1);        /* 0x000100 */
    CHECK_EQ(o24[3], 1);        /* 0x000080: half a step, up */
    CHECK_EQ(o24[4], 0);        /* 0x00007f */
    CHECK_EQ(o24[5], 0);        /* -1 */
    CHECK_EQ(o24[6], -1);       /* -0x000100 */

    static const uint8_t s32[] = { 0xff, 0xff, 0xff, 0x7f,   0x00, 0x00, 0x00, 0x80,
                                   0x00, 0x00, 0x01, 0x00,   0x00, 0x00, 0xff, 0xff };
    int16_t o32[4];
    audio_s16_from_s32le(o32, s32, 4);
    CHECK_EQ(o32[0], 32767);
    CHECK_EQ(o32[1], -32768);
    CHECK_EQ(o32[2], 1);
    CHECK_EQ(o32[3], -1);
    return true;
}

#define RAMP 1000u

bool t_audio_resample(void)
{
    static int16_t in[2 * RAMP], out[2 * 2 * RAMP], again[2 * 2 * RAMP];
    struct audio_rs rs;
    size_t used;

    /* The same rate: the frames as they are; mono becomes both channels. */
    for (unsigned k = 0; k < 8; k++)
        in[k] = (int16_t)(k * 100 - 300);
    audio_rs_init(&rs, 48000, 48000, 2);
    CHECK_EQ(audio_rs_run(&rs, in, 4, &used, out, 16), 4);
    CHECK_EQ(used, 4);
    for (unsigned k = 0; k < 8; k++)
        CHECK_EQ(out[k], in[k]);
    audio_rs_init(&rs, 48000, 48000, 1);
    CHECK_EQ(audio_rs_run(&rs, in, 8, &used, out, 5), 5);   /* only 5 fit */
    CHECK_EQ(used, 5);
    for (unsigned k = 0; k < 5; k++) {
        CHECK_EQ(out[2 * k], in[k]);
        CHECK_EQ(out[2 * k + 1], in[k]);
    }

    /* 44100 -> 48000 of a ramp: linear interpolation of a straight line is
     * the line, so output frame o is 10 * o * 44100 / 48000, to a step. */
    for (unsigned k = 0; k < RAMP; k++)
        in[k] = (int16_t)(k * 10);
    audio_rs_init(&rs, 44100, 48000, 1);
    size_t n = audio_rs_run(&rs, in, RAMP, &used, out, 2 * RAMP);
    CHECK_EQ(used, RAMP);
    size_t want = (size_t)(RAMP - 1) * 48000 / 44100 + 1;
    if (n + 1 < want || n > want + 1)
        FAIL("%lu frames out of %u, want %lu (+-1)", (unsigned long)n, RAMP, (unsigned long)want);
    for (size_t o = 0; o < n; o++) {
        int64_t exact = (int64_t)o * 10 * 44100 * 2 / 48000;   /* twice the value */
        int64_t got = 2 * (int64_t)out[2 * o];
        if (got - exact > 2 || exact - got > 2 || out[2 * o] != out[2 * o + 1])
            FAIL("frame %lu is %d/%d, want %ld.%ld", (unsigned long)o, out[2 * o],
                 out[2 * o + 1], (long)(exact / 2), (long)(exact % 2 * 5));
    }

    /* The same input in odd pieces into a small buffer gives the same frames. */
    audio_rs_init(&rs, 44100, 48000, 1);
    size_t got = 0, at = 0;
    while (at < RAMP) {
        size_t piece = RAMP - at < 7 ? RAMP - at : 7;
        size_t m = audio_rs_run(&rs, in + at, piece, &used, again + 2 * got, 5);
        CHECK(m || used);
        got += m;
        at += used;
    }
    CHECK_EQ(got, n);
    CHECK(!memcmp(again, out, n * 4));

    /* 96000 -> 48000: every other frame exactly (stereo kept apart). */
    for (unsigned k = 0; k < RAMP; k++) {
        in[2 * k] = (int16_t)k;
        in[2 * k + 1] = (int16_t)-k;
    }
    audio_rs_init(&rs, 96000, 48000, 2);
    n = audio_rs_run(&rs, in, RAMP, &used, out, RAMP);
    CHECK_EQ(used, RAMP);
    CHECK_EQ(n, RAMP / 2);
    for (size_t o = 0; o < n; o++) {
        CHECK_EQ(out[2 * o], 2 * o);
        CHECK_EQ(out[2 * o + 1], -2 * (int64_t)o);
    }

    /* 8000 -> 48000: six frames per input frame, the first one exact. */
    in[0] = 0;
    in[1] = 600;
    in[2] = 0;
    audio_rs_init(&rs, 8000, 48000, 1);
    n = audio_rs_run(&rs, in, 3, &used, out, 64);
    CHECK_EQ(n, 12);
    for (unsigned o = 0; o < 6; o++) {
        CHECK_EQ(out[2 * o], o * 100);
        CHECK_EQ(out[2 * (6 + o)], 600 - o * 100);
    }
    return true;
}

/* ---- WAV files in memory -------------------------------------------------------- */

struct mem {
    const uint8_t *p;
    size_t         n;
};

static status_t mem_read(void *ctx, uint64_t off, void *dst, size_t n, size_t *got)
{
    struct mem *m = ctx;
    *got = off >= m->n ? 0 : m->n - off < n ? m->n - off : n;
    memcpy(dst, m->p + off, *got);
    return OK;
}

static uint8_t file[512];
static size_t  len;

static void put32(uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        file[len++] = (uint8_t)(v >> (8 * i));
}

static void put16(uint16_t v)
{
    file[len++] = (uint8_t)v;
    file[len++] = (uint8_t)(v >> 8);
}

static void put_id(const char *id)
{
    memcpy(file + len, id, 4);
    len += 4;
}

/* RIFF/WAVE, an odd-sized LIST chunk (padded), then fmt (extensible with
 * subformat `sub` if ext), then a data chunk claiming data_size bytes
 * with `have` bytes of it present. */
static void build(uint16_t format, uint16_t ch, uint32_t rate, uint16_t bits, bool ext,
                  uint16_t sub, uint32_t data_size, uint32_t have)
{
    len = 0;
    put_id("RIFF");
    put32(0);   /* fixed below */
    put_id("WAVE");
    put_id("LIST");
    put32(3);
    file[len++] = 'a';
    file[len++] = 'b';
    file[len++] = 'c';
    file[len++] = 0;   /* the pad byte */
    put_id("fmt ");
    put32(ext ? 40 : 16);
    put16(ext ? 0xfffe : format);
    put16(ch);
    put32(rate);
    put32(rate * ch * bits / 8);
    put16((uint16_t)(ch * bits / 8));
    put16(bits);
    if (ext) {
        put16(22);
        put16(bits);
        put32(ch == 2 ? 3 : 4);
        put16(sub);
        static const uint8_t guid[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b,
                                          0x71 };
        memcpy(file + len, guid, sizeof(guid));
        len += sizeof(guid);
    }
    put_id("data");
    put32(data_size);
    for (uint32_t i = 0; i < have; i++)
        file[len++] = (uint8_t)i;
    file[4] = (uint8_t)(len - 8);
}

static status_t parse(struct wav_info *w, size_t n, const char **why)
{
    struct mem m = { file, n };
    return wav_parse(w, mem_read, &m, n, why);
}

bool t_wav_parse(void)
{
    struct wav_info w;
    const char *why;

    build(1, 2, 44100, 16, false, 0, 400, 400);
    CHECK_ST(parse(&w, len, &why), OK);
    CHECK_EQ(w.rate, 44100);
    CHECK_EQ(w.channels, 2);
    CHECK_EQ(w.bits, 16);
    CHECK_EQ(w.frame_bytes, 4);
    CHECK_EQ(w.frames, 100);
    CHECK_EQ(w.data_offset, len - 400);

    /* Extensible with the PCM subformat: 24-bit mono. */
    build(0, 1, 48000, 24, true, 1, 300, 300);
    CHECK_ST(parse(&w, len, &why), OK);
    CHECK_EQ(w.format, 0xfffe);
    CHECK_EQ(w.bits, 24);
    CHECK_EQ(w.frames, 100);

    /* 8-bit, cut off in its samples: as many whole frames as are there. */
    build(1, 2, 22050, 8, false, 0, 1000, 201);
    CHECK_ST(parse(&w, len, &why), OK);
    CHECK_EQ(w.frames, 100);

    /* Refused: float (plain and extensible), compressed, 3 channels,
     * 12-bit, a rate of 4000 Hz, no RIFF, cut off in its header, no data. */
    build(3, 2, 48000, 32, false, 0, 8, 8);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(0, 2, 48000, 32, true, 3, 8, 8);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(0x55, 2, 48000, 16, false, 0, 8, 8);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(1, 3, 48000, 16, false, 0, 12, 12);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(1, 1, 48000, 12, false, 0, 12, 12);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(1, 1, 4000, 16, false, 0, 12, 12);
    CHECK_ST(parse(&w, len, &why), ERR_NOT_SUPPORTED);
    build(1, 2, 44100, 16, false, 0, 400, 400);
    memcpy(file, "RIFX", 4);
    CHECK_ST(parse(&w, len, &why), ERR_WRONG_TYPE);
    CHECK(strcmp(why, "not a WAV file (no RIFF/WAVE header)") == 0);
    build(1, 2, 44100, 16, false, 0, 400, 400);
    CHECK_ST(parse(&w, 36, &why), ERR_OUT_OF_RANGE);   /* inside fmt (its body is at 32) */
    CHECK_ST(parse(&w, 8, &why), ERR_WRONG_TYPE);
    CHECK_ST(parse(&w, 20, &why), ERR_WRONG_TYPE);     /* no fmt yet */
    size_t data_at = len - 400 - 8;
    CHECK_ST(parse(&w, data_at, &why), ERR_OUT_OF_RANGE);   /* fmt, no data */

    /* Garbage. */
    for (unsigned i = 0; i < 64; i++)
        file[i] = (uint8_t)(i * 37 + 11);
    CHECK_ST(parse(&w, 64, &why), ERR_WRONG_TYPE);
    return true;
}
