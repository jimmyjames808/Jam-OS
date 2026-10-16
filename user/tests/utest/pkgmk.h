/* utest: what bin/pkg's tests share (pkgplan.c, pkgtxn.c, pkgcut.c): a fat
 * on a RAM disk at MK_MNT with the packages' prefix, the test vectors' key,
 * init's own approvals (user/services/init/pkgapprove.c) behind the
 * transaction code's pkgctl, and a maker of small signed packages
 * (manifest, files, programs, page tables, the archive) so a test can
 * install, upgrade and remove versions of its own. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>
#include "pkg.h"
#include "pkgapprove.h"

#define MK_MNT    "/pkgx"
#define MK_PREFIX MK_MNT "/pkg"
#define MK_ALLOW  MK_MNT "/etc/allow"
#define MK_FILES  8u

extern const struct pkga_place mk_at;   /* init's places for MK_PREFIX */
extern uint8_t mk_sk[64], mk_pk[32];    /* the vectors' key */

/* A RAM disk's fat at MK_MNT, and the keys. */
bool mk_up(void);
bool mk_down(void);
/* The transaction code's world: MK_PREFIX, the key, init's approvals. */
void mk_env(struct pk_env *e);

struct mk_file {
    const char *path;
    unsigned    mode;          /* 0644 or 0755 */
    const char *text;          /* a data file's contents */
    bool        program;       /* a program: bin/vec's bytes, made different per version */
};

struct mkpkg {
    const char    *name, *version;
    unsigned       revision;
    const char    *runs;       /* "dep" or "dep 1.0": a runs line; NULL: none */
    const char    *list;       /* the manifest's list for its programs (NULL: the ELF's own) */
    unsigned       nfiles;
    struct mk_file files[MK_FILES];
    /* made by mk_build */
    uint8_t       *jpk;
    size_t         jlen;
    uint32_t       mlen;       /* the manifest: jpk + 512, mlen bytes */
    uint8_t        jsha[32];   /* of the package */
    uint8_t        msha[32];   /* of the manifest */
    uint8_t       *data[MK_FILES];
    size_t         size[MK_FILES];
};

/* Make the package, signed with mk_sk, files in path order. */
bool mk_build(struct mkpkg *p);
void mk_free(struct mkpkg *p);

/* A source that pushes the package's bytes into the feed in pieces of chunk
 * bytes (0: all at once). */
struct mk_src {
    const uint8_t *d;
    size_t         n, chunk;
    size_t         stop_at;    /* stop pushing after this many bytes (0: all of it) */
};
void mk_source(struct mk_src *s, const uint8_t *d, size_t n, size_t chunk, struct pk_source *out);
/* A job for the package from its own bytes (size and hash as the index would
 * say). */
void mk_job(struct pk_job *j, enum pkg_op op, const struct mkpkg *p, struct mk_src *s,
            struct pk_source *src);

/* ---- what the disk holds ---------------------------------------------------------------- */

/* Is the package installed exactly: db/<name> its manifest, every file at
 * its path with its bytes and none at `.new`, its programs approved (one line
 * each, tagged with it) and no other line of that name, no journal? */
bool mk_installed(const struct mkpkg *p);
/* Is it not there at all: no record, no file, no `.new`, no line of that name? */
bool mk_absent(const struct mkpkg *p);
/* 1: installed exactly; 0: absent; -1: neither (mk_why says what differs).
 * Prints nothing. */
int mk_state(const struct mkpkg *p);
extern char mk_why[200];
/* Nothing of a transaction is left: no journal, no manifest at db/<name>.new. */
bool mk_clean(void);
/* The approvals' text (NUL-terminated, in a static buffer; "" if none). */
const char *mk_allow(void);
/* How many times needle is in the approvals. */
unsigned mk_allow_count(const char *needle);
bool mk_exists(const char *path);
/* A file's text into a static buffer ("" if none). */
const char *mk_text(const char *path);
