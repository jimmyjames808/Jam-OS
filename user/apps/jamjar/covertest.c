/* jamjar: the self-test's covers part (selftest.c runs it): the ID3v2
 * parser on tags built here (2.2, 2.3, 2.4; unsynchronisation of the whole
 * tag and of one frame; a data length indicator; UTF-16 descriptions; the
 * front cover chosen over another picture), tags that are broken in every
 * way it guards against, and a few thousand randomly damaged ones; then
 * the cover helper (decoder.c, bin/jamcover, whose own self-test checks
 * stb_image): a PNG decoded, a non-picture refused by the same helper, a
 * helper that crashes and one that hangs replaced; the scaling; and the
 * covers' states as cover.c's thread moves them
 * (a fake decoder, no thread), with art.c drawing over them; and the
 * second try of a decode that timed out (the real helper, no files). */
#include <testpics.h>
#include "jamjar.h"

static uint8_t buf[4096], plain[2048];

static size_t be32(uint8_t *p, size_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4;
}

static size_t ss32(uint8_t *p, size_t v)
{
    p[0] = (uint8_t)(v >> 21 & 0x7f);
    p[1] = (uint8_t)(v >> 14 & 0x7f);
    p[2] = (uint8_t)(v >> 7 & 0x7f);
    p[3] = (uint8_t)(v & 0x7f);
    return 4;
}

/* Unsynchronise src into dst (a 0x00 after every 0xFF); the new length. */
static size_t unsync(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        dst[o++] = src[i];
        if (src[i] == 0xff)
            dst[o++] = 0;
    }
    return o;
}

/* A frame of version ver (2, 3, 4): its header and data at p. */
static size_t mkframe(uint8_t *p, int ver, const char *id, const uint8_t *d, size_t n, uint8_t fl)
{
    size_t h = ver == 2 ? 6 : 10;
    memcpy(p, id, ver == 2 ? 3 : 4);
    if (ver == 2) {
        p[3] = (uint8_t)(n >> 16);
        p[4] = (uint8_t)(n >> 8);
        p[5] = (uint8_t)n;
    } else {
        (ver == 3 ? be32 : ss32)(p + 4, n);
        p[8] = 0;
        p[9] = fl;
    }
    memcpy(p + h, d, n);
    return h + n;
}

/* An APIC's data (PIC's for ver 2): encoding, MIME, type, description. */
static size_t apic(uint8_t *p, int ver, uint8_t enc, uint8_t type, const uint8_t *desc,
                   size_t dn, const uint8_t *img, size_t n)
{
    size_t o = 0;
    p[o++] = enc;
    const char *mime = img[0] == 0x89 ? "image/png" : "image/jpeg";
    if (ver == 2) {
        memcpy(p + o, img[0] == 0x89 ? "PNG" : "JPG", 3);
        o += 3;
    } else {
        memcpy(p + o, mime, strlen(mime) + 1);
        o += strlen(mime) + 1;
    }
    p[o++] = type;
    memcpy(p + o, desc, dn);
    o += dn;
    memcpy(p + o, img, n);
    return o + n;
}

/* The tag around body[0..n). */
static size_t tag(uint8_t *out, int ver, uint8_t flags, const uint8_t *body, size_t n)
{
    memcpy(out, "ID3", 3);
    out[3] = (uint8_t)ver;
    out[4] = 0;
    out[5] = flags;
    ss32(out + 6, n);
    memcpy(out + 10, body, n);
    return 10 + n;
}

static bool is(const struct id3_pic *p, const uint8_t *img, size_t n)
{
    return p->len == n && !memcmp(p->data, img, n);
}

/* 2.3: a title, a back cover, then the front cover: the front one. */
static bool v23(void)
{
    static uint8_t body[1024], d[512];
    size_t o = mkframe(body, 3, "TIT2", (const uint8_t *)"\0Song", 5, 0);
    size_t n = apic(d, 3, 0, 4, (const uint8_t *)"back", 5, test_jpg16, sizeof(test_jpg16));
    o += mkframe(body + o, 3, "APIC", d, n, 0);
    n = apic(d, 3, 0, 3, (const uint8_t *)"Cover", 6, test_png4, sizeof(test_png4));
    o += mkframe(body + o, 3, "APIC", d, n, 0);
    memset(body + o, 0, 64);   /* padding */
    size_t t = tag(buf, 3, 0, body, o + 64);
    struct id3_pic p;
    return id3_tag_size(buf) == t && id3_cover(buf, t, &p) && p.type == 3 && p.png &&
           is(&p, test_png4, sizeof(test_png4));
}

/* 2.4, a UTF-16 description (with a BOM); and 2.2's PIC. */
static bool v24_v22(void)
{
    static uint8_t body[1024], d[512];
    static const uint8_t desc[] = { 0xff, 0xfe, 'a', 0, 0, 0 };
    size_t n = apic(d, 4, 1, 3, desc, sizeof(desc), test_jpg16, sizeof(test_jpg16));
    size_t t = tag(buf, 4, 0, body, mkframe(body, 4, "APIC", d, n, 0));
    struct id3_pic p;
    bool ok = id3_cover(buf, t, &p) && p.jpeg && is(&p, test_jpg16, sizeof(test_jpg16));
    n = apic(d, 2, 0, 3, (const uint8_t *)"", 1, test_png4, sizeof(test_png4));
    t = tag(buf, 2, 0, body, mkframe(body, 2, "PIC", d, n, 0));
    return ok && id3_cover(buf, t, &p) && p.png && is(&p, test_png4, sizeof(test_png4));
}

/* Unsynchronisation: 2.3's whole tag; 2.4's one frame with a length indicator. */
static bool unsynced(void)
{
    static uint8_t body[1024], d[512], f[1024];
    size_t n = apic(d, 3, 0, 3, (const uint8_t *)"", 1, test_jpg16, sizeof(test_jpg16));
    size_t o = mkframe(plain, 3, "APIC", d, n, 0);
    size_t t = tag(buf, 3, 0x80, body, unsync(body, plain, o));
    struct id3_pic p;
    bool ok = id3_cover(buf, t, &p) && is(&p, test_jpg16, sizeof(test_jpg16));
    n = apic(d, 4, 0, 3, (const uint8_t *)"", 1, test_jpg16, sizeof(test_jpg16));
    size_t fn = ss32(f, n);
    fn += unsync(f + fn, d, n);
    t = tag(buf, 4, 0, body, mkframe(body, 4, "APIC", f, fn, 0x03));
    return ok && fn > n + 4 && id3_cover(buf, t, &p) && is(&p, test_jpg16, sizeof(test_jpg16));
}

/* What must give nothing (and not crash). */
static bool broken(void)
{
    static uint8_t body[1024], d[512];
    struct id3_pic p;
    size_t n = apic(d, 4, 0, 3, (const uint8_t *)"", 1, test_png4, sizeof(test_png4));
    size_t t = tag(buf, 4, 0, body, mkframe(body, 4, "APIC", d, n, 0x08));   /* compressed */
    bool ok = !id3_cover(buf, t, &p);
    t = tag(buf, 4, 0, body, mkframe(body, 4, "APIC", d, n, 0));
    ss32(buf + 14, n + 100);   /* the frame says it is longer than the tag */
    ok &= !id3_cover(buf, t, &p);
    static const uint8_t url[] = "-->http://example.com/cover.png";
    n = apic(d, 3, 0, 3, (const uint8_t *)"", 1, url, sizeof(url));
    t = tag(buf, 3, 0, body, mkframe(body, 3, "APIC", d, n, 0));   /* a link, not an image */
    ok &= !id3_cover(buf, t, &p);
    d[0] = 0;
    memcpy(d + 1, "image/png", 9);   /* no NUL after the MIME type, nor anywhere */
    t = tag(buf, 3, 0, body, mkframe(body, 3, "APIC", d, 10, 0));
    ok &= !id3_cover(buf, t, &p);
    uint8_t h[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0x80, 0 };
    ok &= id3_tag_size(h) == 0;   /* not syncsafe */
    h[8] = 0;
    h[3] = 5;
    ok &= id3_tag_size(h) == 0;   /* no such version */
    h[3] = 4;
    h[5] = 0x10;
    h[9] = 7;
    ok &= id3_tag_size(h) == 10 + 7 + 10;   /* a footer */
    return ok && !id3_cover(buf, 9, &p);
}

/* Random damage to a good tag: never a crash, never a picture outside it. */
static bool fuzz(void)
{
    static uint8_t body[1024], d[512], good[2048];
    size_t n = apic(d, 4, 1, 3, (const uint8_t *)"\xff\xfe" "a\0\0\0", 6, test_png4,
                    sizeof(test_png4));
    size_t o = mkframe(body, 4, "TIT2", (const uint8_t *)"\0x", 2, 0);
    o += mkframe(body + o, 4, "APIC", d, n, 0x02);
    size_t t = tag(good, 4, 0, body, o);
    uint64_t r = 77;
    bool ok = true;
    for (int i = 0; i < 4000; i++) {
        memcpy(buf, good, t);
        for (int k = 0; k < 1 + i % 6; k++)
            buf[rng_next(&r) % t] ^= (uint8_t)(1u << (rng_next(&r) % 8));
        struct id3_pic p;
        size_t len = id3_tag_size(buf) && id3_tag_size(buf) <= t ? t : 0;
        if (len && id3_cover(buf, len, &p))
            ok &= p.data >= buf && p.data + p.len <= buf + t;
    }
    return ok;
}

static void test_id3(void)
{
    fun_check(v23(), "id3: 2.3, the front cover over a back cover, padding after");
    fun_check(v24_v22(), "  ... 2.4 with a UTF-16 description; 2.2's PIC");
    fun_check(unsynced(), "  ... unsynchronised: 2.3's whole tag, 2.4's frame (+ length)");
    fun_check(broken(), "  ... compressed, overlong, a link, no terminator, bad headers: none");
    fun_check(fuzz(), "  ... 4000 randomly damaged tags: no crash, nothing outside the tag");
}

/* ---- the helper (decoder.c, bin/jamcover) ------------------------------------------------ */

static uint32_t *hsmall, *hlarge;

/* The PNG through the helper: OK, and its pixels at both sizes. */
static status_t helper_png(void)
{
    uint8_t *in = decoder_buffer();
    int w = 0, h = 0;
    if (!in)
        return ERR_NO_MEMORY;
    memcpy(in, test_png4, sizeof(test_png4));
    status_t st = decoder_decode(sizeof(test_png4), true, hsmall, hlarge, &w, &h);
    if (st != OK)
        return st;
    bool ok = w == 4 && h == 4 && hsmall[0] == 0xffff0000u &&
              hsmall[COVER_SMALL - 1] == 0x80000080u && hlarge[0] == 0xffff0000u &&
              hlarge[COVER_LARGE * COVER_LARGE - 1] == 0x80000080u;
    return ok ? OK : ERR_INTERNAL;
}

static void test_helper(void)
{
    if (!hsmall)
        hsmall = big_alloc((uint64_t)COVER_SMALL * COVER_SMALL * 4);
    if (!hlarge)
        hlarge = big_alloc((uint64_t)COVER_LARGE * COVER_LARGE * 4);
    if (!hsmall || !hlarge || !decoder_buffer()) {
        fun_check(false, "helper: no memory for the pixels");
        return;
    }
    decoder_test(NULL, 0);
    unsigned n = decoder_test_starts();
    bool ok = helper_png() == OK && decoder_test_starts() == n + 1;
    fun_check(ok, "helper: bin/jamcover decodes a PNG into both sizes");
    static const char junk[] = "no picture at all";
    memcpy(decoder_buffer(), junk, sizeof(junk));
    int w, h;
    ok = decoder_decode(sizeof(junk), false, hsmall, NULL, &w, &h) == ERR_NOT_SUPPORTED;
    ok &= helper_png() == OK && decoder_test_starts() == n + 1;
    fun_check(ok, "  ... refuses what is no picture, and goes on (the same helper)");
    decoder_test("--crash", 0);
    ok = helper_png() == ERR_PEER_CLOSED && decoder_test_starts() == n + 2;
    decoder_test(NULL, 0);
    ok &= helper_png() == OK && decoder_test_starts() == n + 3;
    fun_check(ok, "  ... one that crashes on a picture: that cover lost, a new helper");
    decoder_test("--hang", 300 * NS_PER_MS);
    uint64_t t0 = now();
    ok = helper_png() == ERR_TIMED_OUT && now() - t0 >= 300 * NS_PER_MS;
    decoder_test(NULL, 0);
    ok &= helper_png() == OK && decoder_test_starts() == n + 5;
    fun_check(ok, "  ... one that hangs: killed at its deadline, a new helper");
    /* Scaling: the PNG, premultiplied, to 2x2 by area: red; blue at half. */
    uint32_t src[16], dst[4];
    for (int i = 0; i < 16; i++)
        src[i] = i % 4 < 2 ? 0xffff0000u : 0x80000080u;
    scale_pm(&(struct picture){ src, 4, 4, 4 }, dst, 2, 2);
    ok = dst[0] == 0xffff0000u && dst[1] == 0x80000080u && dst[2] == dst[0] && dst[3] == dst[1];
    scale_pm(&(struct picture){ src, 4, 4, 4 }, dst, 1, 1);   /* half and half */
    ok &= dst[0] == 0xc0800040u;
    fun_check(ok, "scaling down averages areas (premultiplied)");
}

/* ---- the covers' states, without the thread (cover_test_*) ------------------------------- */

#define SIDE 300   /* drawn bigger than COVER_SMALL * 9 / 8: the large copy is asked for */
#define PA "/m/A/Album/a.mp3"
#define PB "/m/B/Album/b.mp3"
#define PC "/m/C/Album/c.mp3"

static uint32_t *spx;

static void work_all(void)
{
    for (int guard = 0; guard < 16 && cover_test_work(); guard++)
        ;
}

/* Album `h` drawn big from path p into the test's surface at `size`;
 * whether its middle is p's cover (fake_decode's colour). */
static bool shows(uint64_t h, const char *p, int size)
{
    struct surf s = { spx, size, size, size };
    art_cover(&s, &(struct rect){ 0, 0, size, size }, h, p, C_PANEL);
    uint32_t want = 0xff000000u | (uint32_t)(name_hash(p) & 0xffffff);
    return (spx[(size_t)size / 2 * size + size / 2] | 0xff000000u) == want;
}

/* Between art_cover asking what is ready and drawing it: B and C asked
 * for big after A, and the thread's work done, so the one of them without
 * a large copy takes A's (the least recently drawn of the two kept). */
static void take_a_large(void)
{
    (void)cover_ready(0xb, PB, SIDE, false);
    (void)cover_ready(0xc, PC, SIDE, false);
    work_all();
}

static void test_states(void)
{
    if (!spx && !(spx = big_alloc((uint64_t)(SIDE + 20) * (SIDE + 20) * 4))) {
        fun_check(false, "covers: no memory to draw into");
        return;
    }
    bool ok = cover_test_start() && !shows(0xa, PA, SIDE);   /* asked: its label meanwhile */
    work_all();
    ok &= shows(0xa, PA, SIDE);
    fun_check(ok, "covers: asked for, read, then drawn (both sizes in one read)");
    ok = !shows(0xb, PB, SIDE) && !shows(0xc, PC, SIDE);
    work_all();   /* B and C take the two large copies; A's goes */
    ok &= shows(0xb, PB, SIDE) && shows(0xc, PC, SIDE) && shows(0xa, PA, SIDE);
    fun_check(ok, "  ... a third album drawn big: A's large copy goes, A shows its small one");
    work_all();   /* A's large copy read again */
    art_test_hook(take_a_large);
    ok = shows(0xa, PA, SIDE);   /* kept (small) -> large, which goes before it is drawn */
    art_test_hook(NULL);
    (void)cover_ready(0xa, PA, SIDE, false);
    work_all();   /* A's large copy read once more */
    art_test_hook(take_a_large);
    ok &= shows(0xa, PA, SIDE + 20);   /* not kept yet: the same, on a new picture */
    art_test_hook(NULL);
    fun_check(ok, "  ... a copy taken away mid-draw: the picture, never the label");
}

/* The pictures test_retry's albums carry: the test PNG, or for a path
 * with "junk" in it something that is no picture. */
static size_t retry_pic(const char *path)
{
    static const char junk[] = "no picture at all";
    uint8_t *in = decoder_buffer();
    bool bad = strstr(path, "junk") != NULL;
    if (!in)
        return 0;
    memcpy(in, bad ? (const void *)junk : test_png4, bad ? sizeof(junk) : sizeof(test_png4));
    return bad ? sizeof(junk) : sizeof(test_png4);
}

/* Asked for small, then the thread's one job done: whether there was one. */
static bool ask_and_work(uint64_t h, const char *p)
{
    (void)cover_ready(h, p, COVER_SMALL, false);
    return cover_test_work();
}

static void pause_ms(uint64_t ms)
{
    jam_nanosleep(now() + ms * NS_PER_MS);
}

/* cover.c's second try, with the real helper: a decode that timed out
 * (--hang) is tried again once, not before the retry time; a second
 * timeout in a row, a crash or a refused picture is no cover at once. */
static void test_retry(void)
{
    work_all();   /* what test_states left asked for, with its one-colour pictures */
    cover_test_retry(retry_pic, 200 * NS_PER_MS);
    decoder_test("--hang", 300 * NS_PER_MS);
    bool ok = ask_and_work(0xd, "/m/D/Album/d.mp3") && !cover_test_none(0xd);
    ok &= !cover_test_work();   /* the second try is not due yet */
    decoder_test(NULL, 0);
    pause_ms(250);
    ok &= cover_test_work() && cover_ready(0xd, "/m/D/Album/d.mp3", COVER_SMALL, false) ==
                                   COVER_SMALL_KIND;
    fun_check(ok, "  ... a decode that timed out: tried again once, later, and kept");
    decoder_test("--hang", 300 * NS_PER_MS);
    ok = ask_and_work(0xe, "/m/E/Album/e.mp3") && !cover_test_none(0xe);
    pause_ms(250);
    ok &= cover_test_work() && cover_test_none(0xe);
    pause_ms(250);
    ok &= !cover_test_work();   /* no third try */
    fun_check(ok, "  ... timed out twice in a row: no cover, no third try");
    decoder_test("--crash", 0);
    ok = ask_and_work(0xf, "/m/F/Album/f.mp3") && cover_test_none(0xf);
    decoder_test(NULL, 0);
    ok &= ask_and_work(0x10, "/m/G/Album/junk.mp3") && cover_test_none(0x10);
    pause_ms(250);
    ok &= !cover_test_work();
    fun_check(ok, "  ... a crash or a refused picture: no cover at once, no second try");
    cover_test_retry(NULL, 0);
}

void test_covers(void)
{
    test_id3();
    test_helper();
    test_states();
    test_retry();
}
