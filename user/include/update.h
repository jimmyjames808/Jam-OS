/* `update`: a new build from the Mac, run without moving the stick
 * (docs/M9-PLAN.md, "update: a new build from the Mac"). This header has
 * what the PC's two sides share: the manifest that names the build, and
 * the hand-off of a fetched build to init.
 *
 * The manifest (made by tools/update-server.py on the Mac) is text: lines
 * each ended by one '\n', single spaces, nothing else (no '\r', no blank
 * line, nothing after the signature line). Format 2 is the stable base:
 * these seven lines, in this order, mean what they say here for every
 * build to come:
 *
 *     jamos-update 2
 *     version <the kernel's version string: 1..47 of A-Z a-z 0-9 . _ + ->
 *     git <7..40 lower-case hex digits, optionally followed by -dirty>
 *     net <the build's network default: vlan<1..4094>, or untagged>
 *     kernel <size> <SHA-256>     build/jamos.elf
 *     bootfs <size> <SHA-256>     build/bootfs.img
 *     signature <signature>
 *
 * Extension lines carry what later builds add, so that a new piece of
 * information never needs a new format: anywhere between the first line
 * and the signature line, a line `<key>` or `<key> <value>`, the key 1..32
 * of a-z 0-9 - (none of the seven names above), the value 1..UPDATE_EXT_MAX
 * printable ASCII bytes (0x20..0x7e). A parser ignores an extension line
 * it doesn't know (it is signed all the same: the signature covers every
 * byte before its line), so an older build takes a newer manifest. A key
 * that starts with '!' (then 1..31 of a-z 0-9 -) is a must-understand
 * line: what a build must act on to run the new one right. A build that
 * doesn't know it can't take the update: init refuses it, once the
 * signature has checked out, saying which line it needs
 * (UPDATE_NEEDS_NEWER), never skipping it. This build knows no extension
 * line. Another first line ("jamos-update 3") is another format, which
 * only a new signature scheme should ever need: refused too
 * (ERR_NOT_SUPPORTED, UPDATE_NEEDS_NEWER). So only a crypto change can
 * make the stick need `make flash` for an update to go on. Every update
 * request carries UPDATE_FORMAT (<updwire.h>), the format this build
 * reads, and the server answers in it: a server that also makes a newer
 * format still serves an older build the format it reads.
 *
 * A size is decimal bytes, 1..UPDATE_FILE_MAX, no leading zero; a SHA-256
 * is 64 lower-case hex digits; a VLAN has no leading zero. The `net` line
 * is the build's build.txt's (the Makefile's JAMOS_VLAN, local.mk): what
 * the build's network does on a boot with no `vlan=` word (format 1 had
 * no `net` line). The signature is 128 lower-case hex digits:
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
 * didn't sign is never compared with anything, and the `net` line is
 * signed with the rest.
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
 * was. By default nothing is written to the stick: the fetched build runs
 * until a power-off or until /esp changes. An offer with
 * UPDATE_OFFER_CHECK_ONLY is checked the same way and answered, but
 * nothing is loaded (the shell's `update -n`). One with UPDATE_OFFER_WRITE
 * (`update -w`) is loaded and then also written to the stick's ESP by init
 * (init's espwrite.c: init alone can make the ESP writable); if that write
 * fails the answer is UPDATE_NOT_WRITTEN: the build is loaded all the same,
 * and the stick still boots (write_step and stick say how far it got).
 *
 * The network's default never changes by accident: init refuses a build
 * whose `net` is not the running build's own (its build.txt), so a PC
 * whose builds tag VLAN 21 can't fetch one that sends untagged frames (or
 * the other way round) and reboot into it, unless the offer carries
 * UPDATE_OFFER_FORCE (the shell's `update -f`).
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
#define UPDATE_NET_MAX      15u            /* bytes of the net field ("vlan4094", "untagged") */
#define UPDATE_KEY_BYTES    32u            /* an Ed25519 public key */
#define UPDATE_SIG_BYTES    64u            /* an Ed25519 signature */
#define UPDATE_KEY_FILE     "update.pub"   /* the public key, in the boot image (bootfs) */
#define UPDATE_FORMAT       2u             /* the manifest format this build reads */
#define UPDATE_EXT_KEY_MAX  32u            /* bytes of an extension line's key ('!' too) */
#define UPDATE_EXT_MAX      200u           /* bytes of an extension line's value */

/* The files of a build, in the manifest's order. */
enum { UPDATE_KERNEL, UPDATE_BOOTFS, UPDATE_FILES };

/* A parsed manifest. */
struct update_manifest {
    char     version[UPDATE_VERSION_MAX + 1];   /* NUL-terminated */
    char     git[UPDATE_GIT_MAX + 1];           /* NUL-terminated */
    char     net[UPDATE_NET_MAX + 1];           /* "vlan21", "untagged"; NUL-terminated */
    struct {
        uint64_t size;                          /* bytes */
        uint8_t  sha256[SHA256_BYTES];
    } file[UPDATE_FILES];
    size_t   signed_len;                        /* bytes before the signature line */
    bool     has_signature;                     /* the signature line has a value */
    uint8_t  signature[UPDATE_SIG_BYTES];       /* ... this one (zeros if not) */
    char     needs[UPDATE_EXT_KEY_MAX + 1];     /* the first must-understand line this
                                                 * build doesn't know (its key, '!' and
                                                 * all; "" if none): it can't take it */
};

/* Parse a manifest of len bytes (not NUL-terminated; any bytes at all),
 * strictly as the header above says, into *out (written only on
 * success, but for needs: below). Only the format is checked here, not
 * the signature: an unsigned manifest parses (has_signature false), and
 * so does one with a must-understand line this build doesn't know
 * (out->needs names it: the caller refuses it once the signature is
 * checked). Extension lines it doesn't know are skipped. ERR_INVALID_ARGS:
 * not a manifest (a line missing, out of order, misspelt, twice, a bad
 * character, too long, a signature that isn't 128 hex digits, anything
 * after the end); ERR_OUT_OF_RANGE: a size of 0 or over UPDATE_FILE_MAX;
 * ERR_NOT_SUPPORTED: another format (out->needs alone is written: its
 * first line, cut to fit). */
status_t update_manifest_parse(const void *text, size_t len, struct update_manifest *out);
/* The network default a build.txt of len bytes records (its line
 * "net vlan21" or "net untagged", by the manifest's rules), into out
 * (written only on success). ERR_NOT_FOUND: no such line, or not a valid
 * value (a build made before build.txt had one). */
status_t update_build_net(const void *text, size_t len, char out[UPDATE_NET_MAX + 1]);
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
/* Take it even if its network default (the manifest's `net`) isn't this
 * build's (`update -f`): init refuses such a build otherwise. */
#define UPDATE_OFFER_FORCE      4u
/* Also write it to the stick (`update -w`): once checked and loaded, init
 * writes the build to the boot stick's ESP, keeping the stick's own build
 * as the previous one (boot/prev-jamos.elf, boot/prev-bootfs.img: the boot
 * menu's "Jam OS (previous build)"), so it survives a power-off. Never
 * with CHECK_ONLY. */
#define UPDATE_OFFER_WRITE      2u
/* A test's (bin/updtest): the stick write fails at step s (enum
 * update_write_step: ROOM, PREV, NEW or SWITCH) as if the ESP's service
 * had died there, once: flags |= UPDATE_OFFER_FAIL(s). Only with WRITE. */
#define UPDATE_OFFER_FAIL_SHIFT 8u
#define UPDATE_OFFER_FAIL_MASK  (0xffu << UPDATE_OFFER_FAIL_SHIFT)
#define UPDATE_OFFER_FAIL(s)    ((uint32_t)(s) << UPDATE_OFFER_FAIL_SHIFT)

/* How far a stick write got (struct update_answer's write_step): DONE, or
 * the step that failed. Each step is whole before the next begins. */
enum update_write_step {
    UPDATE_WRITE_NONE,    /* no stick write asked for */
    UPDATE_WRITE_OPEN,    /* the ESP made writable for init (devmgr's ESP_WRITE) */
    UPDATE_WRITE_ROOM,    /* an earlier write's leftovers removed; room for two builds */
    UPDATE_WRITE_PREV,    /* the stick's build copied as the previous build, read back */
    UPDATE_WRITE_NEW,     /* the new build written under temporary names, read back */
    UPDATE_WRITE_SWITCH,  /* the names switched: the stick's build is the new one */
    UPDATE_WRITE_DONE,    /* all of it, and the ESP read-only again */
    UPDATE_WRITE_STEPS,
};

/* What the stick boots after a stick write (struct update_answer's stick). */
enum update_stick {
    UPDATE_STICK_NONE,      /* no stick write asked for */
    UPDATE_STICK_OLD,       /* its build, as before (untouched, or put back) */
    UPDATE_STICK_NEW,       /* the new build; the old one as "Jam OS (previous build)" */
    UPDATE_STICK_PREVIOUS,  /* only "Jam OS (previous build)" (the old build) is sure to */
    UPDATE_STICK_STATES,
};

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
    UPDATE_NOT_WRITTEN, /* loaded (the stored kernel is the new build), but the stick write
                         * failed: write_step, status and stick say where and what */
    UPDATE_NET_CHANGE,  /* its network default isn't the running build's, and no FORCE */
    UPDATE_NEEDS_NEWER, /* a must-understand line, or a format, this build doesn't know
                         * (the answer's needs says which): only a newer build takes it */
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
    uint32_t write_step;                      /* UPDATE_OFFER_WRITE: enum update_write_step */
    uint32_t stick;                           /* ... enum update_stick */
    uint32_t write_ms;                        /* ... the stick write's time */
    char     net[UPDATE_NET_MAX + 1];         /* the manifest's network default ("" if none) */
    char     net_running[UPDATE_NET_MAX + 1]; /* the running build's ("" if not known) */
    char     needs[UPDATE_EXT_KEY_MAX + 1];   /* UPDATE_NEEDS_NEWER: the line or format */
};

/* "the SHA-256 isn't the manifest's", ...: why, in words. */
const char *update_why_str(uint32_t why);
/* A stick write's step ("writing the new build"), and what the stick
 * boots ("the stick boots the new build ..."), in words. */
const char *update_write_step_str(uint32_t step);
const char *update_stick_str(uint32_t stick);
