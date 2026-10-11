/* jamos-sign: the Mac's half of signed updates (<update.h>) and of signed
 * packages (docs/M13.7-PLAN.md, "PO3 notes"): the keys, made once, and the
 * signature of each manifest tools/update-server.py serves and of each
 * package manifest, index and vulnerability list tools/ports.py makes.
 * Built by the Makefile with the Mac's own compiler (build/host/jamos-sign)
 * from the same Monocypher the PC checks with (third_party/monocypher):
 * Ed25519 as RFC 8032 has it (EdDSA over edwards25519 with SHA-512,
 * Monocypher's optional crypto_ed25519_*), so a signature made here is
 * checked there with the very same code.
 *
 *   jamos-sign keygen [--ports] [DIR]
 *                               DIR/update.key (the secret, mode 0600) and
 *                               DIR/update.pub (default DIR: ~/.config/jamos);
 *                               with --ports, the ports key: DIR/ports.key
 *                               and DIR/ports.pub; refuses if either file
 *                               is there already
 *   jamos-sign pub KEY          the public key file's line for secret KEY
 *   jamos-sign sign KEY [FORMAT]
 *                               a manifest on stdin, its last line
 *                               "signature": out with the line filled in
 *   jamos-sign verify PUB [FORMAT]
 *                               a signed manifest on stdin: exit 0 if PUB's
 *                               key signed exactly the bytes before its
 *                               signature line, 1 if not
 *   jamos-sign self-test        Monocypher's Ed25519 test vectors (its
 *                               tests/vectors.h tables, third_party/
 *                               monocypher/tests/vectors-ed25519.h), a
 *                               round trip with a fresh key and the
 *                               formats' domain separation; exit 0 on PASS
 *
 * The files, one line each, lower-case hex (the ports key's are the same
 * lines, so the PC reads ports.pub with update.pub's parser):
 *   update.key   "ed25519-secret <64 hex: the 32-byte seed>\n"
 *   update.pub   "ed25519 <64 hex: the 32-byte public key>\n"
 * The signature line: "signature <128 hex: the 64-byte signature>\n", over
 * every byte of the manifest before that line.
 *
 * FORMAT names one of the ports formats by its first line's word:
 * jamos-pkg (a package's +MANIFEST), jamos-ports (the repository's INDEX)
 * or jamos-vulns (its VULNS). With it, the text's first line must be
 * exactly that format's ("jamos-pkg 1"), and the text may be as long as
 * the format allows; without it, the text is an update manifest (at most
 * UPDATE_MANIFEST_MAX bytes, any first line). The first line is signed
 * with the rest, and every reader demands its own format's, so a
 * signature made for one format never verifies as another's, whichever
 * key made it: that line is the domain separation. The update key and the
 * ports key are separate keys besides (docs/M13.7-PLAN.md section 7). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "../third_party/monocypher/tests/vectors-ed25519.h"

#define MANIFEST_MAX 1024   /* <update.h> UPDATE_MANIFEST_MAX */
#define SIG_LINE     "signature"
#define KEY_PREFIX   "ed25519-secret "
#define PUB_PREFIX   "ed25519 "

/* The ports formats (tools/portslib/pkgformat.py has the same words and
 * limits): the word a FORMAT argument gives, the first line it demands,
 * and the most bytes a signed text of it may have. */
static const struct format {
    const char *word;    /* the FORMAT argument */
    const char *first;   /* the text's first line, its '\n' included */
    size_t      max;     /* bytes of the signed text, its signature line included */
} formats[] = {
    { "jamos-pkg", "jamos-pkg 1\n", 4u << 20 },
    { "jamos-ports", "jamos-ports 1\n", 1u << 20 },
    { "jamos-vulns", "jamos-vulns 1\n", 1u << 20 },
};
#define NFORMATS (sizeof(formats) / sizeof(formats[0]))

/* An update manifest: no first line demanded, UPDATE_MANIFEST_MAX bytes. */
static const struct format update_format = { "update", NULL, MANIFEST_MAX };

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static int nibble(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Exactly 2n lower-case hex digits at s into out. */
static int unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = nibble(s[2 * i]), lo = hi < 0 ? -1 : nibble(s[2 * i + 1]);
        if (lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* A one-line file "<prefix><2n hex>\n" into out. */
static int read_line_file(const char *path, const char *prefix, uint8_t *out, size_t n)
{
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "jamos-sign: %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';
    size_t p = strlen(prefix);
    if (len != p + 2 * n + 1 || memcmp(buf, prefix, p) || buf[len - 1] != '\n' ||
        unhex(buf + p, out, n)) {
        fprintf(stderr, "jamos-sign: %s is not a \"%s<%zu hex digits>\" line\n", path, prefix,
                2 * n);
        return -1;
    }
    return 0;
}

/* The secret key file's seed, expanded into Monocypher's 64-byte secret
 * key (seed, then the public key) and the public key. */
static int load_secret(const char *path, uint8_t sk[64], uint8_t pk[32])
{
    uint8_t seed[32];
    if (read_line_file(path, KEY_PREFIX, seed, 32))
        return -1;
    crypto_ed25519_key_pair(sk, pk, seed);   /* wipes seed */
    return 0;
}

static int write_file(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        fprintf(stderr, "jamos-sign: %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && fsync(fd) == 0;
    ok &= close(fd) == 0;
    if (!ok)
        fprintf(stderr, "jamos-sign: %s: can't write it\n", path);
    return ok ? 0 : -1;
}

/* A new key pair in dir: <name>.key and <name>.pub ("update" or "ports"). */
static int keygen(const char *dir, const char *name)
{
    char key[1024], pub[1024];
    char line[256], h[129];
    if (snprintf(key, sizeof(key), "%s/%s.key", dir, name) >= (int)sizeof(key) ||
        snprintf(pub, sizeof(pub), "%s/%s.pub", dir, name) >= (int)sizeof(pub))
        return 2;
    if (!access(key, F_OK) || !access(pub, F_OK)) {
        fprintf(stderr, "jamos-sign: %s or %s exists already: refusing to make another key "
                "(builds and signatures made with the old one would stop matching)\n", key, pub);
        return 1;
    }
    if (mkdir(dir, 0700) && errno != EEXIST) {
        fprintf(stderr, "jamos-sign: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    uint8_t seed[32], sk[64], pk[32];
    if (getentropy(seed, sizeof(seed))) {
        fprintf(stderr, "jamos-sign: no random bytes: %s\n", strerror(errno));
        return 1;
    }
    hex(seed, 32, h);
    snprintf(line, sizeof(line), KEY_PREFIX "%s\n", h);
    crypto_ed25519_key_pair(sk, pk, seed);
    crypto_wipe(h, sizeof(h));
    crypto_wipe(sk, sizeof(sk));
    int st = write_file(key, line, 0600);
    crypto_wipe(line, sizeof(line));
    hex(pk, 32, h);
    snprintf(line, sizeof(line), PUB_PREFIX "%s\n", h);
    if (st || write_file(pub, line, 0644))
        return 1;
    printf("jamos-sign: made %s (the secret: keep it, back it up, never share it) and %s\n"
           "  (the public half: `make` builds it into the image)\n", key, pub);
    return 0;
}

/* The format a FORMAT argument names, or NULL (and a message). */
static const struct format *format_named(const char *word)
{
    for (size_t i = 0; i < NFORMATS; i++)
        if (!strcmp(word, formats[i].word))
            return &formats[i];
    fprintf(stderr, "jamos-sign: no format \"%s\" (jamos-pkg, jamos-ports or jamos-vulns)\n",
            word);
    return NULL;
}

/* A whole text from stdin, at most fmt->max bytes: a malloc'd buffer (the
 * caller frees it) and its length, or NULL. */
static char *read_text(const struct format *fmt, size_t *len)
{
    char *buf = malloc(fmt->max + 1);
    if (!buf) {
        fprintf(stderr, "jamos-sign: out of memory\n");
        return NULL;
    }
    size_t n = fread(buf, 1, fmt->max + 1, stdin);
    if (n > fmt->max) {
        fprintf(stderr, "jamos-sign: a %s text is at most %zu bytes\n",
                fmt->first ? fmt->word : "manifest", fmt->max);
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

/* Where the text's last line starts (the bytes signed end there). */
static size_t last_line(const char *buf, size_t len)
{
    size_t at = len ? len - 1 : 0;   /* the last line's own '\n' */
    while (at > 0 && buf[at - 1] != '\n')
        at--;
    return at;
}

/* Whether the text starts with the format's first line (any text, for an
 * update manifest). Says why not. */
static int first_line_ok(const struct format *fmt, const char *buf, size_t len, int quiet)
{
    if (!fmt->first)
        return 1;
    size_t n = strlen(fmt->first);
    if (len >= n && !memcmp(buf, fmt->first, n))
        return 1;
    if (!quiet)
        fprintf(stderr, "jamos-sign: not a %s text: its first line must be \"%.*s\"\n", fmt->word,
                (int)(n - 1), fmt->first);
    return 0;
}

/* The text buf[0..len) with its bare "signature" last line, signed with
 * sk: the signed text written to out (2 * 64 hex digits and a space more
 * than len). 0, or -1 with a message (the first line's, not when quiet). */
static int sign_text(const struct format *fmt, const char *buf, size_t len, const uint8_t sk[64],
                     FILE *out, int quiet)
{
    char h[129];
    uint8_t sig[64];
    size_t at = last_line(buf, len);
    if (!first_line_ok(fmt, buf, len, quiet))
        return -1;
    if (len - at != sizeof(SIG_LINE) || memcmp(buf + at, SIG_LINE "\n", sizeof(SIG_LINE))) {
        fprintf(stderr, "jamos-sign: the manifest's last line must be \"" SIG_LINE "\"\n");
        return -1;
    }
    if (len + 1 + 128 > fmt->max) {
        fprintf(stderr, "jamos-sign: the signed text would be over %zu bytes\n", fmt->max);
        return -1;
    }
    crypto_ed25519_sign(sig, sk, (const uint8_t *)buf, at);
    hex(sig, 64, h);
    fwrite(buf, 1, at, out);
    fprintf(out, SIG_LINE " %s\n", h);
    return 0;
}

/* 0 if pk signed exactly the bytes of buf[0..len) before its signature
 * line, and the text is fmt's; -1 (with a message) if not. */
static int check_text(const struct format *fmt, const char *buf, size_t len, const uint8_t pk[32],
                      int quiet)
{
    uint8_t sig[64];
    size_t at = last_line(buf, len), p = sizeof(SIG_LINE);   /* "signature " */
    if (!first_line_ok(fmt, buf, len, quiet))
        return -1;
    if (len - at != p + 128 + 1 || memcmp(buf + at, SIG_LINE " ", p) || buf[len - 1] != '\n' ||
        unhex(buf + at + p, sig, 64)) {
        if (!quiet)
            fprintf(stderr, "jamos-sign: no \"" SIG_LINE " <128 hex digits>\" last line\n");
        return -1;
    }
    if (crypto_ed25519_check(sig, pk, (const uint8_t *)buf, at)) {
        if (!quiet)
            fprintf(stderr, "jamos-sign: the signature doesn't match: not this key's, or the "
                    "text changed\n");
        return -1;
    }
    return 0;
}

static int sign(const char *keyfile, const struct format *fmt)
{
    size_t len;
    uint8_t sk[64], pk[32];
    char *buf = read_text(fmt, &len);
    if (!buf)
        return 1;
    int st = load_secret(keyfile, sk, pk);
    if (!st)
        st = sign_text(fmt, buf, len, sk, stdout, 0);
    crypto_wipe(sk, sizeof(sk));
    free(buf);
    return st || fflush(stdout) ? 1 : 0;
}

static int verify(const char *pubfile, const struct format *fmt)
{
    size_t len;
    uint8_t pk[32];
    char *buf = read_text(fmt, &len);
    if (!buf)
        return 1;
    int st = read_line_file(pubfile, PUB_PREFIX, pk, 32);
    if (!st)
        st = check_text(fmt, buf, len, pk, 0);
    free(buf);
    if (st)
        return 1;
    printf("jamos-sign: signed by this key\n");
    return 0;
}

/* ---- the self-test: Monocypher's vectors ------------------------------------------ */

/* A vector's string as bytes (into a buffer of cap bytes): its length, or
 * -1 if it is no hex or too long. */
static long vec(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap || unhex(s, out, n / 2))
        return -1;
    return (long)(n / 2);
}

/* ed_25519_vectors: secret seed, public key, message, signature. */
static unsigned sign_vectors(void)
{
    unsigned bad = 0;
    static uint8_t msg[4096];
    for (size_t i = 0; i + 4 <= nb_ed_25519_vectors; i += 4) {
        uint8_t seed[32], pk[32], want[64], sk[64], pk2[32], sig[64];
        long n = vec(ed_25519_vectors[i + 2], msg, sizeof(msg));
        if (vec(ed_25519_vectors[i], seed, 32) != 32 ||
            vec(ed_25519_vectors[i + 1], pk, 32) != 32 || n < 0 ||
            vec(ed_25519_vectors[i + 3], want, 64) != 64) {
            bad++;
            continue;
        }
        crypto_ed25519_key_pair(sk, pk2, seed);
        crypto_ed25519_sign(sig, sk, msg, (size_t)n);
        bad += memcmp(pk2, pk, 32) || memcmp(sig, want, 64) ||
               crypto_ed25519_check(sig, pk, msg, (size_t)n) != 0;
    }
    return bad;
}

/* ed_25519_pk_vectors: seed, public key. */
static unsigned pk_vectors(void)
{
    unsigned bad = 0;
    for (size_t i = 0; i + 2 <= nb_ed_25519_pk_vectors; i += 2) {
        uint8_t seed[32], want[32], sk[64], pk[32];
        if (vec(ed_25519_pk_vectors[i], seed, 32) != 32 ||
            vec(ed_25519_pk_vectors[i + 1], want, 32) != 32) {
            bad++;
            continue;
        }
        crypto_ed25519_key_pair(sk, pk, seed);
        bad += memcmp(pk, want, 32) != 0;
    }
    return bad;
}

/* ed_25519_check_vectors: public key, message, signature, "00" (good) or
 * "ff" (refused): the edge cases (non-canonical S, small-order points). */
static unsigned check_vectors(unsigned *refusals)
{
    unsigned bad = 0;
    static uint8_t msg[4096];
    for (size_t i = 0; i + 4 <= nb_ed_25519_check_vectors; i += 4) {
        uint8_t pk[32], sig[64], want[1];
        long n = vec(ed_25519_check_vectors[i + 1], msg, sizeof(msg));
        if (vec(ed_25519_check_vectors[i], pk, 32) != 32 || n < 0 ||
            vec(ed_25519_check_vectors[i + 2], sig, 64) != 64 ||
            vec(ed_25519_check_vectors[i + 3], want, 1) != 1) {
            bad++;
            continue;
        }
        uint8_t got = (uint8_t)crypto_ed25519_check(sig, pk, msg, (size_t)n);
        *refusals += got != 0;
        bad += got != want[0];
    }
    return bad;
}

/* A fresh key signs a manifest; the signature checks; one changed byte of
 * the manifest, of the signature or of the key and it doesn't. */
static unsigned round_trip(void)
{
    static const char m[] = "jamos-update 1\nversion 0.0.29-test\ngit abcdef0\n";
    uint8_t seed[32], sk[64], pk[32], sig[64], m2[sizeof(m)];
    if (getentropy(seed, sizeof(seed)))
        return 1;
    crypto_ed25519_key_pair(sk, pk, seed);
    crypto_ed25519_sign(sig, sk, (const uint8_t *)m, sizeof(m) - 1);
    unsigned bad = crypto_ed25519_check(sig, pk, (const uint8_t *)m, sizeof(m) - 1) != 0;
    memcpy(m2, m, sizeof(m));
    m2[20] ^= 1;
    bad += crypto_ed25519_check(sig, pk, m2, sizeof(m) - 1) == 0;
    sig[5] ^= 0x40;
    bad += crypto_ed25519_check(sig, pk, (const uint8_t *)m, sizeof(m) - 1) == 0;
    sig[5] ^= 0x40;
    pk[3] ^= 2;
    bad += crypto_ed25519_check(sig, pk, (const uint8_t *)m, sizeof(m) - 1) == 0;
    return bad;
}

/* A text of each ports format, signed through sign_text: it checks as its
 * own format and as no other (the first line is the domain), not with
 * its first line swapped for another format's (the bytes changed), and a
 * text without its format's first line is not signed at all. */
static unsigned domains(void)
{
    static const char *bodies[NFORMATS] = { "name t\nsignature\n", "serial 1\nsignature\n",
                                            "serial 1\nsignature\n" };
    uint8_t seed[32], sk[64], pk[32];
    char text[256], signed_text[512];
    unsigned bad = 0;
    if (getentropy(seed, sizeof(seed)))
        return 1;
    crypto_ed25519_key_pair(sk, pk, seed);
    for (size_t i = 0; i < NFORMATS; i++) {
        int n = snprintf(text, sizeof(text), "%s%s", formats[i].first, bodies[i]);
        FILE *out = fmemopen(signed_text, sizeof(signed_text), "w");
        if (!out || sign_text(&formats[i], text, (size_t)n, sk, out, 1)) {
            bad++;
            if (out)
                fclose(out);
            continue;
        }
        long len = ftell(out);
        fclose(out);
        for (size_t j = 0; j < NFORMATS; j++) {
            int got = check_text(&formats[j], signed_text, (size_t)len, pk, 1) == 0;
            bad += got != (i == j);
        }
        /* The same signature over the text relabelled as the next format. */
        const struct format *other = &formats[(i + 1) % NFORMATS];
        char relabelled[512];
        size_t skip = strlen(formats[i].first), at = strlen(other->first);
        memcpy(relabelled, other->first, at);
        memcpy(relabelled + at, signed_text + skip, (size_t)len - skip);
        bad += check_text(other, relabelled, at + (size_t)len - skip, pk, 1) == 0;
    }
    /* Not the format's first line: refused before any signing. */
    static const char wrong[] = "jamos-ports 1\nsignature\n";
    FILE *sink = fopen("/dev/null", "w");
    if (sink) {
        bad += sign_text(&formats[0], wrong, sizeof(wrong) - 1, sk, sink, 1) == 0;
        fclose(sink);
    }
    crypto_wipe(sk, sizeof(sk));
    return bad;
}

static int self_test(void)
{
    unsigned refusals = 0;
    unsigned s = sign_vectors(), p = pk_vectors(), c = check_vectors(&refusals),
             r = round_trip(), d = domains();
    printf("jamos-sign self-test: %zu signatures, %zu public keys, %zu checks (%u of them "
           "refused, as the vectors say), a round trip, %zu formats' domains: %u, %u, %u, %u, "
           "%u failed\n",
           nb_ed_25519_vectors / 4, nb_ed_25519_pk_vectors / 2, nb_ed_25519_check_vectors / 4,
           refusals, NFORMATS, s, p, c, r, d);
    unsigned bad = s + p + c + r + d;
    printf("jamos-sign self-test: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: jamos-sign keygen [--ports] [DIR] | pub KEY | sign KEY [FORMAT] | "
            "verify PUB [FORMAT] | self-test\n");
    return 2;
}

/* keygen's arguments: [--ports] [DIR]. */
static int cmd_keygen(int argc, char **argv)
{
    const char *name = "update";
    int at = 2;
    if (at < argc && !strcmp(argv[at], "--ports")) {
        name = "ports";
        at++;
    }
    if (argc - at > 1)
        return usage();
    char dir[1024];
    const char *home = getenv("HOME");
    if (at < argc)
        snprintf(dir, sizeof(dir), "%s", argv[at]);
    else if (!home || snprintf(dir, sizeof(dir), "%s/.config/jamos", home) >= (int)sizeof(dir))
        return usage();
    if (at == argc) {   /* ~/.config first, then ~/.config/jamos */
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        if (mkdir(parent, 0700) && errno != EEXIST) {
            fprintf(stderr, "jamos-sign: %s: %s\n", parent, strerror(errno));
            return 1;
        }
    }
    return keygen(dir, name);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "self-test"))
        return self_test();
    if (argc >= 2 && !strcmp(argv[1], "keygen"))
        return cmd_keygen(argc, argv);
    if (argc == 4 && (!strcmp(argv[1], "sign") || !strcmp(argv[1], "verify"))) {
        const struct format *fmt = format_named(argv[3]);
        if (!fmt)
            return 2;
        return argv[1][0] == 's' ? sign(argv[2], fmt) : verify(argv[2], fmt);
    }
    if (argc != 3)
        return usage();
    if (!strcmp(argv[1], "sign"))
        return sign(argv[2], &update_format);
    if (!strcmp(argv[1], "verify"))
        return verify(argv[2], &update_format);
    if (!strcmp(argv[1], "pub")) {
        uint8_t sk[64], pk[32];
        char h[65];
        if (load_secret(argv[2], sk, pk))
            return 1;
        crypto_wipe(sk, sizeof(sk));
        hex(pk, 32, h);
        printf(PUB_PREFIX "%s\n", h);
        return 0;
    }
    return usage();
}
