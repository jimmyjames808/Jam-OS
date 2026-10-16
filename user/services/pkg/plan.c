/* What an install, an upgrade or a remove does, and in what order. Pure
 * functions over the index and the installed set: no file, no network, so
 * utest runs them on made-up ones.
 *
 * Install and upgrade walk the `runs` lines of the index depth first, so a
 * package comes after what it runs. A dependency already installed at the
 * version asked for is left alone; one installed at a lower version is
 * upgraded to the index's. A cycle is refused: nothing could be installed
 * "first". Remove goes the other way: a package comes after everything
 * installed that runs it. */
#include <stdarg.h>
#include "pkg.h"

static struct pkg_str str(const char *s)
{
    return (struct pkg_str){ s, (uint32_t)strlen(s) };
}

static status_t fail(struct pk_plan *p, status_t st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static status_t fail(struct pk_plan *p, status_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->why, sizeof(p->why), fmt, ap);
    va_end(ap);
    return st;
}

void pk_plan_free(struct pk_plan *p)
{
    free(p->v);
    free(p->have);
    memset(p, 0, sizeof(*p));
}

/* One more item at the end. */
static status_t push(struct pk_plan *p, enum pk_kind kind, const struct pkg_ipkg *ip,
                     const struct pk_inst *old)
{
    if (p->n == p->cap) {
        unsigned cap = p->cap ? 2 * p->cap : 8;
        struct pk_item *v = malloc(cap * sizeof(*v));
        if (!v)
            return fail(p, ERR_NO_MEMORY, "out of memory");
        if (p->n)
            memcpy(v, p->v, p->n * sizeof(*v));
        free(p->v);
        p->v = v;
        p->cap = cap;
    }
    p->v[p->n++] = (struct pk_item){ kind, ip, old };
    return OK;
}

bool pk_newer(const struct pkg_ipkg *ip, const struct pk_inst *in)
{
    int c = pkg_version_cmp(ip->version, str(in->version));
    if (c)
        return c > 0;
    if (ip->revision != in->revision)
        return ip->revision > in->revision;
    return in->has_jsha && memcmp(ip->sha256, in->jsha, SHA256_BYTES) != 0;
}

/* ---- install and upgrade ----------------------------------------------------------------- */

struct walk {
    const struct pkg_index *x;
    const struct pk_dbset  *db;
    struct pk_plan         *p;
    uint8_t                *state;     /* per index package: 0 new, 1 on the path, 2 done */
    bool                    upgrade;   /* the command is `upgrade`: only what is newer */
};

static status_t visit(struct walk *w, struct pkg_str name, struct pkg_str least, bool named);

static status_t visit_runs(struct walk *w, const struct pkg_ipkg *ip)
{
    struct pkg_dep runs[PKG_DEPS_MAX];
    unsigned n = pkg_index_runs(w->x, ip, runs);
    for (unsigned i = 0; i < n; i++) {
        status_t st = visit(w, runs[i].name, runs[i].least, false);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Is ip, which is installed as in, to be upgraded? A dependency is when the
 * installed version is below the one asked for. A package named by an
 * `install` is never: it is remembered as there already (the command says
 * so); one named by an `upgrade` is when the index's is newer. */
static bool upgrading(struct walk *w, const struct pkg_ipkg *ip, const struct pk_inst *in,
                      struct pkg_str least, bool named)
{
    if (!named)
        return least.n && pkg_version_cmp(str(in->version), least) < 0;
    if (w->upgrade)
        return pk_newer(ip, in);
    w->p->have[w->p->nhave++] = ip;
    return false;
}

static status_t visit(struct walk *w, struct pkg_str name, struct pkg_str least, bool named)
{
    struct pk_plan *p = w->p;
    const struct pkg_ipkg *ip = pkg_index_find(w->x, name);
    if (!ip)
        return fail(p, ERR_NOT_FOUND, "the repository has no package called %.*s", (int)name.n,
                    name.p);
    unsigned at = (unsigned)(ip - w->x->packages);
    if (w->state[at] == 2)
        return OK;
    if (w->state[at] == 1)
        return fail(p, ERR_INVALID_ARGS, "%.*s runs itself (a cycle in the repository's lists)",
                    (int)name.n, name.p);
    if (pk_name_reserved(name))
        return fail(p, ERR_INVALID_ARGS, "%.*s is a name pkg keeps for itself", (int)name.n,
                    name.p);
    const struct pk_inst *in = pk_db_find(w->db, name);
    if (in && in->broken)
        return fail(p, ERR_BAD_STATE, "the record of %s in db/ is damaged (%s)", in->name,
                    in->why);
    if (in && !upgrading(w, ip, in, least, named)) {
        w->state[at] = 2;
        return OK;
    }
    w->state[at] = 1;
    status_t st = visit_runs(w, ip);
    if (st == OK)
        st = push(p, in ? PK_UPGRADE : PK_INSTALL, ip, in);
    w->state[at] = 2;
    return st;
}

static status_t walk_names(const struct pkg_index *x, const struct pk_dbset *db,
                           const char *const *names, unsigned n, bool upgrade,
                           struct pk_plan *p)
{
    struct walk w = { x, db, p, calloc(x->npackages ? x->npackages : 1, 1), upgrade };
    if (!w.state)
        return fail(p, ERR_NO_MEMORY, "out of memory");
    p->have = calloc(n ? n : 1, sizeof(*p->have));
    status_t st = p->have ? OK : fail(p, ERR_NO_MEMORY, "out of memory");
    for (unsigned i = 0; st == OK && i < n; i++)
        st = visit(&w, str(names[i]), (struct pkg_str){ NULL, 0 }, true);
    free(w.state);
    return st;
}

status_t pk_plan_install(const struct pkg_index *x, const struct pk_dbset *db,
                         const char *const *names, unsigned n, struct pk_plan *p)
{
    memset(p, 0, sizeof(*p));
    return walk_names(x, db, names, n, false, p);
}

status_t pk_plan_upgrade(const struct pkg_index *x, const struct pk_dbset *db,
                         const char *const *names, unsigned n, struct pk_plan *p)
{
    memset(p, 0, sizeof(*p));
    if (n) {
        for (unsigned i = 0; i < n; i++) {
            if (!pk_db_find(db, str(names[i])))
                return fail(p, ERR_NOT_FOUND, "%s is not installed", names[i]);
            if (!pkg_index_find(x, str(names[i])))
                return fail(p, ERR_NOT_FOUND, "the repository has no package called %s", names[i]);
        }
        return walk_names(x, db, names, n, true, p);
    }
    /* Everything installed that the index has: the walk skips what isn't newer. */
    const char **all = calloc(db->n ? db->n : 1, sizeof(*all));
    unsigned k = 0;
    if (!all)
        return fail(p, ERR_NO_MEMORY, "out of memory");
    for (unsigned i = 0; i < db->n; i++)
        if (!db->v[i].broken && pkg_index_find(x, str(db->v[i].name)))
            all[k++] = db->v[i].name;
    status_t st = walk_names(x, db, all, k, true, p);
    free(all);
    return st;
}

/* ---- remove -------------------------------------------------------------------------------- */

/* Does installed package q run installed package r? */
static bool runs_it(const struct pk_inst *q, const struct pk_inst *r)
{
    for (uint32_t i = 0; i < q->nruns; i++)
        if (!strcmp(q->runs[i].name, r->name))
            return true;
    return false;
}

/* Mark what must go with the named ones: with cascade, everything that runs
 * a marked package; else a package that does is a refusal. */
static status_t mark_dependents(const struct pk_dbset *db, uint8_t *mark, bool cascade,
                                struct pk_plan *p)
{
    for (bool changed = true; changed;) {   /* each pass marks more or ends */
        changed = false;
        for (unsigned q = 0; q < db->n; q++) {
            for (unsigned r = 0; r < db->n && !mark[q]; r++) {
                if (!mark[r] || !runs_it(&db->v[q], &db->v[r]))
                    continue;
                if (!cascade)
                    return fail(p, ERR_BAD_STATE, "%s runs %s: remove it first, or use -r",
                                db->v[q].name, db->v[r].name);
                mark[q] = 1;
                changed = true;
            }
        }
    }
    return OK;
}

/* The marked packages, each after everything marked that runs it. */
static status_t order_removals(const struct pk_dbset *db, uint8_t *mark, struct pk_plan *p)
{
    unsigned left = 0;
    for (unsigned i = 0; i < db->n; i++)
        left += mark[i] ? 1u : 0u;
    while (left) {   /* each pass emits one, or all that are left (a cycle) */
        bool any = false;
        for (unsigned r = 0; r < db->n && !any; r++) {
            bool free_to_go = mark[r] == 1;
            for (unsigned q = 0; free_to_go && q < db->n; q++)
                free_to_go = !(mark[q] == 1 && q != r && runs_it(&db->v[q], &db->v[r]));
            if (!free_to_go)
                continue;
            mark[r] = 2;
            left--;
            any = true;
            status_t st = push(p, PK_REMOVE, NULL, &db->v[r]);
            if (st != OK)
                return st;
        }
        for (unsigned r = 0; !any && r < db->n; r++) {
            if (mark[r] != 1)
                continue;
            mark[r] = 2;
            left--;
            status_t st = push(p, PK_REMOVE, NULL, &db->v[r]);
            if (st != OK)
                return st;
        }
    }
    return OK;
}

status_t pk_plan_remove(const struct pk_dbset *db, const char *const *names, unsigned n,
                        bool cascade, struct pk_plan *p)
{
    memset(p, 0, sizeof(*p));
    uint8_t *mark = calloc(db->n ? db->n : 1, 1);
    if (!mark)
        return fail(p, ERR_NO_MEMORY, "out of memory");
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < n; i++) {
        const struct pk_inst *in = pk_db_find(db, str(names[i]));
        if (!in)
            st = fail(p, ERR_NOT_FOUND, "%s is not installed", names[i]);
        else if (in->broken)
            st = fail(p, ERR_BAD_STATE, "the record of %s in db/ is damaged (%s)", in->name,
                      in->why);
        else
            mark[in - db->v] = 1;
    }
    if (st == OK)
        st = mark_dependents(db, mark, cascade, p);
    if (st == OK)
        st = order_removals(db, mark, p);
    free(mark);
    return st;
}
