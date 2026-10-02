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
 *     signature
 *
 * A size is decimal bytes, 1..UPDATE_FILE_MAX, no leading zero; a SHA-256
 * is 64 lower-case hex digits. The signature line is the place kept for
 * signed updates (a ROADMAP follow-up): today it has no value, and a
 * manifest whose signature line has one is refused (ERR_NOT_SUPPORTED:
 * this build can't check it, and fails closed). A signature will cover
 * every byte before its line (struct update_manifest's signed_len).
 *
 * The hand-off: whoever fetched the build (bin/update; a test program,
 * user/tests/updtest) holds an offer channel from init (initctl's
 * update_offer) and writes one struct update_offer on it, with the kernel
 * and the boot image in two VMOs (RIGHT_READ is enough). init copies both
 * into VMOs only it holds, so the sender can't change a byte after the
 * check, checks each file's length and SHA-256 against the manifest
 * (which it parses itself: nothing the sender says about it is trusted),
 * and hands its copies to the kernel (kexec_load) as the stored kernel,
 * the one `reboot` and a panic start. It answers one struct update_answer
 * on the channel and closes it. Any refusal leaves the stored kernel as it
 * was. Nothing is ever written to the stick: the fetched build runs until
 * a power-off or until /esp changes. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <os.h>
#include <sha256.h>

#define UPDATE_MANIFEST_MAX 1024u          /* bytes of a manifest, at most */
#define UPDATE_FILE_MAX     (32u << 20)    /* bytes of either file: the stored kernel's region */
#define UPDATE_VERSION_MAX  47u            /* bytes of the version string */
#define UPDATE_GIT_MAX      47u            /* bytes of the git field (40 hex + "-dirty") */

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
};

/* Parse a manifest of len bytes (not NUL-terminated; any bytes at all),
 * strictly as the header above says, into *out (written only on
 * success). ERR_INVALID_ARGS: not a manifest (a line missing, out of
 * order, misspelt, a bad character, too long, anything after the end);
 * ERR_OUT_OF_RANGE: a size of 0 or over UPDATE_FILE_MAX;
 * ERR_NOT_SUPPORTED: another format version, or a signature. */
status_t update_manifest_parse(const void *text, size_t len, struct update_manifest *out);
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
    uint32_t flags;                           /* 0 */
    uint8_t  manifest[UPDATE_MANIFEST_MAX];   /* as fetched, not parsed */
};

/* Which check refused an offer. */
enum update_why {
    UPDATE_ACCEPTED,    /* the stored kernel is the offered build */
    UPDATE_BAD_OFFER,   /* the message itself: its size, magic, flags or handles */
    UPDATE_BAD_MANIFEST,/* the manifest doesn't parse (status says how) */
    UPDATE_BAD_SIZE,    /* a file's length isn't the manifest's */
    UPDATE_SHORT_VMO,   /* a VMO is shorter than its length, or can't be read */
    UPDATE_BAD_HASH,    /* a file's SHA-256 isn't the manifest's */
    UPDATE_NOT_LOADED,  /* the kernel refused it (kexec_load's status) */
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
