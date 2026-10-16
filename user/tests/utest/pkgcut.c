/* utest: bin/pkg's transactions cut at every change (user/services/pkg: txn.c).
 * An install, an upgrade and a removal are run with the power taken away
 * before the n-th change to the prefix, for every n (and, where the change
 * is a write, with half of it written), on a RAM disk with init's real
 * approvals. Then the next boot: init's settle (pkgapprove.c, the journal as
 * it finds it), or bin/pkg's own at its next start. Either way the package is
 * exactly the old one or exactly the new one (every file as packaged, its
 * record, its approvals and no others, nothing left over), and the two
 * settles go the same way for the same cut.
 *
 *   pkgcut_install, pkgcut_upgrade, pkgcut_remove  each, cut once
 *   pkgcut_double   an upgrade cut, and its settle cut too, at every change */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "pkgmk.h"
#include "utest.h"

#define MAX_OPS 256u

struct cutter {
    unsigned at;                 /* die before this change (0: never) */
    int      mode;               /* what pk_cut answers there: 1 die, 2 tear */
    unsigned seen;               /* changes asked about */
    char     what[MAX_OPS][10];  /* what each was */
};

static int cut_cb(void *ctx, unsigned n, const char *what)
{
    struct cutter *c = ctx;
    if (n < MAX_OPS)
        snprintf(c->what[n], sizeof(c->what[n]), "%s", what);
    c->seen = n;
    return n == c->at ? c->mode : 0;
}

enum scen { S_INSTALL, S_UPGRADE, S_REMOVE };

static struct mkpkg v1, v2;

/* v1: a program, data, a licence and a file v2 lacks. v2: the program and a
 * data file changed, a file added. */
static void make_versions(void)
{
    v1 = (struct mkpkg){ .name = "aaa", .version = "1.0", .revision = 1 };
    v1.files[v1.nfiles++] = (struct mk_file){ "bin/aaa", 0755, NULL, true };
    v1.files[v1.nfiles++] = (struct mk_file){ "share/aaa/readme.txt", 0644, "one\n", false };
    v1.files[v1.nfiles++] = (struct mk_file){ "share/aaa/old.txt", 0644, "old\n", false };
    v1.files[v1.nfiles++] = (struct mk_file){ "share/licences/aaa/LICENSE", 0644, "MIT\n", false };
    v2 = (struct mkpkg){ .name = "aaa", .version = "2.0", .revision = 1 };
    v2.files[v2.nfiles++] = (struct mk_file){ "bin/aaa", 0755, NULL, true };
    v2.files[v2.nfiles++] = (struct mk_file){ "share/aaa/readme.txt", 0644, "two\n", false };
    v2.files[v2.nfiles++] = (struct mk_file){ "share/aaa/new.txt", 0644, "new\n", false };
    v2.files[v2.nfiles++] = (struct mk_file){ "share/licences/aaa/LICENSE", 0644, "MIT\n", false };
}

static status_t run_op(struct pk_env *e, enum pkg_op op, const struct mkpkg *p)
{
    struct mk_src s;
    struct pk_source src;
    struct pk_job job;
    char why[PK_WHY];
    mk_job(&job, op, p, &s, &src);
    return pk_run(e, &job, why);
}

/* The scenario's operation, in e. */
static status_t operate(enum scen s, struct pk_env *e)
{
    return s == S_INSTALL ? run_op(e, PKG_OP_INSTALL, &v1) :
           s == S_UPGRADE ? run_op(e, PKG_OP_UPGRADE, &v2) : run_op(e, PKG_OP_REMOVE, &v1);
}

/* 0: the state before the operation, 1: after it, -1: neither. */
static int where(enum scen s)
{
    int a = mk_state(&v1);
    if (s == S_INSTALL)
        return a;
    if (s == S_REMOVE)
        return a == 1 ? 0 : a == 0 ? 1 : -1;
    return a == 1 ? 0 : mk_state(&v2) == 1 ? 1 : -1;
}

/* Back to the state before the operation, by operations that aren't cut. */
static bool reset(enum scen s)
{
    struct pk_env e;
    mk_env(&e);
    if (where(s) == 0)
        return true;
    if (s == S_INSTALL)
        CHECK_ST(run_op(&e, PKG_OP_REMOVE, &v1), OK);
    else if (s == S_UPGRADE)
        CHECK_ST(run_op(&e, PKG_OP_UPGRADE, &v1), OK);
    else
        CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1), OK);
    CHECK_EQ(where(s), 0);
    return true;
}

/* The next boot: init's settle (the journal as it finds it), or bin/pkg's at
 * its next start. */
static bool reboot(bool by_init)
{
    struct pk_env e;
    char what[PK_PATH];
    mk_env(&e);
    if (by_init) {
        struct pkga_settled out;
        pkga_settle(&mk_at, mk_pk, &out);
        if (out.journal && out.st != OK)
            FAIL("init could not settle it: %s", out.what);
        return true;
    }
    CHECK_ST(pk_settle(&e, what), OK);
    return true;
}

/* Count the operation's changes, uncut. */
static bool count_ops(enum scen s, struct cutter *c, unsigned *total)
{
    struct pk_env e;
    mk_env(&e);
    *c = (struct cutter){ 0 };
    e.cut = cut_cb;
    e.cut_ctx = c;
    CHECK_ST(operate(s, &e), OK);
    CHECK(c->seen >= 10 && c->seen < MAX_OPS);
    *total = c->seen;
    CHECK_EQ(where(s), 1);
    return true;
}

static int outcome[2][MAX_OPS];   /* [by_init][n]: where(s) after the settle, 2: not run */

static bool one_cut(enum scen s, unsigned n, int mode, bool by_init, const struct cutter *known)
{
    struct pk_env e;
    struct cutter c = { .at = n, .mode = mode };
    CHECK(reset(s));
    mk_env(&e);
    e.cut = cut_cb;
    e.cut_ctx = &c;
    (void)operate(s, &e);
    CHECK(e.dead && c.seen == n);
    CHECK(reboot(by_init));
    int w = where(s);
    if (w < 0)
        FAIL("after a cut before change %u (%s, %s) and %s's settle: %s", n, known->what[n],
             mode == 2 ? "torn" : "whole", by_init ? "init" : "pkg", mk_why);
    if (mode == 1)
        outcome[by_init][n] = w;
    return true;
}

static bool cut_all(enum scen s)
{
    struct cutter known;
    unsigned total;
    make_versions();
    CHECK(mk_up() && mk_build(&v1) && mk_build(&v2));
    struct pk_env e;
    mk_env(&e);
    if (s != S_INSTALL)
        CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1), OK);
    CHECK(count_ops(s, &known, &total));
    printf("utest: %s: %u changes\n", utest_cur, total);
    for (unsigned n = 1; n <= total; n++) {
        for (int init = 0; init <= 1; init++)
            CHECK(one_cut(s, n, 1, init, &known));
        CHECK_EQ(outcome[0][n], outcome[1][n]);   /* both settles go the same way */
        if (!strcmp(known.what[n], "write"))
            for (int init = 0; init <= 1; init++)
                CHECK(one_cut(s, n, 2, init, &known));
    }
    /* A cut at every change leaves both ends reachable: the first change
     * leaves the old state, the last one the new. */
    CHECK_EQ(outcome[0][1], 0);
    CHECK_EQ(outcome[0][total], 1);
    CHECK(reset(s));
    mk_free(&v1);
    mk_free(&v2);
    return mk_down();
}

bool t_pkgcut_install(void)
{
    return cut_all(S_INSTALL);
}

bool t_pkgcut_upgrade(void)
{
    return cut_all(S_UPGRADE);
}

bool t_pkgcut_remove(void)
{
    return cut_all(S_REMOVE);
}

/* An upgrade cut at every fourth change, then its settle cut again and again
 * (at the first change, then the second of what is left, and so on until a
 * settle finishes), then settled: the package is the old one or the new one. */
bool t_pkgcut_double(void)
{
    struct cutter known;
    unsigned total;
    make_versions();
    CHECK(mk_up() && mk_build(&v1) && mk_build(&v2));
    struct pk_env e;
    mk_env(&e);
    CHECK_ST(run_op(&e, PKG_OP_INSTALL, &v1), OK);
    CHECK(count_ops(S_UPGRADE, &known, &total));
    for (unsigned n = 3; n <= total; n += 4) {
        struct cutter c = { .at = n, .mode = 1 };
        CHECK(reset(S_UPGRADE));
        mk_env(&e);
        e.cut = cut_cb;
        e.cut_ctx = &c;
        (void)operate(S_UPGRADE, &e);
        CHECK(e.dead && c.seen == n);
        for (unsigned k = 1; k < MAX_OPS; k++) {
            struct pk_env s2;
            struct cutter c2 = { .at = k, .mode = 1 };
            char what[PK_PATH];
            mk_env(&s2);
            s2.cut = cut_cb;
            s2.cut_ctx = &c2;
            (void)pk_settle(&s2, what);
            if (!s2.dead)
                break;   /* the settle finished before change k */
        }
        CHECK(reboot(false));
        int w = where(S_UPGRADE);
        if (w < 0)
            FAIL("cut before change %u, its settle cut too: %s", n, mk_why);
    }
    mk_free(&v1);
    mk_free(&v2);
    return mk_down();
}
