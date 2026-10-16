/* utest: what bin/pkg plans (user/services/pkg/plan.c, db.c), on made-up
 * indexes and installed sets: no file, no network.
 *
 *   pkgplan_install   dependencies first; what is installed is left alone; a
 *                     dependency below the version asked for is upgraded; a
 *                     package named and installed is remembered; a cycle, an
 *                     unknown name and a reserved name are refused
 *   pkgplan_upgrade   what is newer, by version, revision or hash; new
 *                     dependencies of a new version; named and not installed
 *   pkgplan_remove    refused while something runs it, with -r that goes too,
 *                     dependents first
 *   pkgplan_paths     files two packages both have, by FAT's names */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <sha256.h>
#include "pkgmk.h"
#include "utest.h"

#define NPKG 8u

struct ipk {
    const char *name, *version;
    unsigned    revision;
    const char *runs;       /* "dep" or "dep 1.0", one per package at most here */
    uint8_t     seed;       /* its hash */
};

static char idx_text[8192];

/* An index of these packages (sorted by name) with fake hashes, signature
 * not checked. */
static bool make_index(struct pkg_index *x, const struct ipk *p, unsigned n)
{
    size_t at = (size_t)snprintf(idx_text, sizeof(idx_text),
                                 "jamos-ports 1\nserial 7\nmade 2026-10-11T00:00:00Z\n"
                                 "abi 0123456789abcdef\n");
    char hex[65];
    uint8_t d[32];
    for (unsigned i = 0; i < n; i++) {
        memset(d, p[i].seed, sizeof(d));
        sha256_hex(d, hex);
        at += (size_t)snprintf(idx_text + at, sizeof(idx_text) - at,
                               "package %s %s %u 20000 %s %s\n", p[i].name, p[i].version,
                               p[i].revision, hex, p[i].name);
        if (p[i].runs)
            at += (size_t)snprintf(idx_text + at, sizeof(idx_text) - at, "runs %s %s\n",
                                   p[i].name, p[i].runs);
    }
    for (unsigned i = 0; i < n; i++) {
        memset(d, 1, sizeof(d));
        sha256_hex(d, hex);
        at += (size_t)snprintf(idx_text + at, sizeof(idx_text) - at, "src %s %s %u 100 %s\n",
                               p[i].name, p[i].version, p[i].revision, hex);
    }
    at += (size_t)snprintf(idx_text + at, sizeof(idx_text) - at, "signature ");
    for (int i = 0; i < 128; i++)
        idx_text[at++] = '0';
    idx_text[at++] = '\n';
    const char *why = NULL;
    status_t st = pkg_index_parse_unchecked(idx_text, at, x, &why);
    if (st != OK)
        FAIL("the made-up index is refused: %s", why ? why : status_str(st));
    return true;
}

struct ipkg_inst {
    const char *name, *version;
    unsigned    revision;
    const char *runs;       /* names, space separated */
    uint8_t     jseed;      /* the hash of the package it came in (0: unknown) */
};


static void make_db(struct pk_dbset *db, const struct ipkg_inst *p, unsigned n)
{
    memset(db, 0, sizeof(*db));
    db->v = calloc(n ? n : 1, sizeof(*db->v));
    db->n = n;
    for (unsigned i = 0; i < n; i++) {
        struct pk_inst *in = &db->v[i];
        snprintf(in->name, sizeof(in->name), "%s", p[i].name);
        snprintf(in->version, sizeof(in->version), "%s", p[i].version);
        in->revision = p[i].revision;
        in->has_jsha = p[i].jseed != 0;
        memset(in->jsha, p[i].jseed, sizeof(in->jsha));
        in->runs = calloc(4, sizeof(*in->runs));
        char run[200];
        snprintf(run, sizeof(run), "%s", p[i].runs ? p[i].runs : "");
        for (char *w = run; *w;) {
            char *sp = strchr(w, ' ');
            if (sp)
                *sp = '\0';
            snprintf(in->runs[in->nruns++].name, PK_NAME, "%s", w);
            w = sp ? sp + 1 : w + strlen(w);
        }
    }
}

static bool plan_is(const struct pk_plan *p, const char *want)
{
    char got[200] = "";
    for (unsigned i = 0; i < p->n; i++) {
        char one[PK_NAME + 8];
        if (p->v[i].ip)
            snprintf(one, sizeof(one), "%c%.*s ", p->v[i].kind == PK_INSTALL ? '+' : '^',
                     (int)p->v[i].ip->name.n, p->v[i].ip->name.p);
        else
            snprintf(one, sizeof(one), "-%s ", p->v[i].old->name);
        snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s", one);
    }
    if (strcmp(got, want)) {
        printf("utest: %s: plan is \"%s\", want \"%s\" (%s)\n", utest_cur, got, want, p->why);
        return false;
    }
    return true;
}

static struct pkg_index x;   /* 800 KB: static, not on the stack */

bool t_pkgplan_install(void)
{
    struct pk_dbset db;
    struct pk_plan p;
    const struct ipk idx[] = {
        { "aaa", "1.0", 1, "bbb", 1 }, { "bbb", "2.0", 1, "ccc", 2 }, { "ccc", "1.5", 1, NULL, 3 },
        { "ddd", "1.0", 1, "ddd2", 4 }, { "ddd2", "1.0", 1, NULL, 5 },
        { "eee", "1.0", 1, "ccc 1.4", 6 },
    };
    CHECK(make_index(&x, idx, 6));
    /* Empty: aaa runs bbb runs ccc: ccc first. */
    make_db(&db, NULL, 0);
    const char *names1[] = { "aaa" };
    CHECK_ST(pk_plan_install(&x, &db, names1, 1, &p), OK);
    CHECK(plan_is(&p, "+ccc +bbb +aaa "));
    pk_plan_free(&p);
    /* ccc and bbb in the right order, once, with two names that share a dependency. */
    const char *names2[] = { "aaa", "eee", "bbb" };
    CHECK_ST(pk_plan_install(&x, &db, names2, 3, &p), OK);
    CHECK(plan_is(&p, "+ccc +bbb +aaa +eee "));
    pk_plan_free(&p);
    pk_db_free(&db);
    /* ccc 1.5 installed: only aaa and bbb. ccc 1.0 installed, bbb needs it at any
     * version: left alone. eee needs ccc 1.4: upgraded. */
    const struct ipkg_inst c15[] = { { "ccc", "1.5", 1, NULL, 9 } };
    make_db(&db, c15, 1);
    CHECK_ST(pk_plan_install(&x, &db, names1, 1, &p), OK);
    CHECK(plan_is(&p, "+bbb +aaa "));
    pk_plan_free(&p);
    pk_db_free(&db);
    const struct ipkg_inst c10[] = { { "ccc", "1.0", 1, NULL, 9 } };
    make_db(&db, c10, 1);
    CHECK_ST(pk_plan_install(&x, &db, names1, 1, &p), OK);
    CHECK(plan_is(&p, "+bbb +aaa "));
    pk_plan_free(&p);
    const char *names3[] = { "eee" };
    CHECK_ST(pk_plan_install(&x, &db, names3, 1, &p), OK);
    CHECK(plan_is(&p, "^ccc +eee "));
    pk_plan_free(&p);
    pk_db_free(&db);
    /* Named and installed: nothing to do, and said so. */
    const struct ipkg_inst a1[] = { { "aaa", "1.0", 1, "bbb", 9 }, { "bbb", "2.0", 1, "ccc", 9 },
                                    { "ccc", "1.5", 1, NULL, 9 } };
    make_db(&db, a1, 3);
    CHECK_ST(pk_plan_install(&x, &db, names1, 1, &p), OK);
    CHECK_EQ(p.n, 0);
    CHECK_EQ(p.nhave, 1);
    pk_plan_free(&p);
    pk_db_free(&db);
    /* Refusals: unknown, reserved. */
    make_db(&db, NULL, 0);
    const char *nope[] = { "zzz" };
    CHECK_ST(pk_plan_install(&x, &db, nope, 1, &p), ERR_NOT_FOUND);
    pk_plan_free(&p);
    pk_db_free(&db);
    /* A cycle in the repository's lists. */
    const struct ipk cyc[] = { { "xa", "1", 1, "xb", 1 }, { "xb", "1", 1, "xa", 2 } };
    CHECK(make_index(&x, cyc, 2));
    make_db(&db, NULL, 0);
    const char *xa[] = { "xa" };
    CHECK_ST(pk_plan_install(&x, &db, xa, 1, &p), ERR_INVALID_ARGS);
    pk_plan_free(&p);
    const struct ipk res[] = { { "serial", "1", 1, NULL, 1 } };
    CHECK(make_index(&x, res, 1));
    const char *rs[] = { "serial" };
    CHECK_ST(pk_plan_install(&x, &db, rs, 1, &p), ERR_INVALID_ARGS);
    pk_plan_free(&p);
    pk_db_free(&db);
    return true;
}

bool t_pkgplan_upgrade(void)
{
    struct pk_dbset db;
    struct pk_plan p;
    const struct ipk idx[] = {
        { "aaa", "1.1", 1, NULL, 1 }, { "bbb", "1.0", 2, NULL, 2 }, { "ccc", "1.0", 1, NULL, 3 },
        { "ddd", "2.0", 1, "new", 4 }, { "eee", "1.0", 1, NULL, 5 }, { "new", "1.0", 1, NULL, 6 },
    };
    CHECK(make_index(&x, idx, 6));
    /* aaa: newer version. bbb: newer revision. ccc: same, same hash. ddd: newer, and it
     * runs a package not installed. eee: same version and revision, another hash. */
    const struct ipkg_inst inst[] = {
        { "aaa", "1.0", 1, NULL, 1 }, { "bbb", "1.0", 1, NULL, 2 }, { "ccc", "1.0", 1, NULL, 3 },
        { "ddd", "1.0", 1, NULL, 4 }, { "eee", "1.0", 1, NULL, 77 },
    };
    make_db(&db, inst, 5);
    CHECK_ST(pk_plan_upgrade(&x, &db, NULL, 0, &p), OK);
    CHECK(plan_is(&p, "^aaa ^bbb +new ^ddd ^eee "));
    pk_plan_free(&p);
    /* Named: just those; one not installed, or not in the index, is an error. */
    const char *two[] = { "bbb", "ccc" };
    CHECK_ST(pk_plan_upgrade(&x, &db, two, 2, &p), OK);
    CHECK(plan_is(&p, "^bbb "));
    pk_plan_free(&p);
    const char *no1[] = { "new" };
    CHECK_ST(pk_plan_upgrade(&x, &db, no1, 1, &p), ERR_NOT_FOUND);
    pk_plan_free(&p);
    /* A hash that is unknown (installed from a file) is not "different". */
    pk_db_free(&db);
    const struct ipkg_inst unk[] = { { "eee", "1.0", 1, NULL, 0 } };
    make_db(&db, unk, 1);
    CHECK_ST(pk_plan_upgrade(&x, &db, NULL, 0, &p), OK);
    CHECK_EQ(p.n, 0);
    pk_plan_free(&p);
    pk_db_free(&db);
    return true;
}

bool t_pkgplan_remove(void)
{
    struct pk_dbset db;
    struct pk_plan p;
    /* aaa runs bbb runs ccc; ddd stands alone. */
    const struct ipkg_inst inst[] = {
        { "aaa", "1", 1, "bbb", 1 }, { "bbb", "1", 1, "ccc", 1 }, { "ccc", "1", 1, NULL, 1 },
        { "ddd", "1", 1, NULL, 1 },
    };
    make_db(&db, inst, 4);
    const char *c[] = { "ccc" }, *b[] = { "bbb" }, *a[] = { "aaa" }, *d[] = { "ddd" };
    CHECK_ST(pk_plan_remove(&db, c, 1, false, &p), ERR_BAD_STATE);
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, c, 1, true, &p), OK);
    CHECK(plan_is(&p, "-aaa -bbb -ccc "));
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, b, 1, false, &p), ERR_BAD_STATE);
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, b, 1, true, &p), OK);
    CHECK(plan_is(&p, "-aaa -bbb "));
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, a, 1, false, &p), OK);
    CHECK(plan_is(&p, "-aaa "));
    pk_plan_free(&p);
    /* All the chain named, in any order: still dependents first. */
    const char *all[] = { "ccc", "aaa", "bbb", "ddd" };
    CHECK_ST(pk_plan_remove(&db, all, 4, false, &p), OK);
    CHECK(plan_is(&p, "-aaa -bbb -ccc -ddd "));
    pk_plan_free(&p);
    CHECK_ST(pk_plan_remove(&db, d, 1, false, &p), OK);
    CHECK(plan_is(&p, "-ddd "));
    pk_plan_free(&p);
    const char *nope[] = { "zzz" };
    CHECK_ST(pk_plan_remove(&db, nope, 1, false, &p), ERR_NOT_FOUND);
    pk_plan_free(&p);
    pk_db_free(&db);
    return true;
}

/* Parse the manifest of a made package. */
static bool manifest_of(const struct mkpkg *p, struct pkg_manifest *m)
{
    CHECK_ST(pkg_manifest_parse_unchecked(p->jpk + PKG_BLOCK, p->mlen, m, NULL), OK);
    return true;
}

bool t_pkgplan_paths(void)
{
    static struct pkg_manifest ma, mb, mc;
    struct pk_pathset s;
    struct mkpkg a = { .name = "aaa", .version = "1", .revision = 1 };
    struct mkpkg b = { .name = "bbb", .version = "1", .revision = 1 };
    struct mkpkg c = { .name = "ccc", .version = "1", .revision = 1 };
    a.files[a.nfiles++] = (struct mk_file){ "share/x/Readme", 0644, "a\n", false };
    a.files[a.nfiles++] = (struct mk_file){ "share/x/only-a", 0644, "a\n", false };
    b.files[b.nfiles++] = (struct mk_file){ "share/x/only-b", 0644, "b\n", false };
    b.files[b.nfiles++] = (struct mk_file){ "share/x/README", 0644, "b\n", false };
    c.files[c.nfiles++] = (struct mk_file){ "share/y/only-c", 0644, "c\n", false };
    CHECK(mk_up() && mk_build(&a) && mk_build(&b) && mk_build(&c));
    CHECK(manifest_of(&a, &ma) && manifest_of(&b, &mb) && manifest_of(&c, &mc));
    CHECK_ST(pk_pathset_make(&ma, &s), OK);
    const struct pkg_file *hit = pk_pathset_hit(&s, &mb);   /* Readme and README are one file on FAT */
    CHECK(hit && pkg_str_eq(hit->path, "share/x/README"));
    CHECK(pk_pathset_hit(&s, &mc) == NULL);
    pk_pathset_free(&s);
    /* Against what is installed: a is installed; b conflicts, c doesn't, a itself doesn't. */
    struct pk_env e;
    char owner[PK_NAME], path[PK_PATH], why[PK_WHY];
    struct pk_dbset db;
    struct mk_src src;
    struct pk_source source;
    struct pk_job job;
    mk_env(&e);
    mk_job(&job, PKG_OP_INSTALL, &a, &src, &source);
    CHECK_ST(pk_run(&e, &job, why), OK);
    CHECK_ST(pk_db_scan(&e, &db), OK);
    CHECK_ST(pk_db_conflict(&e, &db, &mb, owner, path), ERR_ALREADY_EXISTS);
    CHECK(!strcmp(owner, "aaa") && !strcmp(path, "share/x/Readme"));   /* the owner's spelling */
    CHECK_ST(pk_db_conflict(&e, &db, &mc, owner, path), OK);
    CHECK_ST(pk_db_conflict(&e, &db, &ma, owner, path), OK);
    pk_db_free(&db);
    mk_free(&a);
    mk_free(&b);
    mk_free(&c);
    return mk_down();
}
