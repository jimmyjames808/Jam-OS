/* The commands of `pkg` that look more than they change (docs/M13.7-PLAN.md
 * section 8): list, info, files, check, audit, update; and what every
 * command shares: the settle of a transaction cut short, the index, the
 * question, and the words a program's list is shown in.
 *
 * `list`, `info` and `files` read what is installed from db/ without checking
 * its signature (the records are pkg's own, and only pkg, init and the shell
 * can write them); `check` verifies each record's signature and every file
 * against it. */
#include <wants.h>
#include "pkg.h"

/* ---- shared ----------------------------------------------------------------------------- */

const char *pk_size_text(uint64_t n, char out[24])
{
    if (n < 10 * 1024)
        snprintf(out, 24, "%lu bytes", (unsigned long)n);
    else if (n < 1ull << 20)
        snprintf(out, 24, "%lu.%lu KB", (unsigned long)(n / 1000), (unsigned long)(n % 1000 / 100));
    else
        snprintf(out, 24, "%lu.%lu MB", (unsigned long)(n / 1000000),
                 (unsigned long)(n % 1000000 / 100000));
    return out;
}

bool pk_need_repo(struct pk_app *a)
{
    if (a->have_repo)
        return true;
    pk_say("pkg: no repository: %s\n  Set pkg.repo (or net.host) in /data/etc/settings, e.g. "
           "pkg.repo = http://10.2.21.174:8080/\n", a->repo_why[0] ? a->repo_why : "none given");
    return false;
}

int pk_settle_first(struct pk_app *a)
{
    char what[PK_PATH];
    status_t st = pk_settle(&a->env, what);
    if (what[0])
        pk_say("pkg: %s\n", what);
    if (st == ERR_BUSY) {
        pk_say("pkg: a program in use keeps that waiting: it takes its place when the program "
               "ends; run pkg again then\n");
        return PK_EXIT_BUSY;
    }
    return st == OK ? 0 : 1;
}

int pk_load_index(struct pk_app *a, bool force, struct pkg_index **x, uint8_t **text)
{
    char why[160];
    if (!pk_need_repo(a))
        return 1;
    *x = malloc(sizeof(**x));
    if (!*x)
        return 1;
    status_t st = pk_repo_index(&a->repo, &a->env, force, *x, text, why);
    if (st != OK) {
        pk_say("pkg: %s\n", why);
        free(*x);
        return st == ERR_CANCELED ? PK_EXIT_STOPPED : 1;
    }
    return 0;
}

void pk_show_list(struct pkg_str field, const char *indent)
{
    uint8_t list[PKG_LIST_MAX];
    size_t n;
    struct wants w;
    if (pkg_list_decode(field, list, &n) == OK && wants_parse((const char *)list, n, &w) == OK)
        pk_say("%s%s\n", indent, w.text[0] ? w.text : "nothing but its terminal");
    else
        pk_say("%s(a list this pkg does not read)\n", indent);
}

bool pk_ask(struct pk_app *a, const char *question)
{
    if (!a->ask) {
        pk_say("pkg: nothing to ask the question on: start pkg from the shell\n");
        return false;
    }
    if (jam_channel_write(a->ask, question, (uint32_t)strlen(question), NULL, 0) != OK)
        return false;
    uint64_t end = now() + 600 * NS_PER_S;   /* the owner has ten minutes */
    while (now() < end && !pk_net_stopped()) {   /* each turn waits a tenth of a second */
        signals_t seen;
        status_t st = jam_object_wait_one(a->ask, SIG_READABLE | SIG_PEER_CLOSED,
                                          now() + 100 * NS_PER_MS, &seen);
        if (st != OK && st != ERR_TIMED_OUT)
            return false;
        if (!(seen & SIG_READABLE))
            continue;
        uint8_t ans[8];
        uint32_t got = 0, nh = 0;
        struct channel_read_args r = {
            .h = a->ask, .rd_cap = sizeof(ans), .rd_bytes = (uint64_t)(uintptr_t)ans,
            .rd_actual = (uint64_t)(uintptr_t)&got, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        if (jam_channel_read(&r) != OK)
            return false;
        return got == 1 && ans[0] == 'y';
    }
    return false;
}

/* ---- list ------------------------------------------------------------------------------ */

int pk_cmd_list(struct pk_app *a, int argc, char **argv)
{
    bool upgrades = argc == 2 && !strcmp(argv[1], "-u");
    if (argc > 2 || (argc == 2 && !upgrades)) {
        pk_say("usage: pkg list [-u]\n");
        return 2;
    }
    struct pk_dbset db;
    struct pkg_index *x = NULL;
    uint8_t *itext = NULL;
    int rc = 0;
    if (pk_db_scan(&a->env, &db) != OK)
        return 1;
    if (upgrades && (rc = pk_load_index(a, false, &x, &itext)) != 0) {
        pk_db_free(&db);
        return rc;
    }
    unsigned shown = 0;
    for (unsigned i = 0; i < db.n; i++) {
        const struct pk_inst *in = &db.v[i];
        const struct pkg_ipkg *ip = x ? pkg_index_find(x, (struct pkg_str){ in->name,
                                        (uint32_t)strlen(in->name) }) : NULL;
        bool newer = ip && !in->broken && pk_newer(ip, in);
        if (upgrades && !newer)
            continue;
        shown++;
        if (in->broken)
            pk_say("%s (damaged: %s)\n", in->name, in->why);
        else if (newer)
            pk_say("%s %s-r%u -> %.*s-r%u\n", in->name, in->version, in->revision,
                   (int)ip->version.n, ip->version.p, ip->revision);
        else
            pk_say("%s %s-r%u\n", in->name, in->version, in->revision);
    }
    if (!shown)
        pk_say(upgrades ? "Everything installed is up to date.\n" : "No package is installed.\n");
    pk_db_free(&db);
    free(x);
    free(itext);
    return 0;
}

/* ---- info and files --------------------------------------------------------------------- */

static void show_info(const struct pkg_manifest *m, const char *state)
{
    char sz[24];
    pk_say("%.*s %.*s-r%u (%s)\n", (int)m->name.n, m->name.p, (int)m->version.n, m->version.p,
           m->revision, state);
    pk_say("  %.*s\n  licence: %.*s\n", (int)m->summary.n, m->summary.p, (int)m->licence.n,
           m->licence.p);
    for (uint32_t i = 0; i < m->nruns; i++)
        pk_say("  runs: %.*s%s%.*s\n", (int)m->runs[i].name.n, m->runs[i].name.p,
               m->runs[i].least.n ? " " : "", (int)m->runs[i].least.n, m->runs[i].least.p);
    pk_say("  %lu files, %s\n", (unsigned long)m->nfiles, pk_size_text(m->total, sz));
    for (uint32_t i = 0; i < m->nprograms; i++) {
        pk_say("  program %.*s asks for:\n", (int)m->programs[i].path.n, m->programs[i].path.p);
        pk_show_list(m->programs[i].list, "    ");
    }
}

/* The manifest of an installed package. */
static int load_installed(struct pk_app *a, const char *name, struct pkg_manifest **m,
                          uint8_t **text)
{
    uint64_t len;
    *m = calloc(1, sizeof(**m));
    if (!*m)
        return 1;
    status_t st = pk_db_load(&a->env, name, "", false, text, &len, *m);
    if (st != OK)
        free(*m);
    return st == OK ? 0 : st == ERR_NOT_FOUND ? 2 : 1;
}

/* A package of the repository: its head. */
static int load_remote(struct pk_app *a, const char *name, struct pkg_manifest **m,
                       uint8_t **text)
{
    struct pkg_index *x = NULL;
    uint8_t *itext = NULL;
    int rc = pk_load_index(a, false, &x, &itext);
    if (rc)
        return rc;
    const struct pkg_ipkg *ip = pkg_index_find(x, (struct pkg_str){ name, (uint32_t)strlen(name) });
    if (!ip) {
        pk_say("pkg: no package called %s (installed or in the repository)\n", name);
        rc = 1;
    } else {
        struct pk_remote rm;
        struct pk_source src;
        char why[160];
        uint32_t len;
        rc = pk_remote_make(&rm, &a->repo, ip, name) != OK ? 1 : 0;
        rm.quiet = true;
        pk_remote_source(&rm, &src);
        if (!rc && pk_head(&a->env, &src, m, text, &len, why) != OK) {
            pk_say("pkg: %s: %s\n", name, why);
            rc = 1;
        }
    }
    free(x);
    free(itext);
    return rc;
}

int pk_cmd_info(struct pk_app *a, int argc, char **argv)
{
    struct pkg_manifest *m = NULL;
    uint8_t *text = NULL;
    if (argc != 2) {
        pk_say("usage: pkg info <name>\n");
        return 2;
    }
    int rc = load_installed(a, argv[1], &m, &text);
    if (rc == 0) {
        show_info(m, "installed");
    } else if (rc == 2 && (rc = load_remote(a, argv[1], &m, &text)) == 0) {
        show_info(m, "in the repository, not installed");
    }
    if (rc == 0) {
        free(m);
        free(text);
    }
    return rc == 0 ? 0 : 1;
}

int pk_cmd_files(struct pk_app *a, int argc, char **argv)
{
    struct pkg_manifest *m;
    uint8_t *text;
    if (argc != 2) {
        pk_say("usage: pkg files <name>\n");
        return 2;
    }
    int rc = load_installed(a, argv[1], &m, &text);
    if (rc == 2)
        pk_say("pkg: %s is not installed\n", argv[1]);
    if (rc)
        return 1;
    for (uint32_t i = 0; i < m->nfiles; i++)
        pk_say("%s/%.*s%s\n", a->env.prefix, (int)m->files[i].path.n, m->files[i].path.p,
               m->files[i].program != PKG_NO_PROGRAM ? " (program)" : "");
    free(m);
    free(text);
    return 0;
}

/* ---- check ------------------------------------------------------------------------------- */

/* Is the file at path as the manifest's line says (size and hash)? NULL:
 * yes; else why not. */
static const char *file_problem(const char *path, const struct pkg_file *f)
{
    uint64_t size;
    uint8_t sha[SHA256_BYTES];
    status_t st = pk_hash_file(path, &size, sha);
    if (st == ERR_NOT_FOUND)
        return "missing";
    if (st != OK)
        return "can't be read";
    return size != f->size || memcmp(sha, f->sha256, SHA256_BYTES) ? "changed" : NULL;
}

/* One installed package against its manifest. The number of problems. */
static unsigned check_one(struct pk_app *a, const struct pk_inst *in, const struct pkg_manifest *alt)
{
    uint8_t *text;
    uint64_t len;
    unsigned bad = 0;
    struct pkg_manifest *m = calloc(1, sizeof(*m));
    status_t st = m ? pk_db_load(&a->env, in->name, "", a->env.have_key, &text, &len, m)
                    : ERR_NO_MEMORY;
    if (st != OK) {
        pk_say("%s: the record in db/ is %s\n", in->name, st == ERR_NOT_FOUND ? "missing" :
               st == ERR_ACCESS_DENIED ? "not signed by this build's ports key" : "damaged");
        free(m);
        return 1;
    }
    for (uint32_t i = 0; i < m->nfiles; i++) {
        char path[PK_PATH];
        pk_path(&a->env, m->files[i].path, "", path);
        const char *why = file_problem(path, &m->files[i]);
        if (why && alt) {   /* an upgrade waiting: the new version's file is fine too */
            for (uint32_t k = 0; k < alt->nfiles; k++)
                if (!pkg_str_cmp(alt->files[k].path, m->files[i].path) &&
                    !file_problem(path, &alt->files[k]))
                    why = NULL;
        }
        if (why) {
            pk_say("%s: %.*s %s\n", in->name, (int)m->files[i].path.n, m->files[i].path.p, why);
            bad++;
        }
    }
    free(m);
    free(text);
    return bad;
}

/* The manifest of an upgrade that waits in the journal, if it is for name. */
static struct pkg_manifest *waiting_upgrade(struct pk_app *a, const char *name, uint8_t **text)
{
    struct pkg_journal *j = calloc(1, sizeof(*j));
    uint8_t *jt = NULL, *t;
    const char *why;
    uint64_t len;
    struct pkg_manifest *m = NULL;
    if (j && pk_journal_get(&a->env, j, &jt, &why) == OK && j->op == PKG_OP_UPGRADE &&
        pkg_str_eq(j->name, name)) {
        m = calloc(1, sizeof(*m));
        if (m && pk_db_load(&a->env, name, ".new", false, &t, &len, m) == OK) {
            *text = t;
        } else {
            free(m);
            m = NULL;
        }
    }
    free(jt);
    free(j);
    return m;
}

int pk_cmd_check(struct pk_app *a, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        pk_say("usage: pkg check\n");
        return 2;
    }
    int rc = a->ctl ? pk_settle_first(a) : 0;
    if (rc)
        return rc;
    struct pk_dbset db;
    unsigned bad = 0;
    if (pk_db_scan(&a->env, &db) != OK)
        return 1;
    if (!a->env.have_key)
        pk_say("pkg: this build has no ports key: the records' signatures are not checked\n");
    for (unsigned i = 0; i < db.n; i++) {
        uint8_t *atext = NULL;
        struct pkg_manifest *alt;
        if (db.v[i].broken) {
            pk_say("%s: the record in db/ is damaged (%s)\n", db.v[i].name, db.v[i].why);
            bad++;
            continue;
        }
        alt = waiting_upgrade(a, db.v[i].name, &atext);
        bad += check_one(a, &db.v[i], alt);
        free(alt);
        free(atext);
    }
    if (!bad)
        pk_say("pkg check: %u package%s, every file is as its manifest says.\n", db.n,
               db.n == 1 ? "" : "s");
    pk_db_free(&db);
    return bad ? 1 : 0;
}

/* ---- audit ------------------------------------------------------------------------------- */

int pk_cmd_audit(struct pk_app *a, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        pk_say("usage: pkg audit\n");
        return 2;
    }
    if (!pk_need_repo(a))
        return 1;
    char why[160];
    struct pkg_vulns *v = malloc(sizeof(*v));
    uint8_t *vtext;
    if (!v || pk_repo_vulns(&a->repo, &a->env, v, &vtext, why) != OK) {
        pk_say("pkg: %s\n", why);
        free(v);
        return 1;
    }
    struct pk_dbset db;
    unsigned found = 0;
    struct pkg_manifest *m = calloc(1, sizeof(*m));
    if (!m || pk_db_scan(&a->env, &db) != OK) {
        free(m);
        free(v);
        free(vtext);
        return 1;
    }
    for (unsigned i = 0; i < db.n; i++) {
        uint8_t *text;
        uint64_t len;
        const struct pkg_vuln *hit[32];
        if (db.v[i].broken || pk_db_load(&a->env, db.v[i].name, "", false, &text, &len, m) != OK)
            continue;
        unsigned n = pkg_audit(v, m, hit, 32);
        for (unsigned k = 0; k < n && k < 32; k++) {
            pk_say("%s %s-r%u: %.*s (%.*s): %.*s\n", db.v[i].name, db.v[i].version,
                   db.v[i].revision, (int)hit[k]->id.n, hit[k]->id.p, (int)hit[k]->component.n,
                   hit[k]->component.p, (int)hit[k]->summary.n, hit[k]->summary.p);
            found++;
        }
        free(text);
    }
    if (!found)
        pk_say("No known advisory affects what is installed (advisories of serial %llu).\n",
               (unsigned long long)v->serial);
    pk_db_free(&db);
    free(m);
    free(v);
    free(vtext);
    return found ? 1 : 0;
}

/* ---- update ------------------------------------------------------------------------------ */

int pk_cmd_update(struct pk_app *a, int argc, char **argv)
{
    bool force = argc == 2 && !strcmp(argv[1], "-f");
    if (argc > 2 || (argc == 2 && !force)) {
        pk_say("usage: pkg update [-f]\n  -f: take an index with a lower serial than the last "
               "(a repository made again from the start)\n");
        return 2;
    }
    struct pkg_index *x = NULL;
    uint8_t *itext = NULL;
    int rc = pk_load_index(a, force, &x, &itext);
    if (rc)
        return rc;
    pk_say("The repository has %u packages (serial %llu, made %s).\n", x->npackages,
           (unsigned long long)x->serial, x->made);
    struct pk_dbset db;
    unsigned waiting = 0;
    if (pk_db_scan(&a->env, &db) == OK) {
        for (unsigned i = 0; i < db.n; i++) {
            const struct pkg_ipkg *ip = pkg_index_find(x, (struct pkg_str){ db.v[i].name,
                                                       (uint32_t)strlen(db.v[i].name) });
            if (ip && !db.v[i].broken && pk_newer(ip, &db.v[i])) {
                pk_say("  %s %s-r%u -> %.*s-r%u\n", db.v[i].name, db.v[i].version,
                       db.v[i].revision, (int)ip->version.n, ip->version.p, ip->revision);
                waiting++;
            }
        }
        pk_db_free(&db);
    }
    if (waiting)
        pk_say("`pkg upgrade` installs the %u newer.\n", waiting);
    else
        pk_say("Everything installed is up to date.\n");
    free(x);
    free(itext);
    return 0;
}
