/* bin/pkg's parts (docs/M13.7-PLAN.md sections 8 and 9). The program is a
 * native libos one, like bin/update: it fetches the repository's index and
 * packages over HTTP, shows the owner what an install will do, asks once,
 * and writes the files under the packages' prefix (/jam/pkg on a machine
 * with the Jam OS drive's volume, else /data/pkg; only its own view and
 * init's and the shell's may change it). It parses what the
 * network sends, so it holds only the network, the packages' volume, the boot
 * image and the pkgctl channel to init: init decides what may run.
 *
 * The files:
 *   fsx.c      the file calls it writes with: every change to the prefix
 *              goes through here, where a test can cut the power
 *   db.c       what is installed (db/), the accepted serials, the sums
 *   plan.c     what an install, upgrade or remove does and in what order
 *              (pure functions: no file, no network)
 *   unpack.c   a package's bytes as they arrive: checked, and staged
 *   journal.c  the journal's file: written whole, safely
 *   txn.c      one package's install, upgrade or remove, and the settle
 *              of one that was cut short
 *   http.c     one GET over TCP, every wait bounded
 *   repo.c     the index and VULNS, fetched and kept
 *   ctlc.c     init's pkgctl channel as the transaction code sees it
 *   cmds.c     the commands; main.c their arguments and the question
 *
 * The transaction code (fsx, db, plan, unpack, journal, txn) runs over any
 * prefix and any init (struct pk_env), so utest runs it over a RAM disk
 * against init's own pkgapprove.c, cutting the power before every change
 * to the prefix in turn. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <http.h>
#include <os.h>
#include <pkgfmt.h>

#define PK_PATH     400u    /* a prefix, '/', a path (255 bytes), ".new", with the NUL */
#define PK_NAME     64u     /* a name or a version (the grammar's 63) with the NUL */
#define PK_WHY      96u     /* a reason init gives, with the NUL */

/* A package's bytes are pushed into a feed callback; it answers this when it
 * needs no more (the head of a package is all a question needs), and the
 * source stops, closes and says OK. */
#define PK_FEED_END (-1000)

/* init's answer to an approval (pkgctl.approve's results). */
struct pk_verdict {
    status_t verdict;           /* OK, or why it was refused */
    uint32_t programs;          /* lines approved */
    uint32_t program;           /* the program a refusal is about, 0xffffffff: none */
    char     why[PK_WHY];
};

/* init, as the transaction code calls it. */
#define PK_APPROVE_STAGED 1u   /* pkgctl's STAGED and JAM (ctlc.c checks they agree) */
#define PK_APPROVE_JAM    2u

struct pk_ctl {
    void    *ctx;
    /* Approve the package whose signed manifest is text (len bytes);
     * flags: pkgctl's STAGED, and JAM for a prefix on /jam. Returns the
     * call's status; the answer in *v. */
    status_t (*approve)(void *ctx, uint32_t flags, const void *text, size_t len,
                        struct pk_verdict *v);
    /* Take back the lines of (name, version, revision). */
    status_t (*revoke)(void *ctx, const char *name, const char *version, uint32_t revision,
                       uint32_t *lines);
};

/* Everything the transaction code touches. */
struct pk_env {
    const char   *prefix;            /* the packages' prefix: "/jam/pkg", or "/data/pkg" */
    bool          jam;               /* ... the first: init is told (pkgctl's JAM) */
    uint8_t       key[PKG_KEY_BYTES];   /* the ports key (/boot/ports.pub) */
    bool          have_key;
    char          abi[17];           /* this build's stamp; "" if it has none (not checked) */
    struct pk_ctl ctl;
    bool        (*stopped)(void *ctx);   /* Ctrl+C: stop before the commit point (may be NULL) */
    void         *stop_ctx;
    /* For tests: the power goes before the n-th change to the prefix (n counts
     * from 1). Answers 0 to go on, 1 to die before it, 2 to tear it (a write
     * puts half its bytes, then dies; any other change dies before). Once
     * it answered, every change fails with ERR_IO. */
    int         (*cut)(void *ctx, unsigned n, const char *what);
    void         *cut_ctx;
    /* For `pkg -H <point>` (tools/pkg-test.sh quits QEMU there): the journal is
     * at <point> and may stand still as long as it likes. The points are the
     * journal's steps, each in three states (journal.c): "staged:new" (the new
     * text is written beside the old journal), "staged:alone" (the old one is
     * removed, the new not yet renamed: init's settle reads journal.new) and
     * "staged" (replaced); then "done:new" (journal.new removed) and "done"
     * (the journal gone). NULL: not asked. */
    void        (*hold)(void *ctx, const char *point);
    void         *hold_ctx;
    unsigned      ops;               /* changes made so far */
    bool          dead;              /* the power went */
};

/* A text for the owner, one line (stdout of the program). */
void pk_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- fsx.c: the file calls ------------------------------------------------------------- */

/* Paths: <prefix>/<rel><suffix>, and <prefix>/db/<name><suffix>, into out
 * (PK_PATH bytes). */
void     pk_path(const struct pk_env *e, struct pkg_str rel, const char *suffix, char *out);
void     pk_db_path(const struct pk_env *e, const char *name, const char *suffix, char *out);
/* Does a change happen now? PK_GO: yes. PK_DIE: the power went (e->dead): the
 * change must not be made. PK_TEAR (only if tearable): make half of it, then
 * fail with ERR_IO. Every change to the prefix asks first. */
enum { PK_GO, PK_DIE, PK_TEAR };
int      pk_cut(struct pk_env *e, const char *what, bool tearable);
/* A file made whole: created or emptied, written, synced. */
status_t pk_write(struct pk_env *e, const char *path, const void *data, size_t n);
/* A file or an empty directory removed; one that isn't there is fine.
 * ERR_BUSY: a program runs from it. */
status_t pk_remove(struct pk_env *e, const char *path);
/* from renamed to to, which must not be there. */
status_t pk_rename(struct pk_env *e, const char *from, const char *to);
/* The same with whatever is at to removed first (FAT can't rename over a
 * file): two changes, a cut point between them. */
status_t pk_move(struct pk_env *e, const char *from, const char *to);
/* Every directory above file (a path under the prefix), made. */
status_t pk_mkdirs(struct pk_env *e, const char *file);
/* Everything written to the prefix is on its medium. */
status_t pk_flush(struct pk_env *e);
/* The file whole into new memory (*data, NUL-terminated past its n bytes);
 * ERR_OUT_OF_RANGE: more than max. The caller frees it. */
status_t pk_read(const char *path, uint64_t max, uint8_t **data, uint64_t *n);
bool     pk_exists(const char *path);
/* The size and SHA-256 of the file as it reads back. */
status_t pk_hash_file(const char *path, uint64_t *size, uint8_t sha[SHA256_BYTES]);
/* Does s end with suffix (lower case), as FAT compares names? */
bool     pk_ends_fold(struct pkg_str s, const char *suffix);
/* Names the database keeps for itself, which no package may have. */
bool     pk_name_reserved(struct pkg_str name);

/* ---- db.c: what is installed ----------------------------------------------------------- */

struct pk_dep {                      /* a `runs` line */
    char name[PK_NAME];
    char least[PK_NAME];             /* "": any version */
};

struct pk_inst {                     /* an installed package */
    char           name[PK_NAME];
    char           version[PK_NAME];
    uint32_t       revision;
    uint8_t        msha[SHA256_BYTES];    /* of its manifest (db/<name>) */
    uint8_t        jsha[SHA256_BYTES];    /* of the package it came in, if has_jsha */
    bool           has_jsha;
    uint32_t       nruns;
    struct pk_dep *runs;
    bool           broken;           /* db/<name> can't be read as a manifest */
    const char    *why;
};

struct pk_dbset {
    struct pk_inst *v;               /* by name */
    unsigned        n;
};

/* Every package of db/ (none: n 0). Manifests are read without their
 * signatures, which `pkg check` and an install verify: db/ is the program's
 * own. */
status_t pk_db_scan(struct pk_env *e, struct pk_dbset *s);
void     pk_db_free(struct pk_dbset *s);
const struct pk_inst *pk_db_find(const struct pk_dbset *s, struct pkg_str name);
/* db/<name><suffix> ("" or ".new") read and parsed into *m (signature
 * checked when check: with e->key). *text is the file, the manifest points
 * into it; the caller frees it. */
status_t pk_db_load(struct pk_env *e, const char *name, const char *suffix, bool check,
                    uint8_t **text, uint64_t *len, struct pkg_manifest *m);
/* The paths of a manifest's files, hashed, for asking whether another
 * manifest has one of them too (as FAT sees names: letters in either case
 * are the same). */
struct pk_pathset {
    uint64_t                  *h;
    uint32_t                   n;
    const struct pkg_manifest *m;
};
status_t pk_pathset_make(const struct pkg_manifest *m, struct pk_pathset *s);
void     pk_pathset_free(struct pk_pathset *s);
/* The first file of o that the set's manifest has too, or NULL. */
const struct pkg_file *pk_pathset_hit(const struct pk_pathset *s, const struct pkg_manifest *o);
/* Does an installed package other than nm's own have a file of nm's? OK:
 * none. ERR_ALREADY_EXISTS: *owner has *path. */
status_t pk_db_conflict(struct pk_env *e, const struct pk_dbset *db, const struct pkg_manifest *nm,
                        char owner[PK_NAME], char path[PK_PATH]);
/* The sums db/<name>.sum keeps: the SHA-256 of the package a manifest (by its
 * SHA-256) came in. */
bool     pk_sum_get(struct pk_env *e, const char *name, const uint8_t msha[SHA256_BYTES],
                    uint8_t jsha[SHA256_BYTES]);
/* Keep these two entries (the second may be NULL) and no others. */
status_t pk_sum_put(struct pk_env *e, const char *name, const uint8_t m1[SHA256_BYTES],
                    const uint8_t j1[SHA256_BYTES], const uint8_t *m2, const uint8_t *j2);
/* The last index and VULNS accepted (all zero: none yet). ERR_INVALID_ARGS:
 * db/serial is there but not readable. */
status_t pk_last_get(struct pk_env *e, struct pkg_last *index, struct pkg_last *vulns);
status_t pk_last_put(struct pk_env *e, const struct pkg_last *index,
                     const struct pkg_last *vulns);

/* ---- plan.c: what to do, in what order ------------------------------------------------- */

enum pk_kind { PK_INSTALL, PK_UPGRADE, PK_REMOVE };

struct pk_item {
    enum pk_kind           kind;
    const struct pkg_ipkg *ip;       /* the index's package (install, upgrade) */
    const struct pk_inst  *old;      /* the installed one (upgrade, remove) */
};

struct pk_plan {
    struct pk_item *v;               /* in the order they are done */
    unsigned        n, cap;
    const struct pkg_ipkg **have;    /* named, installed, and not older than the index: skipped */
    unsigned        nhave;
    char            why[160];        /* the reason a plan failed */
};

void     pk_plan_free(struct pk_plan *p);
/* Is the index's package newer than the installed one: a higher version, a
 * higher revision, or the same and not the very package that was installed
 * (the signed index says which bytes are current). */
bool     pk_newer(const struct pkg_ipkg *ip, const struct pk_inst *in);
/* names: install these, with what they `runs`, dependencies first. */
status_t pk_plan_install(const struct pkg_index *x, const struct pk_dbset *db,
                         const char *const *names, unsigned n, struct pk_plan *p);
/* names (none: every installed package) at the index's version, with what
 * the new versions `runs`. */
status_t pk_plan_upgrade(const struct pkg_index *x, const struct pk_dbset *db,
                         const char *const *names, unsigned n, struct pk_plan *p);
/* Remove these, dependents first. A package another installed one `runs`
 * is refused unless cascade: then those go too. */
status_t pk_plan_remove(const struct pk_dbset *db, const char *const *names, unsigned n,
                        bool cascade, struct pk_plan *p);

/* ---- unpack.c: a package as it arrives -------------------------------------------------- */

struct pk_unpack;                    /* opaque */

/* What the unpacker is for. */
struct pk_unpack_use {
    struct pk_env *e;
    bool           stage;            /* write the files at `<path>.new` (else only check) */
    const uint8_t *msha;             /* the manifest's SHA-256 it must have (NULL: any) */
};

/* What the format allows but the database can't hold (a name it keeps for
 * itself, a file under db/ or ending in .new); NULL if the manifest is fine. */
const char *pk_manifest_problem(const struct pkg_manifest *m);
struct pk_unpack *pk_unpack_new(const struct pk_unpack_use *use);
/* More of the package's bytes, of any length. PK_FEED_END after the manifest
 * when not staging; else OK, or why it was refused (said in pk_unpack_why). */
status_t pk_unpack_feed(struct pk_unpack *u, const void *data, size_t n);
/* The same as a pk_feed_fn (ctx is the unpacker). */
status_t pk_unpack_sink(void *u, const void *data, size_t n);
/* The package is whole: every member and the end. *size and sha: of every
 * byte fed. */
status_t pk_unpack_finish(struct pk_unpack *u, uint64_t *size, uint8_t sha[SHA256_BYTES]);
/* The manifest, once the head is in (NULL before). It and its text live
 * until pk_unpack_free; take them with pk_unpack_take. */
const struct pkg_manifest *pk_unpack_manifest(const struct pk_unpack *u);
const uint8_t *pk_unpack_text(const struct pk_unpack *u, uint32_t *len);
/* The manifest and its text taken from u: the caller frees both. */
void     pk_unpack_take(struct pk_unpack *u, struct pkg_manifest **m, uint8_t **text,
                        uint32_t *len);
const char *pk_unpack_why(const struct pk_unpack *u);
void     pk_unpack_free(struct pk_unpack *u);

/* A source of a package's bytes: pushes them all into feed (a pk_unpack_feed
 * or a wrapper), stops early when it answers PK_FEED_END (then OK). */
typedef status_t (*pk_feed_fn)(void *ctx, const void *data, size_t n);
struct pk_source {
    void    *ctx;
    status_t (*run)(void *ctx, pk_feed_fn feed, void *feed_ctx);
};
/* The first part of a package, enough for its manifest (checked with the
 * key): *m and *text for the caller to free. why: the reason it was refused. */
status_t pk_head(struct pk_env *e, const struct pk_source *src, struct pkg_manifest **m,
                 uint8_t **text, uint32_t *len, char why[160]);

/* ---- journal.c, txn.c: one package at a time -------------------------------------------- */

/* The journal (db/journal) read: *text is the file (the journal points into it;
 * the caller frees it). ERR_NOT_FOUND: none. ERR_INVALID_ARGS: it can't be read. */
status_t pk_journal_get(struct pk_env *e, struct pkg_journal *j, uint8_t **text,
                        const char **why);
status_t pk_journal_put(struct pk_env *e, const struct pkg_journal *j);
status_t pk_journal_drop(struct pk_env *e);

/* What a transaction was asked to do. */
struct pk_job {
    enum pkg_op            op;
    const char            *name;
    const uint8_t         *text;     /* install, upgrade: the new signed manifest */
    uint32_t               len;
    const struct pk_source *src;     /* ... and where its package comes from */
    uint64_t               size;     /* ... the size and SHA-256 the index names (size 0: none) */
    const uint8_t         *sha;
};

/* Install, upgrade (the package named is installed) or remove one package,
 * through the journal. OK: done. ERR_BUSY: files are in use: the upgrade or
 * removal waits in the journal (say so; the next run finishes it). Else
 * why it failed; before its commit point it is undone first. *why: a reason
 * for the owner when init refused it, "" else. */
status_t pk_run(struct pk_env *e, const struct pk_job *job, char why[PK_WHY]);

/* The journal of a transaction cut short, finished (from its commit point on)
 * or undone. OK: none, or settled. ERR_BUSY: it waits for programs to end. *what: a line
 * about it, "" if there was none. */
status_t pk_settle(struct pk_env *e, char what[PK_PATH]);

/* ---- http.c: one GET ----------------------------------------------------------------------- */

struct pk_get {
    uint64_t want;                   /* the body's length, exactly, if the index named it (0: any) */
    uint64_t max;                    /* the most it may be */
    void   (*progress)(void *ctx, uint64_t got, uint64_t total);   /* may be NULL */
    void    *progress_ctx;
    char     why[160];               /* out: what went wrong, in words ("" if the sink failed) */
};

/* The file rel of the repository at base, its bytes pushed to sink in pieces
 * (PK_FEED_END from it ends the transfer early: OK). Redirects are not
 * followed; every wait has a deadline; the shell's Ctrl+C gives ERR_CANCELED. */
status_t pk_get_run(const struct http_url *base, const char *rel, struct pk_get *g,
                    pk_feed_fn sink, void *ctx);
/* Has the shell asked pkg to stop (Ctrl+C, or the shell gone)? */
bool     pk_net_stopped(void);

/* ---- repo.c: the repository ------------------------------------------------------------------ */

struct pk_repo {
    struct http_url base;            /* its address; the path ends in '/' */
};

/* The repository at url ("http://host[:port][/dir]"). ERR_NOT_SUPPORTED: https. */
status_t pk_repo_set(struct pk_repo *r, const char *url, char why[160]);
/* The index, fetched, its signature and serial checked (force: an older serial is
 * taken, for a repository made again from the start), kept as the last accepted.
 * *text is the index's text, which x points into; the caller frees it. */
status_t pk_repo_index(const struct pk_repo *r, struct pk_env *e, bool force,
                       struct pkg_index *x, uint8_t **text, char why[160]);
status_t pk_repo_vulns(const struct pk_repo *r, struct pk_env *e, struct pkg_vulns *v,
                       uint8_t **text, char why[160]);

/* A package of the repository as a source of bytes. */
struct pk_remote {
    const struct pk_repo *repo;
    char                  rel[PK_PATH];   /* pkg/<name>-<version>-r<revision>-<hash>.jpk */
    uint64_t              size;           /* the index's */
    const char           *label;          /* "vim 9.1.0-r1" for the progress lines */
    bool                  quiet;          /* no progress lines (a head only) */
    unsigned              said;           /* the quarter last said */
};
status_t pk_remote_make(struct pk_remote *rm, const struct pk_repo *r, const struct pkg_ipkg *ip,
                        const char *label);
void     pk_remote_source(struct pk_remote *rm, struct pk_source *src);

/* ---- ctlc.c: init's pkgctl channel ------------------------------------------------------------ */

/* The transaction code's view of init over the channel *ch (the shell's). */
void     pk_ctl_make(struct pk_ctl *c, handle_t *ch);

/* ---- cmds.c, install.c: the commands ------------------------------------------------------- */

/* Exit codes: 0 done, 1 failed or refused, 2 usage, 3 an upgrade or removal waits for a
 * program in use, 130 stopped. */
#define PK_EXIT_BUSY    3
#define PK_EXIT_STOPPED 130

struct pk_app {
    struct pk_env  env;
    struct pk_repo repo;
    bool           have_repo;     /* pk_repo_set took the address */
    char           repo_why[160];
    handle_t       ctl;           /* init's pkgctl channel (0: none: nothing can be installed) */
    handle_t       ask;           /* where the question goes (0: none) */
};

int pk_cmd_list(struct pk_app *a, int argc, char **argv);
int pk_cmd_info(struct pk_app *a, int argc, char **argv);
int pk_cmd_files(struct pk_app *a, int argc, char **argv);
int pk_cmd_check(struct pk_app *a, int argc, char **argv);
int pk_cmd_audit(struct pk_app *a, int argc, char **argv);
int pk_cmd_update(struct pk_app *a, int argc, char **argv);
int pk_cmd_install(struct pk_app *a, int argc, char **argv);
int pk_cmd_upgrade(struct pk_app *a, int argc, char **argv);
int pk_cmd_remove(struct pk_app *a, int argc, char **argv);

/* Shared by the commands. A repository is needed: false (said) without one. */
bool     pk_need_repo(struct pk_app *a);
/* The journal of a cut transaction settled first (said). PK_EXIT_BUSY etc., or 0. */
int      pk_settle_first(struct pk_app *a);
/* The index fetched and checked for the command: *x and *text for the caller to
 * free (x is malloc'd). 0, or the exit code (said). */
int      pk_load_index(struct pk_app *a, bool force, struct pkg_index **x, uint8_t **text);
/* A program's list (the manifest's encoded field) as the owner reads it. */
void     pk_show_list(struct pkg_str field, const char *indent);
/* Bytes as "1.2 MB". */
const char *pk_size_text(uint64_t n, char out[24]);
/* The question asked of the owner on the shell's terminal: true for yes. */
bool     pk_ask(struct pk_app *a, const char *question);
