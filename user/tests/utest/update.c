/* utest: `update`'s parsers, over hostile input: the manifest (<update.h>)
 * and the protocol's datagrams (<updwire.h>). The golden bytes are what
 * tools/update-server.py makes (its Python struct layouts), so the two
 * sides are held to one format. init's own check of an offer is
 * tools/update-test.sh's (bin/updtest); the fetcher's window is
 * updfetch.c's. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <update.h>
#include <updwire.h>
#include "utest.h"

/* tools/update-server.py --manifest over 10 bytes of 'k' and 5 of 'b',
 * --version 0.0.29-m9 --git abcdef0-dirty. */
static const char golden[] =
    "jamos-update 1\n"
    "version 0.0.29-m9\n"
    "git abcdef0-dirty\n"
    "kernel 10 e444dff1ba68a27e327484b63b7da2c32a32fc7a52a4a0587fcb10dbfdac1b44\n"
    "bootfs 5 5e846c64f2db12266e6b658a8e5b5b42cc225419b3ee1fca88acbb181ddfdb52\n"
    "signature\n";

/* golden with the first `from` replaced by `to`, parsed. */
static status_t edited(const char *from, const char *to, struct update_manifest *m)
{
    char text[UPDATE_MANIFEST_MAX + 64];
    const char *at = strstr(golden, from);
    if (!at)
        return ERR_INTERNAL;
    size_t pre = (size_t)(at - golden), fl = strlen(from), tl = strlen(to);
    memcpy(text, golden, pre);
    memcpy(text + pre, to, tl);
    memcpy(text + pre + tl, at + fl, strlen(at + fl) + 1);
    return update_manifest_parse(text, strlen(text), m);
}

bool t_update_manifest(void)
{
    struct update_manifest m;
    CHECK_ST(update_manifest_parse(golden, strlen(golden), &m), OK);
    CHECK(!strcmp(m.version, "0.0.29-m9") && !strcmp(m.git, "abcdef0-dirty"));
    CHECK_EQ(m.file[UPDATE_KERNEL].size, 10);
    CHECK_EQ(m.file[UPDATE_BOOTFS].size, 5);
    uint8_t want[SHA256_BYTES];
    sha256("kkkkkkkkkk", 10, want);
    CHECK(!memcmp(m.file[UPDATE_KERNEL].sha256, want, SHA256_BYTES));
    sha256("bbbbb", 5, want);
    CHECK(!memcmp(m.file[UPDATE_BOOTFS].sha256, want, SHA256_BYTES));
    CHECK_EQ(m.signed_len, strlen(golden) - strlen("signature\n"));
    /* what the format allows besides */
    CHECK_ST(edited("abcdef0-dirty", "0123456789abcdef0123456789abcdef01234567", &m), OK);
    CHECK_ST(edited("0.0.29-m9", "A.b_c+d-9", &m), OK);
    CHECK_ST(edited("kernel 10 ", "kernel 33554432 ", &m), OK);   /* UPDATE_FILE_MAX */
    return true;
}

/* Each line wrong in each way: refused, with the status that says why. */
bool t_update_manifest_refusals(void)
{
    static const struct { const char *from, *to; status_t want; } cases[] = {
        { "jamos-update 1", "jamos-update 2", ERR_NOT_SUPPORTED },
        { "jamos-update 1", "jamos-update 12", ERR_NOT_SUPPORTED },
        { "jamos-update 1", "jamos-update x", ERR_INVALID_ARGS },
        { "jamos-update 1", "jamos-update ", ERR_INVALID_ARGS },
        { "jamos-update 1", "Jamos-update 1", ERR_INVALID_ARGS },
        { "signature\n", "signature 00ff\n", ERR_NOT_SUPPORTED },
        { "signature\n", "signature \n", ERR_NOT_SUPPORTED },
        { "signature\n", "signature", ERR_INVALID_ARGS },
        { "signature\n", "signature\n\n", ERR_INVALID_ARGS },
        { "signature\n", "signature\nx", ERR_INVALID_ARGS },
        { "signature\n", "signatures\n", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel 0 ", ERR_OUT_OF_RANGE },
        { "kernel 10 ", "kernel 33554433 ", ERR_OUT_OF_RANGE },
        { "kernel 10 ", "kernel 9999999999 ", ERR_OUT_OF_RANGE },
        { "kernel 10 ", "kernel 99999999999 ", ERR_INVALID_ARGS },   /* 11 digits */
        { "kernel 10 ", "kernel 010 ", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel -10 ", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel +10 ", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel  10 ", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel 10  ", ERR_INVALID_ARGS },
        { "kernel 10 ", "kernel ", ERR_INVALID_ARGS },
        { "kernel 10 e444", "kernel 10 E444", ERR_INVALID_ARGS },
        { "kernel 10 e444", "kernel 10 g444", ERR_INVALID_ARGS },
        { "e444", "e44", ERR_INVALID_ARGS },                           /* 63 digits */
        { "e444", "e4444", ERR_INVALID_ARGS },                         /* 65 digits */
        { "kernel", "bootfs", ERR_INVALID_ARGS },                      /* the order */
        { "bootfs 5", "kernel 5", ERR_INVALID_ARGS },
        { "version 0.0.29-m9", "version 0.0.29 m9", ERR_INVALID_ARGS },
        { "version 0.0.29-m9", "version ", ERR_INVALID_ARGS },
        { "version 0.0.29-m9", "version 0.0.29/m9", ERR_INVALID_ARGS },
        { "version 0.0.29-m9", "version 012345678901234567890123456789012345678901234567",
          ERR_INVALID_ARGS },                                          /* 48 bytes */
        { "git abcdef0-dirty", "git abcdef", ERR_INVALID_ARGS },       /* 6 digits */
        { "git abcdef0-dirty", "git abcdef0-dirt", ERR_INVALID_ARGS },
        { "git abcdef0-dirty", "git abcdefg", ERR_INVALID_ARGS },
        { "git abcdef0-dirty", "git ABCDEF0", ERR_INVALID_ARGS },
        { "git abcdef0-dirty", "git 0123456789abcdef0123456789abcdef012345678", ERR_INVALID_ARGS },
        { "git abcdef0-dirty\n", "git abcdef0-dirty\r\n", ERR_INVALID_ARGS },
        { "git abcdef0-dirty\n", "\ngit abcdef0-dirty\n", ERR_INVALID_ARGS },
        { "version", "VERSION", ERR_INVALID_ARGS },
    };
    struct update_manifest m;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        status_t st = edited(cases[i].from, cases[i].to, &m);
        if (st != cases[i].want)
            FAIL("case %u (\"%s\" -> \"%s\"): %s, want %s", i, cases[i].from, cases[i].to,
                 status_str(st), status_str(cases[i].want));
    }
    return true;
}

/* Every prefix of the manifest, a NUL anywhere, every byte changed: never
 * read past len, never taken unless the change is inside a hash or a
 * size (which the check against the file then catches). */
bool t_update_manifest_damage(void)
{
    size_t len = strlen(golden);
    struct update_manifest m;
    CHECK_ST(update_manifest_parse(NULL, 0, &m), ERR_INVALID_ARGS);
    CHECK_ST(update_manifest_parse(golden, UPDATE_MANIFEST_MAX + 1, &m), ERR_INVALID_ARGS);
    /* each prefix, from a buffer whose next byte would complete it */
    for (size_t n = 0; n < len; n++)
        if (update_manifest_parse(golden, n, &m) == OK)
            FAIL("a prefix of %zu bytes parsed", n);
    char text[sizeof(golden)];
    for (size_t i = 0; i < len; i++) {
        static const char swaps[] = { '\0', ' ', '\n', 'x', '0', '9', 'a', '-', '\r' };
        /* the line's key ("kernel"), its spaces and its newline: no change there parses */
        size_t start = i;
        while (start && golden[start - 1] != '\n')
            start--;
        bool structure = golden[i] == ' ' || golden[i] == '\n' || !strchr(golden + start, ' ') ||
                         i <= (size_t)(strchr(golden + start, ' ') - golden) ||
                         !strncmp(golden + start, "jamos-update", 12) ||
                         !strncmp(golden + start, "signature", 9);
        for (unsigned s = 0; s < sizeof(swaps); s++) {
            if (golden[i] == swaps[s])
                continue;
            memcpy(text, golden, len);
            text[i] = swaps[s];
            if (update_manifest_parse(text, len, &m) == OK && structure)
                FAIL("byte %zu changed to %02x parsed", i, (unsigned)(uint8_t)swaps[s]);
        }
    }
    /* random bytes: never taken */
    uint32_t x = 99;
    for (unsigned round = 0; round < 200; round++) {
        for (size_t i = 0; i < len; i++) {
            x = x * 1664525u + 1013904223u;
            text[i] = (char)(x >> 24);
        }
        CHECK(update_manifest_parse(text, len, &m) != OK);
    }
    return true;
}

bool t_updwire_golden(void)
{
    static const uint8_t req[] = {
        0x4a, 0x55, 0x50, 0x44, 0x01, 0x01, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01, 0x0d, 0x0c,
        0x0b, 0x0a, 0x78, 0x05, 0x00, 0x00,
    };
    static const uint8_t rep[] = {
        0x4a, 0x55, 0x50, 0x44, 0x01, 0x02, 0x02, 0x00, 0x04, 0x03, 0x02, 0x01, 0x10, 0x00,
        0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x61, 0x62, 0x63,
    };
    struct updwire_req q;
    CHECK_ST(updwire_req_decode(req, sizeof(req), &q), OK);
    CHECK(q.file == UPDWIRE_KERNEL && q.snapshot == 0x01020304 && q.offset == 0x0a0b0c0d &&
          q.length == 1400);
    uint8_t out[UPDWIRE_REP_MAX];
    CHECK_ST(updwire_req_encode(&q, out), OK);
    CHECK(!memcmp(out, req, sizeof(req)));
    struct updwire_rep r;
    CHECK_ST(updwire_rep_decode(rep, sizeof(rep), &r), OK);
    CHECK(r.file == UPDWIRE_BOOTFS && r.status == UPDWIRE_OK && r.snapshot == 0x01020304 &&
          r.offset == 16 && r.file_size == 19 && r.length == 3 && !memcmp(r.data, "abc", 3));
    size_t n = 0;
    CHECK_ST(updwire_rep_encode(&r, out, sizeof(out), &n), OK);
    CHECK(n == sizeof(rep) && !memcmp(out, rep, n));
    CHECK_ST(updwire_rep_encode(&r, out, n - 1, &n), ERR_BUFFER_TOO_SMALL);
    return true;
}

/* A request with one byte at `at` set to v: refused. */
static bool req_refused(const uint8_t *good, size_t at, uint8_t v)
{
    uint8_t d[UPDWIRE_REQ_SIZE];
    struct updwire_req q;
    memcpy(d, good, sizeof(d));
    d[at] = v;
    return updwire_req_decode(d, sizeof(d), &q) == ERR_INVALID_ARGS;
}

bool t_updwire_hostile(void)
{
    struct updwire_req q = { .file = UPDWIRE_MANIFEST, .snapshot = 0, .offset = 0, .length = 1 };
    uint8_t good[UPDWIRE_REQ_SIZE], d[UPDWIRE_REP_MAX + 8];
    CHECK_ST(updwire_req_encode(&q, good), OK);   /* the manifest may ask for snapshot 0 */
    for (size_t n = 0; n <= sizeof(good) + 1; n++)
        CHECK(updwire_req_decode(good, n, &q) == (n == sizeof(good) ? OK : ERR_INVALID_ARGS));
    CHECK(req_refused(good, 0, 0x4b) && req_refused(good, 4, 2) && req_refused(good, 5, 2));
    CHECK(req_refused(good, 6, 3) && req_refused(good, 7, 1) && req_refused(good, 19, 1));
    CHECK(req_refused(good, 6, UPDWIRE_KERNEL));      /* a file needs a snapshot */
    CHECK(req_refused(good, 16, 0));                  /* length 0 */
    struct updwire_req bad[] = {
        { UPDWIRE_KERNEL, 0, 0, 10 }, { UPDWIRE_FILES, 1, 0, 10 }, { UPDWIRE_KERNEL, 1, 0, 0 },
        { UPDWIRE_KERNEL, 1, 0, UPDWIRE_CHUNK_MAX + 1 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK_ST(updwire_req_encode(&bad[i], d), ERR_INVALID_ARGS);
    /* replies: the rules of rep_ok, from both ends */
    static const uint8_t bytes[UPDWIRE_CHUNK_MAX + 1];
    struct updwire_rep r[] = {
        { UPDWIRE_KERNEL, UPDWIRE_OK, 0, 0, 10, 10, bytes },             /* OK, snapshot 0 */
        { UPDWIRE_KERNEL, UPDWIRE_OK, 1, 5, 10, 6, bytes },              /* past the end */
        { UPDWIRE_KERNEL, UPDWIRE_OK, 1, 11, 10, 0, bytes },             /* offset past */
        { UPDWIRE_KERNEL, UPDWIRE_OK, 1, 0xffffffffu, 0xffffffffu, 1, bytes },
        { UPDWIRE_KERNEL, UPDWIRE_GONE, 1, 0, 10, 1, bytes },            /* bytes with GONE */
        { UPDWIRE_KERNEL, UPDWIRE_STATUSES, 1, 0, 10, 0, bytes },
        { UPDWIRE_FILES, UPDWIRE_OK, 1, 0, 10, 1, bytes },
        { UPDWIRE_KERNEL, UPDWIRE_OK, 1, 0, 0xffffu, UPDWIRE_CHUNK_MAX + 1, bytes },
    };
    size_t n = 0;
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
        CHECK_ST(updwire_rep_encode(&r[i], d, sizeof(d), &n), ERR_INVALID_ARGS);
    struct updwire_rep ok = { UPDWIRE_KERNEL, UPDWIRE_OK, 7, 0, 3000, UPDWIRE_CHUNK_MAX, bytes };
    CHECK_ST(updwire_rep_encode(&ok, d, sizeof(d), &n), OK);
    struct updwire_rep got;
    for (size_t len = 0; len <= n + 1; len++)
        CHECK(updwire_rep_decode(d, len, &got) == (len == n ? OK : ERR_INVALID_ARGS));
    d[20] = 0x79;   /* length 1401: as long as the datagram, but over the cap */
    d[21] = 0x05;
    CHECK_ST(updwire_rep_decode(d, n + 1, &got), ERR_INVALID_ARGS);
    /* random datagrams of every size: never taken unless they happen to fit the rules */
    uint32_t x = 7;
    unsigned taken = 0;
    for (unsigned round = 0; round < 2000; round++) {
        size_t len = round % (UPDWIRE_REP_MAX + 4);
        for (size_t i = 0; i < len; i++) {
            x = x * 1664525u + 1013904223u;
            d[i] = (uint8_t)(x >> 24);
        }
        taken += updwire_rep_decode(d, len, &got) == OK;
        taken += updwire_req_decode(d, len, &q) == OK;
    }
    CHECK_EQ(taken, 0);
    return true;
}
