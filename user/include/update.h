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
 * (UPDATE_NEEDS_NEWER), never skipping it. Another first line
 * ("jamos-update 3") is another format, which
 * only a new signature scheme should ever need: refused too
 * (ERR_NOT_SUPPORTED, UPDATE_NEEDS_NEWER). So only a crypto change can
 * make the stick need `make flash` for an update to go on. Every update
 * request carries UPDATE_FORMAT (<updwire.h>), the format this build
 * reads, and the server answers in it: a server that also makes a newer
 * format still serves an older build the format it reads.
 *
 * The extension lines this build knows (each at most once; a known line
 * that breaks its own rules makes the manifest ERR_INVALID_ARGS):
 *
 *     menu <size> <SHA-256>       boot/limine.conf, the boot menu
 *
 * `menu` names the boot menu that goes with the build (the server's
 * boot/limine.conf, size 1..UPDATE_MENU_MAX): a third file to fetch,
 * checked like the other two, which `update -w` also writes to the
 * stick's ESP as boot/limine/limine.conf once the build is written, if
 * the menu passes init's check of it (<bootmenu.h>; espmenu.c). It is an
 * extension line, not a must-understand one: a build older than it skips
 * it and updates the build alone, its menu left as it was. A manifest
 * without it (an older server's) updates the build alone too.
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
 * and the boot image in two VMOs (RIGHT_READ is enough), and the boot menu
 * in a third when the manifest names one (then it must). init checks the
 * manifest's signature with its build's key, copies every file into VMOs
 * only it holds, so the sender can't change a byte after the check, checks
 * each file's length and SHA-256 against the manifest (which it parses
 * itself: nothing the sender says about it is trusted; a menu that isn't
 * the signed one is refused like a kernel that isn't), and hands its
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
 * Then, with a menu, the stick's boot menu too (espmenu.c; the answer's
 * menu says what became of it): past its SHA-256, the menu never decides
 * whether the build is taken (a menu init's check refuses is not written,
 * and the build is), and only a stick write touches the stick's menu
 * (CHECK_ONLY and a plain offer check its SHA-256 and nothing more).
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
#define UPDATE_MENU_MAX     (64u << 10)    /* bytes of the boot menu (`menu`), at most */

/* The files of a build, in the manifest's order (UPDATE_FILES: the build
 * itself), then the boot menu, which a manifest may name (`menu`). */
enum { UPDATE_KERNEL, UPDATE_BOOTFS, UPDATE_FILES, UPDATE_MENU = UPDATE_FILES, UPDATE_PARTS };

/* A parsed manifest. */
struct update_manifest {
    char     version[UPDATE_VERSION_MAX + 1];   /* NUL-terminated */
    char     git[UPDATE_GIT_MAX + 1];           /* NUL-terminated */
    char     net[UPDATE_NET_MAX + 1];           /* "vlan21", "untagged"; NUL-terminated */
    struct {
        uint64_t size;                          /* bytes */
        uint8_t  sha256[SHA256_BYTES];
    } file[UPDATE_PARTS];                       /* [UPDATE_MENU]: only if has_menu */
    bool     has_menu;                          /* a `menu` line names the boot menu */
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
 * after the end, a `menu` line that breaks its rules or comes twice);
 * ERR_OUT_OF_RANGE: a size of 0 or over UPDATE_FILE_MAX (a menu's, over
 * UPDATE_MENU_MAX);
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
/* The file's name in the manifest ("kernel", "bootfs", "menu"). */
const char *update_file_name(unsigned file);

/* ---- the hand-off to init ------------------------------------------------------- */

#define UPDATE_OFFER_MAGIC  0x4f50554au   /* "JUPO" */
#define UPDATE_ANSWER_MAGIC 0x4150554au   /* "JUPA" */

/* The one message on an offer channel, with exactly two handles: the
 * kernel's VMO, then the boot image's; and a third, the boot menu's, when
 * the manifest has a `menu` line (exactly then). Every field is checked. */
struct update_offer {
    uint32_t txid;                            /* 0 (the channel convention's place) */
    uint32_t magic;                           /* UPDATE_OFFER_MAGIC */
    uint64_t bytes[UPDATE_PARTS];             /* each file's length as fetched (its VMO may
                                               * be longer: page-rounded); [UPDATE_MENU]
                                               * 0 when the manifest names no menu */
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
 * menu's "Jam OS (previous build)"), so it survives a power-off; then the
 * boot menu, if the offer has one. Never with CHECK_ONLY. */
#define UPDATE_OFFER_WRITE      2u
/* A test's (bin/updtest): the stick write fails at step s (enum
 * update_write_step: ROOM, PREV, NEW, SWITCH or MENU) as if the ESP's
 * service had died there, once: flags |= UPDATE_OFFER_FAIL(s). Only with
 * WRITE. */
#define UPDATE_OFFER_FAIL_SHIFT 8u
#define UPDATE_OFFER_FAIL_MASK  (0xffu << UPDATE_OFFER_FAIL_SHIFT)
#define UPDATE_OFFER_FAIL(s)    ((uint32_t)(s) << UPDATE_OFFER_FAIL_SHIFT)
/* A test's too: the stick write stops dead right after the n-th change of
 * its swaps (1..UPDATE_STOP_MAX), with no clean-up and no sync, as a power
 * cut there would leave it: flags |= UPDATE_OFFER_STOP(n). Only with
 * WRITE, not with FAIL. */
#define UPDATE_OFFER_STOP_SHIFT 16u
#define UPDATE_OFFER_STOP_MASK  (0xffu << UPDATE_OFFER_STOP_SHIFT)
#define UPDATE_OFFER_STOP(n)    ((uint32_t)(n) << UPDATE_OFFER_STOP_SHIFT)
/* The build's swap's changes of names (init's espwrite.c has the order):
 * the stick's two files renamed aside, the new two into their names, the
 * older previous build's two removed, the stick's old two renamed as the
 * previous build. Stops 1..UPDATE_SWAP_OPS. */
#define UPDATE_SWAP_OPS         8u
/* The boot menu's changes (espmenu.c has the order): the new menu written
 * as limine.conf.new, the stick's menu copied as the spare
 * (boot/limine.conf), the older limine.conf.prev removed, limine.conf
 * renamed limine.conf.prev, limine.conf.new renamed limine.conf, the spare
 * removed. Stops UPDATE_SWAP_OPS + 1.. UPDATE_STOP_MAX, numbered so
 * whether or not the build's swap ran (a stick that had the build). */
#define UPDATE_MENU_OPS         6u
#define UPDATE_STOP_MAX         (UPDATE_SWAP_OPS + UPDATE_MENU_OPS)

/* How far a stick write got (struct update_answer's write_step): DONE, or
 * the step that failed. Each step is whole before the next begins; they
 * run in the order ROOM, NEW, SWITCH, PREV, MENU (the numbers are older).
 * MENU is never a failed step: the build is written by then, and the
 * answer's menu says what became of the menu. */
enum update_write_step {
    UPDATE_WRITE_NONE,    /* no stick write asked for */
    UPDATE_WRITE_OPEN,    /* the ESP made writable for init (devmgr's ESP_WRITE) */
    UPDATE_WRITE_ROOM,    /* an earlier write's renames finished or undone, its leftovers
                           * removed; room for the new build */
    UPDATE_WRITE_PREV,    /* the older previous build removed, the stick's old build
                           * renamed to be the previous one */
    UPDATE_WRITE_NEW,     /* the new build written under temporary names, read back */
    UPDATE_WRITE_SWITCH,  /* the stick's build renamed aside, the new one into its names */
    UPDATE_WRITE_MENU,    /* the boot menu checked and written (espmenu.c) */
    UPDATE_WRITE_DONE,    /* all of it, and the ESP read-only again */
    UPDATE_WRITE_STEPS,
};

/* What became of the boot menu (struct update_answer's menu). In every
 * case the stick has a whole boot menu that Limine reads: the new one or
 * the one it had (espmenu.c). */
enum update_menu {
    UPDATE_MENU_NONE,        /* none offered (no `menu` line), or no stick write asked for */
    UPDATE_MENU_WRITTEN,     /* the stick has the new menu; its old one is limine.conf.prev */
    UPDATE_MENU_SAME,        /* the stick had this menu already: nothing written */
    UPDATE_MENU_REFUSED,     /* init's check refused it (menu_why says why): not written */
    UPDATE_MENU_NOT_WRITTEN, /* the write failed (menu_status): the stick keeps its menu */
    UPDATE_MENU_SKIPPED,     /* the build's write failed first: the menu not tried */
    UPDATE_MENU_STATES,
};
#define UPDATE_MENU_WHY_MAX 95u   /* bytes of the answer's menu_why */

/* What the stick boots after a stick write (struct update_answer's stick). */
enum update_stick {
    UPDATE_STICK_NONE,      /* no stick write asked for */
    UPDATE_STICK_OLD,       /* its build, as before (untouched, or put back) */
    UPDATE_STICK_NEW,       /* the new build; the old one as "Jam OS (previous build)" */
    UPDATE_STICK_PREVIOUS,  /* only "Jam OS (previous build)" is sure to boot */
    UPDATE_STICK_NEW_ALONE, /* the new build; "Jam OS (previous build)" may not boot */
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
    uint32_t menu;                            /* UPDATE_OFFER_WRITE: enum update_menu */
    int32_t  menu_status;                     /* ... MENU_NOT_WRITTEN: why */
    char     menu_why[UPDATE_MENU_WHY_MAX + 1]; /* ... MENU_REFUSED: the check's reason */
};

/* "the SHA-256 isn't the manifest's", ...: why, in words. */
const char *update_why_str(uint32_t why);
/* A stick write's step ("writing the new build"), what the stick boots
 * ("the stick boots the new build ..."), and what became of the boot menu
 * ("the boot menu written ..."), in words. */
const char *update_write_step_str(uint32_t step);
const char *update_stick_str(uint32_t stick);
const char *update_menu_str(uint32_t menu);
