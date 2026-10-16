/* `pkg install`, `pkg upgrade` and `pkg remove` (docs/M13.7-PLAN.md
 * sections 8 and 9): from the request to a plan (plan.c), the head of each
 * package fetched so the owner can be shown what they would get, the
 * conflicts looked for, one question, and then each package through the
 * journal (txn.c), dependencies first.
 *
 * What the owner is shown is what is installed: the head of a package (its
 * signed manifest) is checked against the ports key when it is fetched, and
 * the install later refuses a package whose manifest is not that very one
 * (the unpacker is given its SHA-256). A package from the repository is also
 * held to the size and SHA-256 the signed index names, whole, before any of
 * it is approved. One question covers the whole plan, dependencies and all;
 * init asks nothing more, but checks everything again itself. */
#include <wants.h>
#include "pkg.h"

#define PLAN_MAX 256u

/* A file on a mount as a source of bytes. */
struct file_src {
    char path[PK_PATH];
};

static status_t file_run(void *ctx, pk_feed_fn feed, void *fctx)
{
    struct file_src *f = ctx;
    struct jfile h;
    status_t st = file_open(f->path, FS_READ, &h);
    uint8_t *buf = st == OK ? malloc(64 * 1024) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    for (uint64_t at = 0; st == OK;) {   /* each turn feeds a piece */
        size_t got = 0;
        st = file_read(&h, at, buf, 64 * 1024, &got);
        if (st == OK && !got)
            break;
        at += got;
        if (st == OK)
            st = feed(fctx, buf, got);
    }
    if (buf)
        file_close(&h);
    free(buf);
    return st == PK_FEED_END ? OK : st;
}

/* One package of a plan. */
struct step {
    enum pk_kind           kind;
    char                   name[PK_NAME];
    const struct pkg_ipkg *ip;         /* from the repository (NULL: from a file) */
    struct pkg_manifest   *m;          /* its head */
    uint8_t               *text;
    uint32_t               len;
    struct pk_remote       rm;         /* the repository's side */
    struct file_src        fs;         /* the file's side */
    struct pk_source       src;
    char                   label[PK_NAME * 2 + 16];   /* "vim 9.1.0-r1" */
};

static void steps_free(struct step *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        free(s[i].m);
        free(s[i].text);
    }
    free(s);
}

/* ---- heads ------------------------------------------------------------------------------ */

/* The head of the step's package: its manifest, checked against the ports key,
 * and (from the repository) against the line of the index that named it. */
static int fetch_head(struct pk_app *a, struct step *s)
{
    char why[160];
    s->rm.quiet = true;
    status_t st = pk_head(&a->env, &s->src, &s->m, &s->text, &s->len, why);
    s->rm.quiet = false;
    if (st != OK) {
        pk_say("pkg: %s: %s\n", s->name, why[0] ? why : status_str(st));
        return st == ERR_CANCELED ? PK_EXIT_STOPPED : 1;
    }
    snprintf(s->label, sizeof(s->label), "%.*s %.*s-r%u", (int)s->m->name.n, s->m->name.p,
             (int)s->m->version.n, s->m->version.p, s->m->revision);
    if (s->ip && (pkg_str_cmp(s->ip->name, s->m->name) || pkg_str_cmp(s->ip->version, s->m->version) ||
                  s->ip->revision != s->m->revision)) {
        pk_say("pkg: %s: the package is not the one the index names\n", s->name);
        return 1;
    }
    return 0;
}

/* The steps of a plan from the repository, heads fetched. */
static int steps_from_plan(struct pk_app *a, const struct pk_plan *p, struct step **out)
{
    struct step *s = calloc(p->n ? p->n : 1, sizeof(*s));
    if (!s)
        return 1;
    for (unsigned i = 0; i < p->n; i++) {
        const struct pk_item *it = &p->v[i];
        s[i].kind = it->kind;
        s[i].ip = it->ip;
        snprintf(s[i].name, sizeof(s[i].name), "%.*s", (int)it->ip->name.n, it->ip->name.p);
        snprintf(s[i].label, sizeof(s[i].label), "%s", s[i].name);
        status_t st = pk_remote_make(&s[i].rm, &a->repo, it->ip, s[i].label);
        pk_remote_source(&s[i].rm, &s[i].src);
        int rc = st == OK ? fetch_head(a, &s[i]) : 1;
        if (rc) {
            steps_free(s, p->n);
            return rc;
        }
    }
    *out = s;
    return 0;
}

/* ---- conflicts and warnings -------------------------------------------------------------- */

static bool boot_has(const char *dir, struct pkg_str path)
{
    char p[PK_PATH];
    const char *base = path.p;
    for (uint32_t i = 0; i < path.n; i++)
        if (path.p[i] == '/')
            base = path.p + i + 1;
    snprintf(p, sizeof(p), "%s/%.*s", dir, (int)(path.p + path.n - base), base);
    return fs_stat(p, NULL, NULL, NULL) == OK;
}

/* A program with the name of one of the boot image's, which is found first on
 * PATH. */
static void warn_shadows(const struct pkg_manifest *m)
{
    for (uint32_t i = 0; i < m->nprograms; i++) {
        struct pkg_str p = m->programs[i].path;
        if (p.n < 5 || memcmp(p.p, "bin/", 4))
            continue;
        if (boot_has("/boot/usr/bin", p) || boot_has("/boot/bin", p))
            pk_say("    note: %.*s has the name of a program of the boot image, which is found "
                   "first on PATH\n", (int)p.n, p.p);
    }
}

/* Files two packages of the plan, or a package and an installed one, both
 * have: refused (said). */
static int check_conflicts(struct pk_app *a, struct step *s, unsigned n,
                           const struct pk_dbset *db)
{
    char owner[PK_NAME], path[PK_PATH];
    for (unsigned i = 0; i < n; i++) {
        status_t st = pk_db_conflict(&a->env, db, s[i].m, owner, path);
        if (st == ERR_ALREADY_EXISTS) {
            pk_say("pkg: %s has a file that %s already has: %s\n", s[i].name, owner, path);
            return 1;
        }
        struct pk_pathset set;
        if (st != OK || pk_pathset_make(s[i].m, &set) != OK)
            return 1;
        for (unsigned k = i + 1; k < n; k++) {
            const struct pkg_file *hit = pk_pathset_hit(&set, s[k].m);
            if (hit) {
                pk_say("pkg: %s and %s both have the file %.*s\n", s[i].name, s[k].name,
                       (int)hit->path.n, hit->path.p);
                pk_pathset_free(&set);
                return 1;
            }
        }
        pk_pathset_free(&set);
    }
    return 0;
}

/* ---- the question ------------------------------------------------------------------------- */

static void show_step(const struct step *s)
{
    char sz[24];
    const struct pkg_manifest *m = s->m;
    pk_say("  %s %s  (licence %.*s)\n", s->kind == PK_UPGRADE ? "upgrade" : "install", s->label,
           (int)m->licence.n, m->licence.p);
    pk_say("    %.*s\n", (int)m->summary.n, m->summary.p);
    pk_say("    %lu files, %s\n", (unsigned long)m->nfiles, pk_size_text(m->total, sz));
    for (uint32_t i = 0; i < m->nprograms; i++) {
        pk_say("    program %.*s asks for:\n", (int)m->programs[i].path.n, m->programs[i].path.p);
        pk_show_list(m->programs[i].list, "      ");
    }
    warn_shadows(m);
}

/* The plan shown, and the question asked: true for yes. */
static bool confirm(struct pk_app *a, const struct step *s, unsigned n)
{
    unsigned installs = 0, upgrades = 0, programs = 0;
    uint64_t bytes = 0;
    char sz[24], q[160];
    for (unsigned i = 0; i < n; i++) {
        installs += s[i].kind == PK_INSTALL ? 1u : 0u;
        upgrades += s[i].kind == PK_UPGRADE ? 1u : 0u;
        programs += s[i].m->nprograms;
        bytes += s[i].ip ? s[i].ip->size : 0;
        show_step(&s[i]);
    }
    if (bytes)
        pk_say("To download: %s\n", pk_size_text(bytes, sz));
    snprintf(q, sizeof(q), "%s%s%s %u package%s, %u program%s?",
             installs ? "Install" : "", installs && upgrades ? " and " : "",
             upgrades ? (installs ? "upgrade" : "Upgrade") : "", n, n == 1 ? "" : "s", programs,
             programs == 1 ? "" : "s");
    return pk_ask(a, q);
}

/* ---- running the steps -------------------------------------------------------------------- */

/* One step through the journal; its outcome said. 0, or the exit code. */
static int run_step(struct pk_app *a, struct step *s)
{
    char why[PK_WHY], sz[24];
    struct pk_job job = { .op = s->kind == PK_UPGRADE ? PKG_OP_UPGRADE : PKG_OP_INSTALL,
                          .name = s->name, .text = s->text, .len = s->len, .src = &s->src,
                          .size = s->ip ? s->ip->size : 0, .sha = s->ip ? s->ip->sha256 : NULL };
    if (s->ip)
        pk_say("Downloading %s (%s)\n", s->label, pk_size_text(s->ip->size, sz));
    status_t st = pk_run(&a->env, &job, why);
    if (st == OK) {
        pk_say("%s %s\n", s->kind == PK_UPGRADE ? "Upgraded" : "Installed", s->label);
        return 0;
    }
    if (st == ERR_BUSY) {
        pk_say("pkg: %s is running: the new version takes its place when it ends; run `pkg "
               "upgrade` again\n", s->name);
        return PK_EXIT_BUSY;
    }
    if (st == ERR_CANCELED) {
        pk_say("pkg: stopped; nothing of %s was changed\n", s->label);
        return PK_EXIT_STOPPED;
    }
    pk_say("pkg: %s was not %s: %s\n", s->label,
           s->kind == PK_UPGRADE ? "upgraded" : "installed", why[0] ? why : status_str(st));
    return 1;
}

/* Confirm, then run every step in order; stops at the first that fails. */
static int run_steps(struct pk_app *a, struct step *s, unsigned n)
{
    if (!a->ctl) {
        pk_say("pkg: no channel to init: start pkg from the shell (`pkg install ...`)\n");
        return 1;
    }
    if (!confirm(a, s, n)) {
        pk_say("Nothing was changed.\n");
        return 1;
    }
    for (unsigned i = 0; i < n; i++) {
        int rc = run_step(a, &s[i]);
        if (rc)
            return rc;
    }
    return 0;
}

/* ---- install and upgrade from the repository ---------------------------------------------- */

/* The plan for names, its steps (heads fetched) and their conflicts checked.
 * 0 with p->n == 0: nothing to do (said). */
static int prepare(struct pk_app *a, const struct pkg_index *x, const struct pk_dbset *db,
                   bool upgrade, const char *const *names, unsigned n, struct pk_plan *p,
                   struct step **s)
{
    status_t st = upgrade ? pk_plan_upgrade(x, db, names, n, p)
                          : pk_plan_install(x, db, names, n, p);
    if (st != OK) {
        pk_say("pkg: %s\n", p->why);
        return 1;
    }
    if (!p->n) {
        pk_say(upgrade ? "Everything installed is up to date.\n" :
               "Already installed: nothing to do (`pkg upgrade` looks for newer versions).\n");
        return 0;
    }
    if (p->n > PLAN_MAX) {
        pk_say("pkg: that is more than %u packages at once\n", PLAN_MAX);
        return 1;
    }
    int rc = steps_from_plan(a, p, s);
    return rc ? rc : check_conflicts(a, *s, p->n, db);
}

static int plan_run(struct pk_app *a, bool upgrade, const char *const *names, unsigned n)
{
    struct pkg_index *x = NULL;
    uint8_t *itext = NULL;
    struct pk_dbset db;
    struct pk_plan p;
    struct step *s = NULL;
    int rc = pk_load_index(a, false, &x, &itext);
    if (rc)
        return rc;
    if (pk_db_scan(&a->env, &db) != OK) {
        free(x);
        free(itext);
        return 1;
    }
    rc = prepare(a, x, &db, upgrade, names, n, &p, &s);
    if (rc == 0 && s)
        rc = run_steps(a, s, p.n);
    if (s)
        steps_free(s, p.n);
    pk_plan_free(&p);
    pk_db_free(&db);
    free(x);
    free(itext);
    return rc;
}

/* ---- install from files --------------------------------------------------------------------- */

static bool is_file_arg(const char *s)
{
    size_t n = strlen(s);
    return strchr(s, '/') || (n > 4 && !strcmp(s + n - 4, ".jpk"));
}

static struct pkg_str vstr(const char *v)
{
    return (struct pkg_str){ v, (uint32_t)strlen(v) };
}

/* One file's package against the installed set: its kind, and what it needs. */
static int judge_file(struct step *s, const struct pk_dbset *db)
{
    const struct pk_inst *in = pk_db_find(db, s->m->name);
    s->kind = in ? PK_UPGRADE : PK_INSTALL;
    snprintf(s->name, sizeof(s->name), "%.*s", (int)s->m->name.n, s->m->name.p);
    if (in && pkg_version_cmp(s->m->version, vstr(in->version)) < 0) {
        pk_say("pkg: %s is older than the %s installed: remove that first\n", s->label,
               in->version);
        return 1;
    }
    for (uint32_t k = 0; k < s->m->nruns; k++) {
        const struct pk_inst *dep = pk_db_find(db, s->m->runs[k].name);
        if (!dep || (s->m->runs[k].least.n &&
                     pkg_version_cmp(vstr(dep->version), s->m->runs[k].least) < 0)) {
            pk_say("pkg: %s needs %.*s installed first\n", s->label, (int)s->m->runs[k].name.n,
                   s->m->runs[k].name.p);
            return 1;
        }
    }
    return 0;
}

/* Files: each installed (or upgraded) from a mount. */
static int install_files(struct pk_app *a, const char *const *files, unsigned n)
{
    struct pk_dbset db;
    struct step *s = calloc(n, sizeof(*s));
    if (!s)
        return 1;
    if (pk_db_scan(&a->env, &db) != OK) {
        free(s);
        return 1;
    }
    int rc = 0;
    for (unsigned i = 0; i < n && !rc; i++) {
        snprintf(s[i].fs.path, sizeof(s[i].fs.path), "%s", files[i]);
        s[i].src = (struct pk_source){ &s[i].fs, file_run };
        snprintf(s[i].name, sizeof(s[i].name), "%s", files[i]);
        rc = fetch_head(a, &s[i]);
        if (!rc)
            rc = judge_file(&s[i], &db);
    }
    if (!rc)
        rc = check_conflicts(a, s, n, &db);
    if (!rc)
        rc = run_steps(a, s, n);
    pk_db_free(&db);
    steps_free(s, n);
    return rc;
}

int pk_cmd_install(struct pk_app *a, int argc, char **argv)
{
    if (argc < 2) {
        pk_say("usage: pkg install <name>... | <file.jpk>...\n");
        return 2;
    }
    bool files = is_file_arg(argv[1]);
    for (int i = 2; i < argc; i++)
        if (is_file_arg(argv[i]) != files) {
            pk_say("pkg: install names or files, not both in one command\n");
            return 2;
        }
    int rc = pk_settle_first(a);
    if (rc)
        return rc;
    return files ? install_files(a, (const char *const *)argv + 1, (unsigned)argc - 1)
                 : plan_run(a, false, (const char *const *)argv + 1, (unsigned)argc - 1);
}

int pk_cmd_upgrade(struct pk_app *a, int argc, char **argv)
{
    int rc = pk_settle_first(a);
    return rc ? rc : plan_run(a, true, (const char *const *)argv + 1, (unsigned)argc - 1);
}

/* ---- remove -------------------------------------------------------------------------------- */

static int run_removals(struct pk_app *a, const struct pk_plan *p)
{
    char q[160], why[PK_WHY];
    pk_say("Remove:\n");
    for (unsigned i = 0; i < p->n; i++)
        pk_say("  %s %s-r%u\n", p->v[i].old->name, p->v[i].old->version, p->v[i].old->revision);
    snprintf(q, sizeof(q), "Remove %u package%s?", p->n, p->n == 1 ? "" : "s");
    if (!a->ctl) {
        pk_say("pkg: no channel to init: start pkg from the shell (`pkg remove ...`)\n");
        return 1;
    }
    if (!pk_ask(a, q)) {
        pk_say("Nothing was changed.\n");
        return 1;
    }
    for (unsigned i = 0; i < p->n; i++) {
        struct pk_job job = { .op = PKG_OP_REMOVE, .name = p->v[i].old->name };
        status_t st = pk_run(&a->env, &job, why);
        if (st == ERR_BUSY) {
            pk_say("pkg: %s is running: its files go when it ends; run `pkg remove %s` again\n",
                   job.name, job.name);
            return PK_EXIT_BUSY;
        }
        if (st != OK) {
            pk_say("pkg: %s was not removed: %s\n", job.name, status_str(st));
            return 1;
        }
        pk_say("Removed %s\n", job.name);
    }
    return 0;
}

int pk_cmd_remove(struct pk_app *a, int argc, char **argv)
{
    bool cascade = argc > 1 && !strcmp(argv[1], "-r");
    int first = cascade ? 2 : 1;
    if (argc <= first) {
        pk_say("usage: pkg remove [-r] <name>...\n");
        return 2;
    }
    int rc = pk_settle_first(a);
    if (rc)
        return rc;
    struct pk_dbset db;
    struct pk_plan p;
    if (pk_db_scan(&a->env, &db) != OK)
        return 1;
    if (pk_plan_remove(&db, (const char *const *)argv + first, (unsigned)(argc - first), cascade,
                       &p) != OK) {
        pk_say("pkg: %s\n", p.why);
        rc = 1;
    } else {
        rc = run_removals(a, &p);
    }
    pk_plan_free(&p);
    pk_db_free(&db);
    return rc;
}
