/* utest: bin/pkg's transactions (user/services/pkg: txn.c, unpack.c, db.c)
 * over fat on a RAM disk, against init's own approvals
 * (user/services/init/pkgapprove.c), with packages made here and signed by
 * the test vectors' key (pkgmk.h).
 *
 *   pkgtxn_install    a package installed: files, record, approvals, sums;
 *                     again: refused; fed in pieces of any size
 *   pkgtxn_refused    nothing is left of a package that is changed in
 *                     transit, is not the index's, is signed by another key,
 *                     is stopped by Ctrl+C, or is cut short
 *   pkgtxn_init_no    nothing is left of a package init refuses, or that
 *                     asks for a name or a path the database keeps
 *   pkgtxn_upgrade    a new version replaces the old: changed, added and
 *                     removed files, the old approvals revoked, the sum;
 *                     the same version rebuilt is newer by its hash
 *   pkgtxn_remove     a package removed: files, empty directories, record
 *   pkgtxn_hold       the journal's hold points (`pkg -H`), in order, for an install,
 *                     an upgrade and a removal
 *   pkgtxn_busy       an upgrade and a removal that wait for a program in use
 *   pkgtxn_db         the serials and sums db/ keeps */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <sha256.h>
#include "pkgmk.h"
#include "utest.h"

/* aaa at (version, revision): a program, a data file and a licence. */
static struct mkpkg make_a(const char *version, unsigned revision)
{
    struct mkpkg p = { .name = "aaa", .version = version, .revision = revision };
    p.files[p.nfiles++] = (struct mk_file){ "bin/aaa", 0755, NULL, true };
    p.files[p.nfiles++] = (struct mk_file){ "share/aaa/readme.txt", 0644, "hello a\n", false };
    p.files[p.nfiles++] = (struct mk_file){ "share/licences/aaa/LICENSE", 0644, "MIT\n", false };
    return p;
}

/* Run op on p (its package from memory, in pieces of chunk bytes). */
static status_t run_op(struct pk_env *e, enum pkg_op op, const struct mkpkg *p, size_t chunk,
                       char why[PK_WHY])
{
    struct mk_src s;
    struct pk_source src;
    struct pk_job job;
    mk_job(&job, op, p, &s, &src);
    s.chunk = chunk;
    return pk_run(e, &job, why);
}

/* A program past the old 64 MiB cap, through the whole path: bin/pkg's code
 * unpacks it in pieces, init's approve reads it back in pieces (never whole:
 * the pages it adds to the job are a few, not the program's 17,921), the
 * line and the table are init's; removed again. */
bool t_pkgtxn_big(void)
{
    enum { BIG_MIB = 70, DISK_MIB = 160, ADDED_MAX = 1024 };   /* pages: 4 MiB */
    struct mkpkg b = { .name = "big", .version = "1.0", .revision = 1, .pad = (size_t)BIG_MIB << 20 };
    b.files[b.nfiles++] = (struct mk_file){ "bin/big", 0755, NULL, true };
    b.files[b.nfiles++] = (struct mk_file){ "share/big/readme.txt", 0644, "big\n", false };
    struct pk_env e;
    char why[PK_WHY];
    CHECK(mk_up_mib(DISK_MIB) && mk_build(&b));
    CHECK(b.size[0] > (64u << 20));
    mk_env(&e);
    mk_track_approve = true;
    status_t st = run_op(&e, PKG_OP_INSTALL, &b, 65536, why);
    mk_track_approve = false;
    CHECK_ST(st, OK);
    printf("utest: %s: approve added %lu pages to the job (the program is %lu)\n", utest_cur,
           (unsigned long)mk_approve_peak, (unsigned long)(b.size[0] / PAGE_SIZE));
    CHECK(mk_approve_peak <= ADDED_MAX);
    CHECK(mk_installed(&b));
    CHECK_ST(run_op(&e, PKG_OP_REMOVE, &b, 0, why), OK);
    CHECK(mk_absent(&b));
    mk_free(&b);
    return mk_down();
}

bool t_pkgtxn_install(void)
{
    struct mkpkg a = make_a("1.0", 1);
    struct pk_env e;
    char why[PK_WHY];
    struct pk_dbset db;
    CHECK(mk_up() && mk_build(&a));
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 3000, why), OK);
    CHECK(mk_installed(&a));
    CHECK_ST(pk_db_scan(&e, &db), OK);
    CHECK_EQ(db.n, 1);
    CHECK(!strcmp(db.v[0].name, "aaa") && !strcmp(db.v[0].version, "1.0") && !db.v[0].broken);
    CHECK_EQ(db.v[0].revision, 1);
    CHECK(db.v[0].has_jsha && !memcmp(db.v[0].jsha, a.jsha, 32));
    CHECK(!memcmp(db.v[0].msha, a.msha, 32));
    pk_db_free(&db);
    /* Installed already: refused, and nothing changed. */
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 3000, why), ERR_ALREADY_EXISTS);
    CHECK(mk_installed(&a));
    mk_free(&a);
    /* The same bytes in pieces of every kind: one at a time, odd sizes, whole. */
    static const size_t chunks[] = { 1, 7, 511, 513, 4096, 100000 };
    struct mkpkg b = make_a("2.0", 1);
    b.name = "bbb";
    b.files[0].path = "bin/bbb";
    b.files[1].path = "share/bbb/readme.txt";
    b.files[2].path = "share/licences/bbb/LICENSE";
    CHECK(mk_build(&b));
    for (unsigned i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        CHECK_ST(run_op(&e, PKG_OP_INSTALL, &b, chunks[i], why), OK);
        CHECK(mk_installed(&b));
        CHECK_ST(run_op(&e, PKG_OP_REMOVE, &b, 0, why), OK);
        CHECK(mk_absent(&b));
    }
    mk_free(&b);
    return mk_down();
}

/* ---- refusals ----------------------------------------------------------------------------- */

struct tweak {
    size_t   flip;         /* the package's byte at this offset changed (0: none) */
    bool     wrong_sha;    /* the index names other bytes */
    bool     other_key;    /* the build's key is not the one that signed it */
    unsigned stop_after;   /* Ctrl+C at this member header (0: never) */
    size_t   cut_at;       /* the source ends after this many bytes (0: whole) */
};

static unsigned stop_count;
static bool stop_now(void *ctx)
{
    (void)ctx;
    return stop_count && --stop_count == 0;
}

/* p installed under the tweak: it fails (with want if given), and nothing of
 * it is left. */
static bool refused(struct mkpkg *p, const struct tweak *t, status_t want)
{
    struct pk_env e;
    struct pk_job job;
    struct mk_src s;
    struct pk_source src;
    static uint8_t other[32];
    char why[PK_WHY];
    mk_env(&e);
    mk_job(&job, PKG_OP_INSTALL, p, &s, &src);
    if (t->flip)
        p->jpk[t->flip] ^= 0x40;
    if (t->wrong_sha) {
        memset(other, 7, sizeof(other));
        job.sha = other;
    }
    if (t->other_key)
        e.key[0] ^= 1;
    if (t->stop_after) {
        stop_count = t->stop_after;
        e.stopped = stop_now;
    }
    s.stop_at = t->cut_at;
    status_t st = pk_run(&e, &job, why);
    if (t->flip)
        p->jpk[t->flip] ^= 0x40;
    stop_count = 0;
    if (want)
        CHECK_ST(st, want);
    else
        CHECK(st != OK);
    CHECK(mk_absent(p));
    return true;
}

bool t_pkgtxn_refused(void)
{
    struct mkpkg a = make_a("1.0", 1);
    struct pk_env e;
    char why[PK_WHY];
    CHECK(mk_up() && mk_build(&a));
    /* A byte changed in the manifest, a header, a file, a table, the end. */
    size_t file0 = 512 + (((size_t)a.mlen + 511) & ~(size_t)511) + 512;
    CHECK(refused(&a, &(struct tweak){ .flip = 512 + 20 }, ERR_ACCESS_DENIED));
    CHECK(refused(&a, &(struct tweak){ .flip = file0 - 512 + 3 }, ERR_INVALID_ARGS));
    CHECK(refused(&a, &(struct tweak){ .flip = file0 + 5 }, ERR_ACCESS_DENIED));
    CHECK(refused(&a, &(struct tweak){ .flip = a.jlen - 1024 - 512 + 70 }, ERR_ACCESS_DENIED));
    CHECK(refused(&a, &(struct tweak){ .flip = a.jlen - 3 }, 0));
    /* The index names other bytes, the key is another, Ctrl+C, a package cut short. */
    CHECK(refused(&a, &(struct tweak){ .wrong_sha = true }, ERR_ACCESS_DENIED));
    CHECK(refused(&a, &(struct tweak){ .other_key = true }, ERR_ACCESS_DENIED));
    CHECK(refused(&a, &(struct tweak){ .stop_after = 2 }, ERR_CANCELED));
    CHECK(refused(&a, &(struct tweak){ .cut_at = a.jlen - 700 }, ERR_INVALID_ARGS));
    CHECK(refused(&a, &(struct tweak){ .cut_at = 300 }, ERR_INVALID_ARGS));
    /* After all that the package installs. */
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 0, why), OK);
    CHECK(mk_installed(&a));
    mk_free(&a);
    return mk_down();
}

bool t_pkgtxn_init_no(void)
{
    CHECK(mk_up());
    /* init refuses the program: the manifest's list is not the ELF's own. */
    struct mkpkg a = make_a("1.0", 1);
    a.list = "posix\\x0a";
    CHECK(mk_build(&a));
    struct pk_env e;
    char why[PK_WHY];
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 0, why), ERR_ACCESS_DENIED);
    CHECK(why[0] != '\0');
    CHECK(mk_absent(&a));
    mk_free(&a);
    /* A name the database keeps, a path under db/, a path ending in .new. */
    struct mkpkg r = make_a("1.0", 1);
    r.name = "serial";
    CHECK(mk_build(&r));
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &r, 0, why), ERR_ACCESS_DENIED);
    mk_free(&r);
    struct mkpkg d = make_a("1.0", 1);
    d.files[1].path = "db/aaa.txt";
    CHECK(mk_build(&d));
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &d, 0, why), ERR_ACCESS_DENIED);
    CHECK(mk_absent(&d));
    mk_free(&d);
    struct mkpkg n = make_a("1.0", 1);
    n.files[1].path = "share/aaa/readme.NEW";
    CHECK(mk_build(&n));
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &n, 0, why), ERR_ACCESS_DENIED);
    CHECK(mk_absent(&n));
    mk_free(&n);
    CHECK(mk_clean());
    return mk_down();
}

/* ---- upgrade ------------------------------------------------------------------------------ */

/* aaa 2.0: bin/aaa changed, readme changed, old.txt gone, new.txt added. */
static struct mkpkg make_a1(void)
{
    struct mkpkg p = make_a("1.0", 1);
    p.files[p.nfiles++] = (struct mk_file){ "share/aaa/old.txt", 0644, "old\n", false };
    return p;
}

static struct mkpkg make_a2(const char *readme)
{
    struct mkpkg p = make_a("2.0", 1);
    p.files[1].text = readme;
    p.files[p.nfiles++] = (struct mk_file){ "share/aaa/new.txt", 0644, "new\n", false };
    return p;
}

bool t_pkgtxn_upgrade(void)
{
    struct mkpkg v1 = make_a1(), v2 = make_a2("hello a, again\n"), v2b = make_a2("rebuilt\n");
    struct pk_env e;
    char why[PK_WHY];
    CHECK(mk_up() && mk_build(&v1) && mk_build(&v2) && mk_build(&v2b));
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1, 0, why), OK);
    CHECK(mk_installed(&v1));
    /* An upgrade needs the package installed. */
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v2, 0, why), OK);
    CHECK(mk_installed(&v2));
    CHECK(!mk_exists(MK_PREFIX "/share/aaa/old.txt"));
    CHECK(mk_exists(MK_PREFIX "/share/aaa/new.txt"));
    CHECK_EQ(mk_allow_count("pkg=aaa 1.0-r1"), 0);
    /* The same version rebuilt: newer by its hash only. */
    struct pk_dbset db;
    CHECK_ST(pk_db_scan(&e, &db), OK);
    struct pkg_ipkg ip = { .name = { "aaa", 3 }, .version = { "2.0", 3 }, .revision = 1,
                           .size = v2b.jlen };
    memcpy(ip.sha256, v2b.jsha, 32);
    CHECK(pk_newer(&ip, &db.v[0]));
    memcpy(ip.sha256, v2.jsha, 32);
    CHECK(!pk_newer(&ip, &db.v[0]));
    ip.revision = 2;
    CHECK(pk_newer(&ip, &db.v[0]));
    ip.revision = 1;
    ip.version = (struct pkg_str){ "1.9", 3 };
    CHECK(!pk_newer(&ip, &db.v[0]));
    pk_db_free(&db);
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v2b, 0, why), OK);
    CHECK(mk_installed(&v2b));
    uint8_t j[32];
    CHECK(pk_sum_get(&e, "aaa", v2b.msha, j) && !memcmp(j, v2b.jsha, 32));
    CHECK(!pk_sum_get(&e, "aaa", v2.msha, j));
    /* And back to 1.0: a downgrade is the same steps. */
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v1, 0, why), OK);
    CHECK(mk_installed(&v1));
    CHECK(mk_exists(MK_PREFIX "/share/aaa/old.txt") && !mk_exists(MK_PREFIX "/share/aaa/new.txt"));
    /* Not installed: an upgrade is refused. */
    struct mkpkg b = make_a("1.0", 1);
    b.name = "bbb";
    b.files[0].path = "bin/bbb";
    b.files[1].path = "share/bbb/readme.txt";
    b.files[2].path = "share/licences/bbb/LICENSE";
    CHECK(mk_build(&b));
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &b, 0, why), ERR_NOT_FOUND);
    CHECK(mk_absent(&b));
    mk_free(&b);
    mk_free(&v1);
    mk_free(&v2);
    mk_free(&v2b);
    return mk_down();
}

/* ---- remove ------------------------------------------------------------------------------- */

bool t_pkgtxn_remove(void)
{
    struct mkpkg a = make_a1();
    struct pk_env e;
    char why[PK_WHY];
    CHECK(mk_up() && mk_build(&a));
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 0, why), OK);
    CHECK(mk_exists(MK_PREFIX "/share/aaa"));
    CHECK_ST(run_op(&e, PKG_OP_REMOVE, &a, 0, why), OK);
    CHECK(mk_absent(&a));
    CHECK(!mk_exists(MK_PREFIX "/share/aaa") && !mk_exists(MK_PREFIX "/bin"));
    CHECK(!mk_exists(MK_PREFIX "/db/aaa.sum"));
    /* Not installed: refused. */
    CHECK_ST(run_op(&e, PKG_OP_REMOVE, &a, 0, why), ERR_NOT_FOUND);
    mk_free(&a);
    return mk_down();
}

/* ---- the journal's hold points (`pkg -H <point>`, tools/pkg-test.sh) ------------------------ */

struct points {
    char     at[40][24];
    unsigned n;
};

static void note_point(void *ctx, const char *point)
{
    struct points *p = ctx;
    if (p->n < sizeof(p->at) / sizeof(p->at[0]))
        snprintf(p->at[p->n++], sizeof(p->at[0]), "%s", point);
}

/* Did the journal stand at <step>:new, <step>:alone and <step> for each of the n steps, in
 * order, and then at done:new and done? */
static bool steps_are(const struct points *p, const char *const *steps, unsigned n)
{
    static const char *const state[] = { ":new", ":alone", "" };
    if (p->n != n * 3 + 2)
        return false;
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < 3; k++) {
            char want[24];
            snprintf(want, sizeof(want), "%s%s", steps[i], state[k]);
            if (strcmp(p->at[i * 3 + k], want))
                return false;
        }
    return !strcmp(p->at[n * 3], "done:new") && !strcmp(p->at[n * 3 + 1], "done");
}

bool t_pkgtxn_hold(void)
{
    static const char *const install[] = { "begin", "staged", "approved", "swapped", "recorded" };
    static const char *const upgrade[] = { "begin", "staged", "approved", "swapped", "revoked",
                                           "recorded" };
    static const char *const remove[] = { "begin", "revoked", "removed", "recorded" };
    struct mkpkg v1 = make_a1(), v2 = make_a2("hello a, again\n");
    struct pk_env e;
    struct points p = { .n = 0 };
    char why[PK_WHY];
    CHECK(mk_up() && mk_build(&v1) && mk_build(&v2));
    mk_env(&e);
    e.hold = note_point;
    e.hold_ctx = &p;
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1, 0, why), OK);
    CHECK(mk_installed(&v1) && steps_are(&p, install, 5));
    p.n = 0;
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v2, 0, why), OK);
    CHECK(mk_installed(&v2) && steps_are(&p, upgrade, 6));
    p.n = 0;
    CHECK_ST(run_op(&e, PKG_OP_REMOVE, &v2, 0, why), OK);
    CHECK(mk_absent(&v2) && steps_are(&p, remove, 4));
    mk_free(&v1);
    mk_free(&v2);
    return mk_down();
}

/* ---- files in use --------------------------------------------------------------------- */

bool t_pkgtxn_busy(void)
{
    struct mkpkg v1 = make_a1(), v2 = make_a2("hello a, again\n");
    struct pk_env e;
    char why[PK_WHY], what[PK_PATH];
    CHECK(mk_up() && mk_build(&v1) && mk_build(&v2));
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1, 0, why), OK);
    /* A program holds a file open: fat won't remove it. */
    struct jfile held;
    CHECK_ST(file_open(MK_PREFIX "/share/aaa/readme.txt", FS_READ, &held), OK);
    status_t st = fs_unlink(MK_PREFIX "/share/aaa/readme.txt");
    if (st != ERR_BUSY) {
        file_close(&held);
        printf("utest: %s: fat removes an open file (%s): the busy case can't be made here: "
               "skipped\n", utest_cur, status_str(st));
        mk_free(&v1);
        mk_free(&v2);
        return mk_down();
    }
    CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v2, 0, why), ERR_BUSY);
    /* The journal waits, at the commit point, naming the file; the old approval stays
     * until the swap is done; the new one is there. */
    struct pkg_journal *j = calloc(1, sizeof(*j));
    uint8_t *jt;
    const char *jwhy;
    CHECK(j);
    CHECK_ST(pk_journal_get(&e, j, &jt, &jwhy), OK);
    CHECK_EQ(j->step, PKG_STEP_APPROVED);
    CHECK_EQ(j->nbusy, 1);
    CHECK(pkg_str_eq(j->busy[0], "share/aaa/readme.txt"));
    free(jt);
    free(j);
    CHECK(mk_exists(MK_PREFIX "/share/aaa/readme.txt.new"));
    CHECK_EQ(mk_allow_count("pkg=aaa 1.0-r1"), 1);
    CHECK_EQ(mk_allow_count("pkg=aaa 2.0-r1"), 1);
    /* Still in use: the settle says so and changes nothing more. */
    CHECK_ST(pk_settle(&e, what), ERR_BUSY);
    /* Closed: the settle finishes it. */
    file_close(&held);
    CHECK_ST(pk_settle(&e, what), OK);
    CHECK(mk_installed(&v2));
    CHECK_ST(pk_settle(&e, what), OK);
    CHECK(what[0] == '\0');
    mk_free(&v1);
    mk_free(&v2);
    return mk_down();
}

/* ---- dependencies as installed -------------------------------------------------------------- */

bool t_pkgtxn_runs(void)
{
    struct mkpkg a = make_a("1.0", 1), b = make_a("2.0", 1);
    struct pk_env e;
    struct pk_dbset db;
    struct pk_plan p;
    char why[PK_WHY];
    b.name = "bbb";
    b.runs = "aaa 1.0";
    b.files[0].path = "bin/bbb";
    b.files[1].path = "share/bbb/readme.txt";
    b.files[2].path = "share/licences/bbb/LICENSE";
    CHECK(mk_up() && mk_build(&a) && mk_build(&b));
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &a, 0, why), OK);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &b, 0, why), OK);
    /* What db/ says bbb runs, read back from its manifest. */
    CHECK_ST(pk_db_scan(&e, &db), OK);
    CHECK_EQ(db.n, 2);
    CHECK(!strcmp(db.v[1].name, "bbb") && db.v[1].nruns == 1);
    CHECK(!strcmp(db.v[1].runs[0].name, "aaa") && !strcmp(db.v[1].runs[0].least, "1.0"));
    CHECK_EQ(db.v[0].nruns, 0);
    const char *an[] = { "aaa" };
    CHECK_ST(pk_plan_remove(&db, an, 1, false, &p), ERR_BAD_STATE);
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, an, 1, true, &p), OK);
    CHECK_EQ(p.n, 2);
    CHECK(!strcmp(p.v[0].old->name, "bbb") && !strcmp(p.v[1].old->name, "aaa"));
    pk_plan_free(&p);
    pk_db_free(&db);
    mk_free(&a);
    mk_free(&b);
    return mk_down();
}

/* ---- db ------------------------------------------------------------------------------------ */

bool t_pkgtxn_db(void)
{
    struct pk_env e;
    struct pkg_last i, v, i2, v2;
    CHECK(mk_up());
    mk_env(&e);
    CHECK_ST(pk_last_get(&e, &i, &v), OK);
    CHECK(i.serial == 0 && v.serial == 0);
    memset(&i, 0, sizeof(i));
    memset(&v, 0, sizeof(v));
    i.serial = 412;
    memset(i.sha256, 0xab, 32);
    CHECK_ST(pk_last_put(&e, &i, &v), OK);
    CHECK_ST(pk_last_get(&e, &i2, &v2), OK);
    CHECK(i2.serial == 412 && !memcmp(i2.sha256, i.sha256, 32) && v2.serial == 0);
    v.serial = 99;
    memset(v.sha256, 0xcd, 32);
    CHECK_ST(pk_last_put(&e, &i, &v), OK);
    CHECK_ST(pk_last_get(&e, &i2, &v2), OK);
    CHECK(i2.serial == 412 && v2.serial == 99 && !memcmp(v2.sha256, v.sha256, 32));
    /* Cut between the old one's removal and the new one's rename: serial.new is read. */
    CHECK_ST(fs_rename(MK_PREFIX "/db/serial", MK_PREFIX "/db/serial.new"), OK);
    CHECK_ST(pk_last_get(&e, &i2, &v2), OK);
    CHECK(i2.serial == 412 && v2.serial == 99);
    CHECK_ST(fs_rename(MK_PREFIX "/db/serial.new", MK_PREFIX "/db/serial"), OK);
    /* A damaged file is an error, never "nothing accepted". */
    CHECK_ST(pk_write(&e, MK_PREFIX "/db/serial", "index 4", 7), OK);
    CHECK_ST(pk_last_get(&e, &i2, &v2), ERR_INVALID_ARGS);
    CHECK(i2.serial == 0);
    CHECK_ST(pk_write(&e, MK_PREFIX "/db/serial", "", 0), OK);
    CHECK_ST(pk_last_get(&e, &i2, &v2), OK);
    /* Sums: a line whose manifest is not the asked one is not an answer. */
    uint8_t m1[32], j1[32], m2[32], j2[32], got[32];
    memset(m1, 1, 32);
    memset(j1, 2, 32);
    memset(m2, 3, 32);
    memset(j2, 4, 32);
    CHECK_ST(pk_sum_put(&e, "zed", m1, j1, m2, j2), OK);
    CHECK(pk_sum_get(&e, "zed", m1, got) && !memcmp(got, j1, 32));
    CHECK(pk_sum_get(&e, "zed", m2, got) && !memcmp(got, j2, 32));
    CHECK(!pk_sum_get(&e, "zed", j1, got));
    CHECK_ST(pk_sum_put(&e, "zed", m2, j2, NULL, NULL), OK);
    CHECK(!pk_sum_get(&e, "zed", m1, got) && pk_sum_get(&e, "zed", m2, got));
    return mk_down();
}
