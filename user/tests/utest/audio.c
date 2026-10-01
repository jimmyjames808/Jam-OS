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
#define TWO_PI 6.283185307179586

/* sin by its series after range reduction (utest has no libm). */
static double usin(double x)
{
    x -= TWO_PI * (double)(int64_t)(x / TWO_PI);
    if (x > TWO_PI / 2)
        x -= TWO_PI;
    else if (x < -TWO_PI / 2)
        x += TWO_PI;
    double term = x, sum = x;
    for (int k = 1; k < 20; k++) {
        term *= -x * x / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

/* in_frames of a sine at hz (amp) on each channel (ch 1 uses hz2), at rate. */
static void sine(int16_t *in, size_t frames, unsigned channels, unsigned rate, double hz,
                 double hz2, double amp)
{
    for (size_t k = 0; k < frames; k++)
        for (unsigned c = 0; c < channels; c++) {
            double v = amp * usin(TWO_PI * (c ? hz2 : hz) * (double)k / rate);
            in[k * channels + c] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
}

/* All of in through rs (run, then flush) into out (cap frames): how many. */
static size_t resample_all(struct audio_rs *rs, const int16_t *in, size_t frames, int16_t *out,
                           size_t cap, size_t piece, size_t room)
{
    size_t got = 0, at = 0, used;
    while (at < frames) {
        size_t k = frames - at < piece ? frames - at : piece;
        size_t c = cap - got < room ? cap - got : room;
        size_t m = audio_rs_run(rs, in + at * rs->channels, k, &used, out + 2 * got, c);
        if (!m && !used)
            return got;   /* out is full */
        got += m;
        at += used;
    }
    size_t m;
    while (got < cap && (m = audio_rs_flush(rs, out + 2 * got,
                                            cap - got < room ? cap - got : room)) > 0)
        got += m;
    return got;
}

/* Output frames [from, to) of channel c against amp * sin(2 pi hz t) at
 * 48 kHz: the largest difference. */
static double worst(const int16_t *out, size_t from, size_t to, unsigned c, double hz, double amp)
{
    double w = 0;
    for (size_t o = from; o < to; o++) {
        double d = out[2 * o + c] - amp * usin(TWO_PI * hz * (double)o / 48000);
        if (d < 0)
            d = -d;
        if (d > w)
            w = d;
    }
    return w;
}

bool t_audio_resample(void)
{
    static int16_t in[2 * 9600], out[2 * 9800], again[2 * 9800];
    struct audio_rs rs;
    size_t used;

    /* The same rate: the frames as they are; mono becomes both channels. */
    for (unsigned k = 0; k < 8; k++)
        in[k] = (int16_t)(k * 100 - 300);
    CHECK_ST(audio_rs_init(&rs, 48000, 48000, 2), OK);
    CHECK_EQ(audio_rs_run(&rs, in, 4, &used, out, 16), 4);
    CHECK_EQ(used, 4);
    for (unsigned k = 0; k < 8; k++)
        CHECK_EQ(out[k], in[k]);
    CHECK_EQ(audio_rs_flush(&rs, out, 16), 0);
    CHECK_ST(audio_rs_init(&rs, 48000, 48000, 1), OK);
    CHECK_EQ(audio_rs_run(&rs, in, 8, &used, out, 5), 5);   /* only 5 fit */
    CHECK_EQ(used, 5);
    for (unsigned k = 0; k < 5; k++) {
        CHECK_EQ(out[2 * k], in[k]);
        CHECK_EQ(out[2 * k + 1], in[k]);
    }

    /* 44100 -> 48000: 1 kHz and 20 kHz sines come out as the same sines at
     * 48 kHz, on time (output frame o is at o / 48000 s), to the dither's
     * +-1 and the passband's ripple; the start's step rings for the
     * filter's half length, so the comparison starts after it. */
    static const double hzs[] = { 1000, 20000 };
    for (unsigned t = 0; t < 2; t++) {
        sine(in, 4410, 1, 44100, hzs[t], 0, 10000);
        CHECK_ST(audio_rs_init(&rs, 44100, 48000, 1), OK);
        CHECK_EQ(rs.L, 160);
        CHECK_EQ(rs.M, 147);
        size_t n = resample_all(&rs, in, 4410, out, 9800, 4410, 9800);
        if (n < 4800 || n > 4802)
            FAIL("%lu frames from 4410 at 44.1 kHz, want 4800-4802", (unsigned long)n);
        double w = worst(out, 200, 4600, 0, hzs[t], 10000);
        if (w > 2.5)
            FAIL("%.0f Hz from 44.1 kHz: off the sine by %.2f", hzs[t], w);
        for (size_t o = 0; o < n; o++)
            CHECK_EQ(out[2 * o], out[2 * o + 1]);
        /* The same input in odd pieces into a small buffer: the same frames. */
        audio_rs_reset(&rs);
        rs.seed = 0x2545f491u;
        size_t m = resample_all(&rs, in, 4410, again, 9800, 7, 5);
        CHECK_EQ(m, n);
        CHECK(!memcmp(again, out, n * 4));
        audio_rs_free(&rs);
    }

    /* 96000 -> 48000 stereo: 1 kHz left comes through; 30 kHz right (it
     * would alias to 18 kHz) is gone: nothing above the dither. */
    sine(in, 9600, 2, 96000, 1000, 30000, 10000);
    CHECK_ST(audio_rs_init(&rs, 96000, 48000, 2), OK);
    size_t n = resample_all(&rs, in, 9600, out, 9800, 9600, 9800);
    if (n < 4800 || n > 4802)
        FAIL("%lu frames from 9600 at 96 kHz", (unsigned long)n);
    double w = worst(out, 200, 4600, 0, 1000, 10000);
    if (w > 2.5)
        FAIL("1 kHz from 96 kHz: off by %.2f", w);
    w = worst(out, 200, 4600, 1, 0, 0);
    if (w > 2)
        FAIL("30 kHz from 96 kHz: %.2f left (an alias at 18 kHz)", w);
    audio_rs_free(&rs);

    /* 8000 -> 48000 and an odd rate whose phases are interpolated. */
    static const unsigned rates[] = { 8000, 44056 };
    for (unsigned t = 0; t < 2; t++) {
        unsigned r = rates[t];
        size_t frames = r / 10;
        sine(in, frames, 1, r, 1000, 0, 10000);
        CHECK_ST(audio_rs_init(&rs, r, 48000, 1), OK);
        n = resample_all(&rs, in, frames, out, 9800, frames, 9800);
        if (n < 4800 || n > 4800 + 48000 / r)   /* the flush ends at the next input frame */
            FAIL("%lu frames from %lu at %u Hz", (unsigned long)n, (unsigned long)frames, r);
        w = worst(out, 300, 4500, 0, 1000, 10000);
        if (w > 2.5)
            FAIL("1 kHz from %u Hz: off by %.2f", r, w);
        audio_rs_free(&rs);
    }

    /* The cost: a second of 44.1 kHz stereo. */
    sine(in, 4410, 2, 44100, 1000, 1500, 10000);
    CHECK_ST(audio_rs_init(&rs, 44100, 48000, 2), OK);
    unsigned taps = rs.taps;
    uint64_t t0 = now();
    for (unsigned k = 0; k < 10; k++) {
        size_t at = 0;
        while (at < 4410) {
            size_t m = audio_rs_run(&rs, in + 2 * at, 4410 - at, &used, out, 9800);
            (void)m;
            at += used;
        }
    }
    uint64_t ns = now() - t0;
    audio_rs_free(&rs);
    printf("utest: audio_resample: 1 s of 44.1 kHz stereo in %lu.%03lu ms (%u taps)\n",
           (unsigned long)(ns / NS_PER_MS), (unsigned long)(ns % NS_PER_MS / 1000), taps);
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
