/* jamos-sign: the Mac's half of signed updates (<update.h>): the key, made
 * once, and the signature of each manifest tools/update-server.py serves.
 * Built by the Makefile with the Mac's own compiler (build/host/jamos-sign)
 * from the same Monocypher the PC checks with (third_party/monocypher):
 * Ed25519 as RFC 8032 has it (EdDSA over edwards25519 with SHA-512,
 * Monocypher's optional crypto_ed25519_*), so a signature made here is
 * checked there with the very same code.
 *
 *   jamos-sign keygen [DIR]     DIR/update.key (the secret, mode 0600) and
 *                               DIR/update.pub (default DIR: ~/.config/jamos);
 *                               refuses if either is there already
 *   jamos-sign pub KEY          the public key file's line for secret KEY
 *   jamos-sign sign KEY         a manifest on stdin, its last line
 *                               "signature": out with the line filled in
 *   jamos-sign verify PUB       a signed manifest on stdin: exit 0 if PUB's
 *                               key signed exactly the bytes before its
 *                               signature line, 1 if not
 *   jamos-sign self-test        Monocypher's Ed25519 test vectors (its
 *                               tests/vectors.h tables, third_party/
 *                               monocypher/tests/vectors-ed25519.h) and a
 *                               round trip with a fresh key; exit 0 on PASS
 *
 * The files, one line each, lower-case hex:
 *   update.key   "ed25519-secret <64 hex: the 32-byte seed>\n"
 *   update.pub   "ed25519 <64 hex: the 32-byte public key>\n"
 * The signature line: "signature <128 hex: the 64-byte signature>\n", over
 * every byte of the manifest before that line. */
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

static int keygen(const char *dir)
{
    char key[1024], pub[1024];
    char line[256], h[129];
    if (snprintf(key, sizeof(key), "%s/update.key", dir) >= (int)sizeof(key) ||
        snprintf(pub, sizeof(pub), "%s/update.pub", dir) >= (int)sizeof(pub))
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

/* A whole manifest from stdin: its bytes and length. */
static int read_manifest(char *buf, size_t *len)
{
    size_t n = fread(buf, 1, MANIFEST_MAX + 1, stdin);
    if (n > MANIFEST_MAX) {
        fprintf(stderr, "jamos-sign: a manifest is at most %d bytes\n", MANIFEST_MAX);
        return -1;
    }
    *len = n;
    return 0;
}

/* Where the manifest's last line starts (the bytes signed end there). */
static size_t last_line(const char *buf, size_t len)
{
    size_t at = len ? len - 1 : 0;   /* the last line's own '\n' */
    while (at > 0 && buf[at - 1] != '\n')
        at--;
    return at;
}

static int sign(const char *keyfile)
{
    char buf[MANIFEST_MAX + 1], h[129];
    size_t len;
    uint8_t sk[64], pk[32], sig[64];
    if (read_manifest(buf, &len))
        return 1;
    size_t at = last_line(buf, len);
    if (len - at != sizeof(SIG_LINE) || memcmp(buf + at, SIG_LINE "\n", sizeof(SIG_LINE))) {
        fprintf(stderr, "jamos-sign: the manifest's last line must be \"" SIG_LINE "\"\n");
        return 1;
    }
    if (len + 1 + 128 > MANIFEST_MAX) {
        fprintf(stderr, "jamos-sign: the signed manifest would be over %d bytes\n", MANIFEST_MAX);
        return 1;
    }
    if (load_secret(keyfile, sk, pk))
        return 1;
    crypto_ed25519_sign(sig, sk, (const uint8_t *)buf, at);
    crypto_wipe(sk, sizeof(sk));
    hex(sig, 64, h);
    fwrite(buf, 1, at, stdout);
    printf(SIG_LINE " %s\n", h);
    return fflush(stdout) ? 1 : 0;
}

static int verify(const char *pubfile)
{
    char buf[MANIFEST_MAX + 1];
    size_t len;
    uint8_t pk[32], sig[64];
    if (read_manifest(buf, &len) || read_line_file(pubfile, PUB_PREFIX, pk, 32))
        return 1;
    size_t at = last_line(buf, len), p = sizeof(SIG_LINE);   /* "signature " */
    if (len - at != p + 128 + 1 || memcmp(buf + at, SIG_LINE " ", p) || buf[len - 1] != '\n' ||
        unhex(buf + at + p, sig, 64)) {
        fprintf(stderr, "jamos-sign: no \"" SIG_LINE " <128 hex digits>\" last line\n");
        return 1;
    }
    if (crypto_ed25519_check(sig, pk, (const uint8_t *)buf, at)) {
        fprintf(stderr, "jamos-sign: the signature doesn't match: not this key's, or the "
                "manifest changed\n");
        return 1;
    }
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

static int self_test(void)
{
    unsigned refusals = 0;
    unsigned s = sign_vectors(), p = pk_vectors(), c = check_vectors(&refusals),
             r = round_trip();
    printf("jamos-sign self-test: %zu signatures, %zu public keys, %zu checks (%u of them "
           "refused, as the vectors say), a round trip: %u, %u, %u, %u failed\n",
           nb_ed_25519_vectors / 4, nb_ed_25519_pk_vectors / 2, nb_ed_25519_check_vectors / 4,
           refusals, s, p, c, r);
    unsigned bad = s + p + c + r;
    printf("jamos-sign self-test: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: jamos-sign keygen [DIR] | pub KEY | sign KEY | verify PUB | "
            "self-test\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "self-test"))
        return self_test();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "keygen")) {
        char dir[1024];
        const char *home = getenv("HOME");
        if (argc == 3)
            snprintf(dir, sizeof(dir), "%s", argv[2]);
        else if (!home || snprintf(dir, sizeof(dir), "%s/.config/jamos", home) >= (int)sizeof(dir))
            return usage();
        if (argc == 2) {   /* ~/.config first, then ~/.config/jamos */
            char parent[1024];
            snprintf(parent, sizeof(parent), "%s/.config", home);
            if (mkdir(parent, 0700) && errno != EEXIST) {
                fprintf(stderr, "jamos-sign: %s: %s\n", parent, strerror(errno));
                return 1;
            }
        }
        return keygen(dir);
    }
    if (argc != 3)
        return usage();
    if (!strcmp(argv[1], "sign"))
        return sign(argv[2]);
    if (!strcmp(argv[1], "verify"))
        return verify(argv[2]);
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
