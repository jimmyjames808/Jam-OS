/* `update`: a new build from the Mac, run without moving the stick
 * (docs/M9-PLAN.md, "update: a new build from the Mac"). This header has
 * what the PC's two sides share: the manifest that names the build, and
 * the hand-off of a fetched build to init.
 *
 * The manifest (made by tools/update-server.py on the Mac) is text, six
 * lines in this order, each ended by one '\n', single spaces, nothing
 * else (no '\r', no blank line, nothing after the last line):
 *
 *     jamos-update 1
 *     version <the kernel's version string: 1..47 of A-Z a-z 0-9 . _ + ->
 *     git <7..40 lower-case hex digits, optionally followed by -dirty>
 *     kernel <size> <SHA-256>     build/jamos.elf
 *     bootfs <size> <SHA-256>     build/bootfs.img
 *     signature <signature>
 *
 * A size is decimal bytes, 1..UPDATE_FILE_MAX, no leading zero; a SHA-256
 * is 64 lower-case hex digits. The signature is 128 lower-case hex digits:
 * the Ed25519 signature (RFC 8032: EdDSA over edwards25519 with SHA-512,
 * Monocypher's crypto_ed25519_check) of every byte before its line
 * (struct update_manifest's signed_len), by the owner's update key
 * (tools/jamos-sign.c makes it, once, on the Mac, and signs each manifest
 * with it). The key's public half is in each build's boot image, the file
 * UPDATE_KEY_FILE ("ed25519 <64 lower-case hex digits>\n"): a build
 * without it has no key and takes no update at all. A bare "signature"
 * line is an unsigned manifest: it parses (the parser only reads the
 * format), and init refuses it. init checks the signature before it
 * trusts anything else the manifest says: a size or a SHA-256 the key
 * didn't sign is never compared with anything.
 *
 * The hand-off: whoever fetched the build (bin/update; a test program,
 * user/tests/updtest) holds an offer channel from init (initctl's
 * update_offer) and writes one struct update_offer on it, with the kernel
 * and the boot image in two VMOs (RIGHT_READ is enough). init checks the
 * manifest's signature with its build's key, copies both files into VMOs
 * only it holds, so the sender can't change a byte after the check, checks
 * each file's length and SHA-256 against the manifest (which it parses
 * itself: nothing the sender says about it is trusted), and hands its
 * copies to the kernel (kexec_load) as the stored kernel,
 * the one `reboot` and a panic start. It answers one struct update_answer
 * on the channel and closes it. Any refusal leaves the stored kernel as it
 * was. Nothing is ever written to the stick: the fetched build runs until
 * a power-off or until /esp changes. An offer with UPDATE_OFFER_CHECK_ONLY
 * is checked the same way and answered, but nothing is loaded (the shell's
 * `update -n`).
 *
 * Who offers: the shell's `update` takes the offer channel from init
 * (initctl.update_offer) and hands it to bin/update (user/services/update),
 * the fetcher, which holds only that channel and /svc/net. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <os.h>
#include <sha256.h>

#define UPDATE_MANIFEST_MAX 1024u          /* bytes of a manifest, at most */
#define UPDATE_FILE_MAX     (32u << 20)    /* bytes of either file: the stored kernel's region */
#define UPDATE_VERSION_MAX  47u            /* bytes of the version string */
#define UPDATE_GIT_MAX      47u            /* bytes of the git field (40 hex + "-dirty") */
#define UPDATE_KEY_BYTES    32u            /* an Ed25519 public key */
#define UPDATE_SIG_BYTES    64u            /* an Ed25519 signature */
#define UPDATE_KEY_FILE     "update.pub"   /* the public key, in the boot image (bootfs) */

/* The files of a build, in the manifest's order. */
enum { UPDATE_KERNEL, UPDATE_BOOTFS, UPDATE_FILES };

/* A parsed manifest. */
struct update_manifest {
    char     version[UPDATE_VERSION_MAX + 1];   /* NUL-terminated */
    char     git[UPDATE_GIT_MAX + 1];           /* NUL-terminated */
    struct {
        uint64_t size;                          /* bytes */
        uint8_t  sha256[SHA256_BYTES];
    } file[UPDATE_FILES];
    size_t   signed_len;                        /* bytes before the signature line */
    bool     has_signature;                     /* the signature line has a value */
    uint8_t  signature[UPDATE_SIG_BYTES];       /* ... this one (zeros if not) */
};

/* Parse a manifest of len bytes (not NUL-terminated; any bytes at all),
 * strictly as the header above says, into *out (written only on
 * success). Only the format is checked here, not the signature: an
 * unsigned manifest parses (has_signature false). ERR_INVALID_ARGS: not a
 * manifest (a line missing, out of order, misspelt, a bad character, too
 * long, a signature that isn't 128 hex digits, anything after the end);
 * ERR_OUT_OF_RANGE: a size of 0 or over UPDATE_FILE_MAX;
 * ERR_NOT_SUPPORTED: another format version. */
status_t update_manifest_parse(const void *text, size_t len, struct update_manifest *out);
/* Did `key` sign exactly the first m->signed_len bytes of text, the
 * manifest m was parsed from? OK if so; ERR_ACCESS_DENIED if m has no
 * signature, or it isn't key's signature of those bytes. */
status_t update_manifest_verify(const struct update_manifest *m, const void *text,
                                const uint8_t key[UPDATE_KEY_BYTES]);
/* The public key file (UPDATE_KEY_FILE) of len bytes into key (written
 * only on success): exactly "ed25519 <64 lower-case hex digits>\n".
 * ERR_INVALID_ARGS otherwise. */
status_t update_key_parse(const void *text, size_t len, uint8_t key[UPDATE_KEY_BYTES]);
/* The file's name in the manifest ("kernel", "bootfs"). */
const char *update_file_name(unsigned file);

/* ---- the hand-off to init ------------------------------------------------------- */

#define UPDATE_OFFER_MAGIC  0x4f50554au   /* "JUPO" */
#define UPDATE_ANSWER_MAGIC 0x4150554au   /* "JUPA" */

/* The one message on an offer channel, with exactly two handles: the
 * kernel's VMO, then the boot image's. Every field is checked. */
struct update_offer {
    uint32_t txid;                            /* 0 (the channel convention's place) */
    uint32_t magic;                           /* UPDATE_OFFER_MAGIC */
    uint64_t bytes[UPDATE_FILES];             /* each file's length as fetched (its VMO may
                                               * be longer: page-rounded) */
    uint32_t manifest_len;                    /* bytes of manifest[] used */
    uint32_t flags;                           /* 0, or UPDATE_OFFER_CHECK_ONLY */
    uint8_t  manifest[UPDATE_MANIFEST_MAX];   /* as fetched, not parsed */
};

#define UPDATE_OFFER_CHECK_ONLY 1u   /* check it, load nothing: the stored kernel stays */

/* Which check refused an offer. */
enum update_why {
    UPDATE_ACCEPTED,    /* the stored kernel is the offered build (CHECK_ONLY: it would be) */
    UPDATE_BAD_OFFER,   /* the message itself: its size, magic, flags or handles */
    UPDATE_BAD_MANIFEST,/* the manifest doesn't parse (status says how) */
    UPDATE_BAD_SIZE,    /* a file's length isn't the manifest's */
    UPDATE_SHORT_VMO,   /* a VMO is shorter than its length, or can't be read */
    UPDATE_BAD_HASH,    /* a file's SHA-256 isn't the manifest's */
    UPDATE_NOT_LOADED,  /* the kernel refused it (kexec_load's status) */
    UPDATE_NO_KEY,      /* this build has no update key (or no valid one): updates are off */
    UPDATE_UNSIGNED,    /* the manifest has no signature */
    UPDATE_BAD_SIGNATURE, /* not this build's key's signature, or the manifest changed */
    UPDATE_WHY_COUNT,
};

/* init's answer, then the channel closes. */
struct update_answer {
    uint32_t txid;                            /* 0 */
    uint32_t magic;                           /* UPDATE_ANSWER_MAGIC */
    int32_t  status;                          /* OK only with UPDATE_ACCEPTED */
    uint32_t why;                             /* enum update_why */
    uint32_t file;                            /* the file a size, VMO or hash refusal is about */
    uint32_t check_ms;                        /* the copy and the check */
    char     version[UPDATE_VERSION_MAX + 1]; /* the manifest's, if it parsed ("" if not) */
    char     git[UPDATE_GIT_MAX + 1];
};

/* "the SHA-256 isn't the manifest's", ...: why, in words. */
const char *update_why_str(uint32_t why);
