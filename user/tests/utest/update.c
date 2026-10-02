/* utest: `update`'s parsers, over hostile input: the manifest (<update.h>)
 * and the protocol's datagrams (<updwire.h>); and the manifest's
 * signature (Monocypher's Ed25519 as built here, against RFC 8032's
 * vectors and a manifest tools/jamos-sign signed). The golden bytes are what
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
 * --version 0.0.29-m9 --git abcdef0-dirty --net vlan21. */
static const char golden[] =
    "jamos-update 2\n"
    "version 0.0.29-m9\n"
    "git abcdef0-dirty\n"
    "net vlan21\n"
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

/* Must-understand lines ('!'): the manifest parses (the signature is
 * still to be checked), and needs names the first one, which init then
 * refuses; another format is refused at once, named the same way. */
static bool manifest_must_understand(void)
{
    struct update_manifest m;
    CHECK_ST(edited("net vlan21\n", "net vlan21\n!new-boot-rule yes\nnote 1\n!other\n", &m), OK);
    CHECK(!strcmp(m.needs, "!new-boot-rule"));
    CHECK(!strcmp(m.net, "vlan21") && m.file[UPDATE_BOOTFS].size == 5);
    CHECK_ST(edited("signature\n", "!k234567890123456789012345678901\nsignature\n", &m), OK);
    CHECK_EQ(strlen(m.needs), UPDATE_EXT_KEY_MAX);
    static const char *const bad[] = {
        "!\n", "!Up 1\n", "!!x\n", "! x\n", "!k2345678901234567890123456789012\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char with[64];
        snprintf(with, sizeof(with), "%ssignature\n", bad[i]);
        if (edited("signature\n", with, &m) != ERR_INVALID_ARGS)
            FAIL("the must-understand line %u (\"%s\") wasn't refused", i, bad[i]);
    }
    memset(&m, 0, sizeof(m));
    CHECK_ST(edited("jamos-update 2", "jamos-update 3", &m), ERR_NOT_SUPPORTED);
    CHECK(!strcmp(m.needs, "jamos-update 3"));
    return true;
}

/* Extension lines (a later build's): skipped wherever format 2 allows
 * them, and still inside what the signature covers; the format's own
 * lines read as before. */
static bool manifest_extensions(void)
{
    struct update_manifest m;
    CHECK_ST(edited("jamos-update 2\n", "jamos-update 2\nchannel nightly builds\n", &m), OK);
    CHECK(!strcmp(m.version, "0.0.29-m9"));
    CHECK_ST(edited("signature\n", "min-ram 2048\nreboot\nsignature\n", &m), OK);
    CHECK_EQ(m.signed_len,
             strlen(golden) - strlen("signature\n") + strlen("min-ram 2048\nreboot\n"));
    CHECK_EQ(m.file[UPDATE_BOOTFS].size, 5);
    static const char every[] =
        "jamos-update 2\na 1\nversion 0.0.29-m9\nb ~!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}\n"
        "git abcdef0-dirty\nc\nnet vlan21\nd x y z\n"
        "kernel 10 e444dff1ba68a27e327484b63b7da2c32a32fc7a52a4a0587fcb10dbfdac1b44\ne-1 0\n"
        "bootfs 5 5e846c64f2db12266e6b658a8e5b5b42cc225419b3ee1fca88acbb181ddfdb52\nf 9\n"
        "signature\n";
    CHECK_ST(update_manifest_parse(every, strlen(every), &m), OK);
    CHECK(!strcmp(m.git, "abcdef0-dirty") && !strcmp(m.net, "vlan21"));
    CHECK_EQ(m.file[UPDATE_KERNEL].size, 10);
    CHECK_EQ(m.signed_len, strlen(every) - strlen("signature\n"));
    /* the longest key and value */
    char line[UPDATE_EXT_KEY_MAX + UPDATE_EXT_MAX + 3];
    memset(line, 'k', UPDATE_EXT_KEY_MAX);
    line[UPDATE_EXT_KEY_MAX] = ' ';
    memset(line + UPDATE_EXT_KEY_MAX + 1, 'v', UPDATE_EXT_MAX);
    memcpy(line + UPDATE_EXT_KEY_MAX + 1 + UPDATE_EXT_MAX, "\n", 2);
    char with[sizeof(line) + 16];
    snprintf(with, sizeof(with), "%ssignature\n", line);
    CHECK_ST(edited("signature\n", with, &m), OK);
    snprintf(with, sizeof(with), "%.*sv\nsignature\n", (int)(sizeof(line) - 2), line);
    CHECK_ST(edited("signature\n", with, &m), ERR_INVALID_ARGS);   /* a value a byte longer */
    CHECK(!m.needs[0]);
    return manifest_must_understand();
}

bool t_update_manifest(void)
{
    struct update_manifest m;
    CHECK_ST(update_manifest_parse(golden, strlen(golden), &m), OK);
    CHECK(!strcmp(m.version, "0.0.29-m9") && !strcmp(m.git, "abcdef0-dirty"));
    CHECK(!strcmp(m.net, "vlan21"));
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
    CHECK_ST(edited("net vlan21", "net untagged", &m), OK);
    CHECK(!strcmp(m.net, "untagged"));
    CHECK_ST(edited("net vlan21", "net vlan4094", &m), OK);
    CHECK_ST(edited("net vlan21", "net vlan1", &m), OK);
    CHECK(!strcmp(m.net, "vlan1"));
    return manifest_extensions();
}

/* Each line wrong in each way: refused, with the status that says why. */
bool t_update_manifest_refusals(void)
{
    static const struct { const char *from, *to; status_t want; } cases[] = {
        { "jamos-update 2", "jamos-update 1", ERR_NOT_SUPPORTED },   /* no net line */
        { "jamos-update 2", "jamos-update 3", ERR_NOT_SUPPORTED },
        { "jamos-update 2", "jamos-update 12", ERR_NOT_SUPPORTED },
        { "jamos-update 2", "jamos-update x", ERR_INVALID_ARGS },
        { "jamos-update 2", "jamos-update ", ERR_INVALID_ARGS },
        { "jamos-update 2", "Jamos-update 2", ERR_INVALID_ARGS },
        { "net vlan21\n", "", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan0", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan021", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan4095", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan40940", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan", ERR_INVALID_ARGS },
        { "net vlan21", "net 21", ERR_INVALID_ARGS },
        { "net vlan21", "net VLAN21", ERR_INVALID_ARGS },
        { "net vlan21", "net none", ERR_INVALID_ARGS },
        { "net vlan21", "net off", ERR_INVALID_ARGS },
        { "net vlan21", "net untaggedx", ERR_INVALID_ARGS },
        { "net vlan21", "net  vlan21", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan21 ", ERR_INVALID_ARGS },
        { "net vlan21", "net vlan-21", ERR_INVALID_ARGS },
        { "net vlan21\n", "net vlan21\nnet vlan21\n", ERR_INVALID_ARGS },
        { "git abcdef0-dirty\nnet vlan21\n", "net vlan21\ngit abcdef0-dirty\n",
          ERR_INVALID_ARGS },                                         /* the order */
        { "signature\n", "signature 00ff\n", ERR_INVALID_ARGS },   /* not 128 digits */
        { "signature\n", "signature \n", ERR_INVALID_ARGS },
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
        /* extension lines: their own rules */
        { "signature\n", "Future 1\nsignature\n", ERR_INVALID_ARGS },   /* upper case */
        { "signature\n", "fu_ture 1\nsignature\n", ERR_INVALID_ARGS },
        { "signature\n", "future \nsignature\n", ERR_INVALID_ARGS },    /* an empty value */
        { "signature\n", "future\t1\nsignature\n", ERR_INVALID_ARGS },
        { "signature\n", "future 1\t2\nsignature\n", ERR_INVALID_ARGS },
        { "signature\n", "future 1\x7f\nsignature\n", ERR_INVALID_ARGS },
        { "signature\n", "future 1\r\nsignature\n", ERR_INVALID_ARGS },
        { "signature\n", "future 1", ERR_INVALID_ARGS },                 /* no signature */
        { "signature\n", "signature\nfuture 1\n", ERR_INVALID_ARGS },   /* after it */
        { "signature\n", "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk 1\nsignature\n",
          ERR_INVALID_ARGS },                                            /* a 33-byte key */
        { "git abcdef0", "version 1\ngit abcdef0", ERR_INVALID_ARGS },  /* a line twice */
        { "kernel 10", "signature\nkernel 10", ERR_INVALID_ARGS },
        { "net vlan21\n", "net vlan21\njamos-update 2\n", ERR_INVALID_ARGS },
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

/* golden, signed by `jamos-sign sign` with the secret key whose seed is the
 * bytes 00 01 .. 1f (its public half: golden_key). */
static const char golden_signed[] =
    "jamos-update 2\n"
    "version 0.0.29-m9\n"
    "git abcdef0-dirty\n"
    "net vlan21\n"
    "kernel 10 e444dff1ba68a27e327484b63b7da2c32a32fc7a52a4a0587fcb10dbfdac1b44\n"
    "bootfs 5 5e846c64f2db12266e6b658a8e5b5b42cc225419b3ee1fca88acbb181ddfdb52\n"
    "signature eacb5680b899f929e6214968464c15c768382478a86ce619e380ad8ccf44bf7a"
    "d8f2c58435434e78e21e6859f77fd64276d78e734596ab21696d6e238969b609\n";
static const char golden_key[] =
    "ed25519 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n";

static unsigned nibble(char c)
{
    return (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
}

/* n bytes of lower-case hex into out. */
static void unhex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(nibble(h[2 * i]) << 4 | nibble(h[2 * i + 1]));
}

/* RFC 8032's Ed25519 tests 1-3 (section 7.1; also in Monocypher's own
 * vectors, third_party/monocypher/tests/vectors-ed25519.h): Monocypher as
 * built for Jam OS checks what the standard says. */
static bool rfc8032(void)
{
    static const struct { const char *key, *msg, *sig; } t[] = {
        { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
          "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        struct update_manifest m = { .has_signature = true, .signed_len = strlen(t[i].msg) / 2 };
        uint8_t key[UPDATE_KEY_BYTES], msg[2];
        unhex(t[i].key, key, sizeof(key));
        unhex(t[i].sig, m.signature, sizeof(m.signature));
        unhex(t[i].msg, msg, m.signed_len);
        CHECK_ST(update_manifest_verify(&m, msg, key), OK);
        m.signature[63] ^= 0x01;
        CHECK_ST(update_manifest_verify(&m, msg, key), ERR_ACCESS_DENIED);
    }
    return true;
}

/* The signature: the key's over exactly the bytes before its line, and
 * nothing else checks. */
bool t_update_signature(void)
{
    if (!rfc8032())
        return false;
    uint8_t key[UPDATE_KEY_BYTES], other[UPDATE_KEY_BYTES];
    CHECK_ST(update_key_parse(golden_key, strlen(golden_key), key), OK);
    struct update_manifest m;
    size_t len = strlen(golden_signed);
    CHECK_ST(update_manifest_parse(golden_signed, len, &m), OK);
    CHECK(m.has_signature && m.signed_len == strlen(golden) - strlen("signature\n"));
    CHECK_ST(update_manifest_verify(&m, golden_signed, key), OK);
    /* any byte it covers changed: refused (the sizes and hashes too) */
    char text[sizeof(golden_signed)];
    for (size_t i = 0; i < m.signed_len; i += 7) {
        memcpy(text, golden_signed, len);
        text[i] ^= 0x01;
        struct update_manifest t;
        if (update_manifest_parse(text, len, &t) == OK &&
            update_manifest_verify(&t, text, key) != ERR_ACCESS_DENIED)
            FAIL("byte %zu changed: the signature still checks", i);
    }
    /* another key, a changed signature, no signature at all */
    memcpy(other, key, sizeof(other));
    other[0] ^= 0x80;
    CHECK_ST(update_manifest_verify(&m, golden_signed, other), ERR_ACCESS_DENIED);
    struct update_manifest bent = m;
    bent.signature[10] ^= 0x04;
    CHECK_ST(update_manifest_verify(&bent, golden_signed, key), ERR_ACCESS_DENIED);
    CHECK_ST(update_manifest_parse(golden, strlen(golden), &m), OK);
    CHECK(!m.has_signature);
    CHECK_ST(update_manifest_verify(&m, golden, key), ERR_ACCESS_DENIED);
    /* the key file: exactly one line of 64 digits */
    static const char *const bad_keys[] = {
        "ed25519 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8",
        "ed25519 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b\n",
        "ed25519 03A107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n",
        "ed25519  03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n",
        "ed448 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n",
        "ed25519 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n\n", "",
    };
    for (unsigned i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); i++)
        CHECK_ST(update_key_parse(bad_keys[i], strlen(bad_keys[i]), other), ERR_INVALID_ARGS);
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

/* The network default line of a build.txt (init reads its own; the
 * update check compares it with the manifest's). */
bool t_update_build_net(void)
{
    char net[UPDATE_NET_MAX + 1];
    static const struct { const char *text; const char *want; } cases[] = {
        { "git 5e3d102\nnet vlan21\n", "vlan21" },
        { "git 5e3d102-dirty\nnet untagged\n", "untagged" },
        { "net vlan4094\ngit 5e3d102\n", "vlan4094" },
        { "git 5e3d102\n", NULL },                      /* a build from before */
        { "git 5e3d102\nnet vlan21", NULL },            /* no newline: no line */
        { "git 5e3d102\nnet vlan0\n", NULL },
        { "git 5e3d102\nnet none\n", NULL },
        { "git 5e3d102\nnet vlan21 \n", NULL },
        { "git 5e3d102\n net vlan21\n", NULL },
        { "git 5e3d102\nnetvlan21\n", NULL },
        { "", NULL },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(net, 'x', sizeof(net));
        status_t st = update_build_net(cases[i].text, strlen(cases[i].text), net);
        bool ok = cases[i].want ? st == OK && !strcmp(net, cases[i].want) : st == ERR_NOT_FOUND;
        if (!ok)
            FAIL("case %u: %s", i, status_str(st));
    }
    /* never read past len: the line cut by len is not a line */
    const char *t = "git 5e3d102\nnet vlan21\n";
    CHECK_ST(update_build_net(t, strlen(t) - 1, net), ERR_NOT_FOUND);
    CHECK_ST(update_build_net(t, strlen(t) - 3, net), ERR_NOT_FOUND);
    CHECK_ST(update_build_net(NULL, 0, net), ERR_NOT_FOUND);
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
          q.length == 1400 && q.format == 0);   /* an older build's: no format said */
    uint8_t out[UPDWIRE_REP_MAX];
    CHECK_ST(updwire_req_encode(&q, out), OK);
    CHECK(!memcmp(out, req, sizeof(req)));
    /* this build's: its manifest format in byte 7 */
    q.format = UPDATE_FORMAT;
    CHECK_ST(updwire_req_encode(&q, out), OK);
    CHECK(out[7] == UPDATE_FORMAT && !memcmp(out, req, 7) && !memcmp(out + 8, req + 8, 12));
    CHECK_ST(updwire_req_decode(out, UPDWIRE_REQ_SIZE, &q), OK);
    CHECK_EQ(q.format, UPDATE_FORMAT);
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
    CHECK(req_refused(good, 6, 3) && req_refused(good, 18, 1) && req_refused(good, 19, 1));
    memcpy(d, good, sizeof(good));
    d[7] = 0xff;   /* any format is a request: the server says what it can make */
    CHECK(updwire_req_decode(d, sizeof(good), &q) == OK && q.format == 0xff);
    CHECK(req_refused(good, 6, UPDWIRE_KERNEL));      /* a file needs a snapshot */
    CHECK(req_refused(good, 16, 0));                  /* length 0 */
    struct updwire_req bad[] = {
        { UPDWIRE_KERNEL, 0, 0, 10, 2 }, { UPDWIRE_FILES, 1, 0, 10, 2 },
        { UPDWIRE_KERNEL, 1, 0, 0, 2 }, { UPDWIRE_KERNEL, 1, 0, UPDWIRE_CHUNK_MAX + 1, 2 },
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
