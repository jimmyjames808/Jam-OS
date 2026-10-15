/* init's approvals of a package's programs, their revocation, and the
 * settling of a package transaction cut short (pkgapprove.c;
 * abi/idl/pkgctl.idl, docs/M13.7-PLAN.md section 9). The work alone, on
 * paths given, so utest runs it over a RAM filesystem with the test
 * vectors' key; pkg.c serves it to bin/pkg on init's worker thread with
 * /boot/ports.pub and the real paths. Blocking file I/O throughout: never
 * from init's loop. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <pkgfmt.h>

/* Where things are. */
struct pkga_place {
    const char *prefix;   /* the packages' prefix: "/data/pkg" */
    const char *etc;      /* the approvals' directory: "/data/etc" (DP_ALLOW_DIR) */
    const char *allow;    /* the approvals: "/data/etc/allow" (DP_ALLOW_FILE) */
    const char *tables;   /* their page tables: "/data/etc/allow.d" (DP_TABLE_DIR) */
};

#define PKGA_STAGED  1u          /* pkgctl's STAGED: the programs are at `<path>.new` */
#define PKGA_NONE    0xffffffffu /* no program */
#define PKGA_WHY_MAX 96          /* a reason, its NUL included (pkgctl's WHY_MAX + 1) */
#define PKGA_TAG_MAX (4 + 63 + 1 + 63 + 2 + 10 + 1)   /* "pkg=<name> <version>-r<revision>" */

/* An approval's answer. */
struct pkga_result {
    status_t verdict;              /* OK, or why it was refused (pkgctl.approve's) */
    uint32_t programs;             /* lines approved (OK) */
    uint32_t program;              /* the program a refusal is about, or PKGA_NONE */
    char     why[PKGA_WHY_MAX];    /* for the owner; "" on OK */
};

/* The tag of a package's lines: "pkg=<name> <version>-r<revision>" into
 * out (PKGA_TAG_MAX bytes). One tag names one (name, version, revision):
 * a name has no space, and a revision no "-r". */
void     pkga_tag(struct pkg_str name, struct pkg_str version, uint32_t revision, char *out);

/* Approve the programs of the package whose signed manifest is the len
 * bytes at text (init's own copy), key the ports key: the signature, then
 * each program read back from the prefix (at `<path>.new` with
 * PKGA_STAGED) into memory of our own and checked from those bytes (its
 * size and SHA-256, its page table's SHA-256, its own list against the
 * manifest's, the refusals of dp_wants_refused), and only when all
 * passed, the tables and then the lines, tagged. *r says how it went; the
 * return is r->verdict. Nothing is written on a refusal. */
status_t pkga_approve(const struct pkga_place *at, const uint8_t key[PKG_KEY_BYTES],
                      const void *text, size_t len, uint32_t flags, struct pkga_result *r);

/* Take back the lines tagged with the package (name, version, revision),
 * and the tables no line names any more; *lines: how many went.
 * ERR_INVALID_ARGS: a name or version of 0 or more than 63 bytes, a byte
 * outside 0x21..0x7e in one, or revision 0. */
status_t pkga_revoke(const struct pkga_place *at, struct pkg_str name, struct pkg_str version,
                     uint32_t revision, uint32_t *lines);

/* What a settle did, for the log. */
struct pkga_settled {
    bool        journal;           /* there was one */
    bool        forward;           /* finished (else undone) */
    status_t    st;                /* OK: the journal is gone; else why it stays */
    char        what[PKGA_WHY_MAX];   /* "install vim 9.1.0-r1 finished", or why not */
};

/* The transaction the journal (<prefix>/db/journal) names, after a cut:
 * finished from its commit point on (an install's or an upgrade's
 * `approved`, a remove's `begin`), undone before it, then the journal
 * removed. Every file it touches is one a signed manifest names (key; NULL:
 * a build without a ports key, which settles nothing), and every step is
 * one that can be done again, so a cut during the settle is settled the
 * same way next time. */
void     pkga_settle(const struct pkga_place *at, const uint8_t *key, struct pkga_settled *out);
