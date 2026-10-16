/* One package's install, upgrade or remove, through the journal
 * (docs/M13.7-PLAN.md section 9; <pkgfmt.h> has the journal and its steps),
 * and the settle of one that was cut short.
 *
 * The steps are init's step meanings (user/services/init/pkgapprove.c
 * settles the same journal at boot): a step names what is done.
 *
 *   begin     the journal is written; nothing else
 *   staged    db/<name>.new (the new manifest) and every file at `<path>.new`
 *             are written, synced and read back against their hashes
 *   approved  init has approved the programs, staged (the commit point)
 *   swapped   every `.new` is at its path; an upgrade's old version's files
 *             that the new one lacks are gone
 *   revoked   (upgrade) the old version's approval lines are taken back
 *   removed   (remove) the files are gone
 *   recorded  db/<name> is the new manifest (remove: gone)
 *
 * Before the commit point a cut is undone: the staged files go, any line
 * the new version got goes, and the new manifest. From it on the
 * transaction is finished. Each step can be done over, so a cut anywhere
 * leaves a journal that the next run (this code, or init at boot) carries
 * on from; the step in the journal may be one behind what was done.
 *
 * Files in use (ERR_BUSY: a program runs from them) can't be replaced or
 * removed: an upgrade leaves their `.new` and lists them in the journal as
 * `busy`, a removal stops after revoking, and both wait in the journal
 * until the program ends. pk_run says ERR_BUSY then, and the next run
 * tries again. */
#include <sha256.h>
#include "pkg.h"

struct loaded {                      /* a manifest of db/ */
    uint8_t             *text;
    uint64_t             len;
    struct pkg_manifest *m;
};

static void unload(struct loaded *l)
{
    free(l->text);
    free(l->m);
    memset(l, 0, sizeof(*l));
}

/* The manifest db/<name><suffix>, if its SHA-256 is want (NULL: any) and the
 * ports key signed it. ERR_NOT_FOUND: no such file; ERR_ACCESS_DENIED: not
 * the manifest wanted, or not signed. */
static status_t load(struct pk_env *e, const char *name, const char *suffix,
                     const uint8_t *want, struct loaded *l)
{
    memset(l, 0, sizeof(*l));
    l->m = calloc(1, sizeof(*l->m));
    if (!l->m)
        return ERR_NO_MEMORY;
    status_t st = e->have_key ? pk_db_load(e, name, suffix, true, &l->text, &l->len, l->m)
                              : ERR_ACCESS_DENIED;
    if (st == OK && want) {
        uint8_t d[SHA256_BYTES];
        sha256(l->text, l->len, d);
        if (memcmp(d, want, SHA256_BYTES))
            st = ERR_ACCESS_DENIED;
    }
    if (st == ERR_NOT_SUPPORTED || st == ERR_INVALID_ARGS || st == ERR_OUT_OF_RANGE)
        st = ERR_ACCESS_DENIED;   /* not a manifest of ours */
    if (st != OK)
        unload(l);
    return st;
}

static void copy_str(char *out, struct pkg_str s)
{
    uint32_t n = s.n < PK_NAME - 1 ? s.n : PK_NAME - 1;
    memcpy(out, s.p, n);
    out[n] = '\0';
}

static bool is_of(const struct pkg_manifest *m, struct pkg_str name, struct pkg_str version,
                  uint32_t revision)
{
    return !pkg_str_cmp(m->name, name) && !pkg_str_cmp(m->version, version) &&
           m->revision == revision;
}

static bool same_tag(const struct pkg_journal *j)
{
    return !pkg_str_cmp(j->version, j->old_version) && j->revision == j->old_revision;
}

/* ---- init ------------------------------------------------------------------------------- */

static status_t revoke_tag(struct pk_env *e, struct pkg_str name, struct pkg_str version,
                           uint32_t revision)
{
    char n[PK_NAME], v[PK_NAME];
    uint32_t lines;
    if (!e->ctl.revoke)
        return ERR_NOT_SUPPORTED;
    if (pk_cut(e, "revoke", false) != PK_GO)
        return ERR_IO;
    copy_str(n, name);
    copy_str(v, version);
    return e->ctl.revoke(e->ctl.ctx, n, v, revision, &lines);
}

/* ---- the files of a manifest ----------------------------------------------------------- */

/* Each of m's files (not those keep also has, by FAT's names), removed, at
 * its path or, staged, at `.new`. ERR_BUSY if any is in use (the others
 * are removed all the same). */
static status_t remove_files(struct pk_env *e, const struct pkg_manifest *m,
                             const struct pkg_manifest *keep, bool staged)
{
    status_t result = OK;
    for (uint32_t i = 0; i < m->nfiles; i++) {
        bool kept = false;
        for (uint32_t k = 0; keep && !kept && k < keep->nfiles; k++)
            kept = pkg_path_fold_equal(m->files[i].path, keep->files[k].path);
        if (kept)
            continue;
        char path[PK_PATH];
        pk_path(e, m->files[i].path, staged ? ".new" : "", path);
        status_t st = pk_remove(e, path);
        if (st == ERR_BUSY)
            result = ERR_BUSY;
        else if (st != OK)
            return st;
    }
    return result;
}

/* Directories a removal left empty, deepest first (best effort: a
 * directory with anything in it stays). */
static void remove_dirs(struct pk_env *e, const struct pkg_manifest *m)
{
    for (uint32_t i = m->nfiles; i-- > 0;) {
        char path[PK_PATH];
        pk_path(e, m->files[i].path, "", path);
        for (char *s = strrchr(path, '/'); s && strlen(path) > strlen(e->prefix) + 1;
             s = strrchr(path, '/')) {
            *s = '\0';
            if (strlen(path) <= strlen(e->prefix) + 1 || pk_remove(e, path) != OK)
                break;
        }
    }
}

/* ---- the journal's steps ----------------------------------------------------------------- */

/* The journal one step on (past the last: gone). */
static status_t advance(struct pk_env *e, struct pkg_journal *j, enum pkg_step to)
{
    j->step = to;
    j->nbusy = 0;
    return pk_journal_put(e, j);
}

/* Each `.new` file moved to its path, the old version's other files
 * removed. ERR_BUSY if a program runs from some (they are listed in the
 * journal, which stays at its step). */
static status_t swap(struct pk_env *e, struct pkg_journal *j, const struct pkg_manifest *nm,
                     const struct pkg_manifest *om)
{
    uint32_t nbusy = 0;
    for (uint32_t i = 0; i < nm->nfiles; i++) {
        char from[PK_PATH], to[PK_PATH];
        pk_path(e, nm->files[i].path, ".new", from);
        pk_path(e, nm->files[i].path, "", to);
        if (!pk_exists(from))
            continue;   /* moved already */
        status_t st = pk_remove(e, to);
        if (st == ERR_BUSY) {
            j->busy[nbusy++] = nm->files[i].path;
            continue;
        }
        st = st == OK ? pk_rename(e, from, to) : st;
        if (st != OK)
            return st;
    }
    status_t st = om ? remove_files(e, om, nm, false) : OK;
    if (st == ERR_BUSY)
        st = OK;   /* an old file that stays; nothing approves it any more */
    if (st == OK)
        st = pk_flush(e);
    if (st != OK || !nbusy)
        return st;
    j->nbusy = nbusy;
    st = pk_journal_put(e, j);
    return st == OK ? ERR_BUSY : st;
}

/* db/<name>.new made db/<name>, and the sums cut to the new one. */
static status_t record(struct pk_env *e, struct pkg_journal *j)
{
    char name[PK_NAME], now[PK_PATH], fresh[PK_PATH];
    copy_str(name, j->name);
    pk_db_path(e, name, "", now);
    pk_db_path(e, name, ".new", fresh);
    status_t st = OK;
    if (pk_exists(fresh))
        st = pk_move(e, fresh, now);
    uint8_t sum[SHA256_BYTES];
    if (st == OK && pk_sum_get(e, name, j->manifest, sum))
        st = pk_sum_put(e, name, j->manifest, sum, NULL, NULL);
    return st == OK ? pk_flush(e) : st;
}

/* An install or an upgrade from its commit point on. */
static status_t forward(struct pk_env *e, struct pkg_journal *j)
{
    char name[PK_NAME];
    copy_str(name, j->name);
    struct loaded nw, old = { 0 };
    status_t st = load(e, name, ".new", j->manifest, &nw);
    if (st == ERR_NOT_FOUND)   /* recorded already: the rename was the last thing done */
        st = load(e, name, "", j->manifest, &nw);
    if (st == OK && !is_of(nw.m, j->name, j->version, j->revision)) {
        unload(&nw);
        st = ERR_ACCESS_DENIED;
    }
    if (st != OK)
        return st;
    if (j->op == PKG_OP_UPGRADE && load(e, name, "", j->old_manifest, &old) == OK &&
        !is_of(old.m, j->name, j->old_version, j->old_revision))
        unload(&old);
    if (j->step <= PKG_STEP_APPROVED) {
        st = swap(e, j, nw.m, old.m);
        if (st == OK)
            st = advance(e, j, PKG_STEP_SWAPPED);
    }
    if (st == OK && j->op == PKG_OP_UPGRADE && j->step <= PKG_STEP_SWAPPED) {
        st = same_tag(j) ? OK : revoke_tag(e, j->name, j->old_version, j->old_revision);
        if (st == OK)
            st = advance(e, j, PKG_STEP_REVOKED);
    }
    if (st == OK)
        st = record(e, j);
    if (st == OK)
        st = advance(e, j, PKG_STEP_RECORDED);
    if (st == OK)
        st = pk_journal_drop(e);
    unload(&nw);
    unload(&old);
    return st;
}

/* An install or an upgrade before its commit point: the staged files, the
 * lines the new version got (a cut inside init's write) unless it is the
 * one installed, and the new manifest. */
static status_t undo(struct pk_env *e, struct pkg_journal *j)
{
    char name[PK_NAME], fresh[PK_PATH], now[PK_PATH];
    copy_str(name, j->name);
    pk_db_path(e, name, ".new", fresh);
    pk_db_path(e, name, "", now);
    struct loaded l;
    status_t st = load(e, name, ".new", j->manifest, &l);
    if (st == ERR_NO_MEMORY || st == ERR_IO)
        return st;
    /* No manifest of the journal's (a cut before it was written, or while): nothing
     * was staged yet, or what was isn't known and stays at `.new`, which nothing
     * runs or reads. */
    if (st == OK && is_of(l.m, j->name, j->version, j->revision))
        st = remove_files(e, l.m, NULL, true);
    else
        st = OK;
    if (st == ERR_BUSY)
        st = OK;
    unload(&l);
    if (st != OK)
        return st;
    bool same = j->op == PKG_OP_UPGRADE ? same_tag(j) : pk_exists(now);
    st = same ? OK : revoke_tag(e, j->name, j->version, j->revision);
    if (st == OK)
        st = pk_remove(e, fresh);
    return st == OK ? pk_journal_drop(e) : st;
}

/* A remove, from wherever it was. */
static status_t forward_remove(struct pk_env *e, struct pkg_journal *j)
{
    char name[PK_NAME], now[PK_PATH];
    copy_str(name, j->name);
    pk_db_path(e, name, "", now);
    status_t st = OK;
    if (j->step <= PKG_STEP_BEGIN) {
        st = revoke_tag(e, j->name, j->old_version, j->old_revision);
        if (st == OK)
            st = advance(e, j, PKG_STEP_REVOKED);
    }
    struct loaded old;
    if (st == OK && j->step <= PKG_STEP_REVOKED) {
        st = load(e, name, "", j->old_manifest, &old);
        if (st == ERR_NOT_FOUND) {
            st = OK;   /* recorded already */
        } else if (st == OK) {
            st = remove_files(e, old.m, NULL, false);
            if (st == OK) {
                remove_dirs(e, old.m);
                st = pk_flush(e);
            }
            unload(&old);
        }
        if (st == OK)
            st = advance(e, j, PKG_STEP_REMOVED);
    }
    if (st == OK && j->step <= PKG_STEP_REMOVED) {
        st = pk_remove(e, now);
        pk_db_path(e, name, ".sum", now);
        if (st == OK)
            st = pk_remove(e, now);
        if (st == OK)
            st = advance(e, j, PKG_STEP_RECORDED);
    }
    return st == OK ? pk_journal_drop(e) : st;
}

/* The commit point: an install's or an upgrade's `approved`, a remove's
 * `begin`. */
static status_t carry_on(struct pk_env *e, struct pkg_journal *j)
{
    if (j->op == PKG_OP_REMOVE)
        return forward_remove(e, j);
    return j->step >= PKG_STEP_APPROVED ? forward(e, j) : undo(e, j);
}

/* ---- settle --------------------------------------------------------------------------- */

status_t pk_settle(struct pk_env *e, char what[PK_PATH])
{
    what[0] = '\0';
    struct pkg_journal *j = calloc(1, sizeof(*j));
    uint8_t *text = NULL;
    const char *why = NULL;
    if (!j)
        return ERR_NO_MEMORY;
    status_t st = pk_journal_get(e, j, &text, &why);
    if (st == ERR_NOT_FOUND) {   /* nothing began; a journal.new left over goes */
        char fresh[PK_PATH];
        pk_db_path(e, "journal", ".new", fresh);
        st = pk_exists(fresh) ? pk_remove(e, fresh) : OK;
        free(j);
        return st;
    }
    if (st != OK) {
        snprintf(what, PK_PATH, "the journal %s/db/journal can't be read (%s): remove it by "
                 "hand when nothing is half installed", e->prefix, why ? why : status_str(st));
    } else if (!e->have_key) {
        st = ERR_ACCESS_DENIED;
        snprintf(what, PK_PATH, "an interrupted %s of %.*s is waiting, and this build has no "
                 "ports key to settle it", pkg_op_name(j->op), (int)j->name.n, j->name.p);
    } else {
        bool fwd = j->op == PKG_OP_REMOVE || j->step >= PKG_STEP_APPROVED;
        st = carry_on(e, j);
        snprintf(what, PK_PATH, "%s of %.*s %s (it had reached %s)%s%s", pkg_op_name(j->op),
                 (int)j->name.n, j->name.p, fwd ? "finished" : "undone", pkg_step_name(j->step),
                 st == OK ? "" : ": stopped, ", st == OK ? "" : status_str(st));
    }
    free(text);
    free(j);
    return st;
}

/* ---- install and upgrade ------------------------------------------------------------------ */

struct run {
    struct pk_env         *e;
    const struct pk_job   *job;
    struct pkg_manifest   *nm;       /* the new manifest, parsed from job->text */
    struct pkg_journal    *j;
    char                  *why;      /* a reason for the owner (PK_WHY bytes) */
    char                   over[PK_NAME];
    uint8_t                osha[SHA256_BYTES];   /* the installed manifest's SHA-256 (upgrade) */
    bool                   have_old;
};

/* The free space on the mount the prefix is on, against what the files need. */
static status_t room(struct pk_env *e, uint64_t need)
{
    char mnt[PK_PATH];
    const char *second = strchr(e->prefix + 1, '/');
    snprintf(mnt, sizeof(mnt), "%.*s", second ? (int)(second - e->prefix) : (int)strlen(e->prefix),
             e->prefix);
    uint64_t total, avail;
    if (fs_statfs(mnt, &total, &avail, NULL, NULL) != OK)
        return OK;   /* the mount doesn't say: the writes will */
    return avail < need + (1u << 20) ? ERR_NO_SPACE : OK;
}

/* The new manifest checked, the old one found (an upgrade), the journal made. */
static status_t prepare(struct run *r)
{
    struct pk_env *e = r->e;
    const struct pk_job *job = r->job;
    const char *why = NULL;
    if (!e->have_key)
        return ERR_ACCESS_DENIED;
    status_t st = pkg_manifest_parse(job->text, job->len, e->key, r->nm, &why);
    if (st != OK)
        return st;
    why = pk_manifest_problem(r->nm);
    if (why) {
        snprintf(r->why, PK_WHY, "%s", why);
        return ERR_ACCESS_DENIED;
    }
    char path[PK_PATH];
    pk_db_path(e, job->name, "", path);
    bool installed = pk_exists(path);
    if (job->op == PKG_OP_INSTALL && installed)
        return ERR_ALREADY_EXISTS;
    if (job->op == PKG_OP_UPGRADE) {
        struct loaded old;
        st = installed ? load(e, job->name, "", NULL, &old) : ERR_NOT_FOUND;
        if (st != OK)
            return st;
        sha256(old.text, old.len, r->osha);
        copy_str(r->over, old.m->version);
        r->j->old_revision = old.m->revision;
        r->have_old = true;
        unload(&old);
    }
    return room(e, r->nm->total);
}

static void fill_journal(struct run *r)
{
    struct pkg_journal *j = r->j;
    j->op = r->job->op;
    j->step = PKG_STEP_BEGIN;
    j->name = r->nm->name;
    j->version = r->nm->version;
    j->revision = r->nm->revision;
    memcpy(j->manifest, r->nm->sha256, SHA256_BYTES);
    if (r->have_old) {
        j->old_version = (struct pkg_str){ r->over, (uint32_t)strlen(r->over) };
        memcpy(j->old_manifest, r->osha, SHA256_BYTES);
    }
}

/* The new manifest into db/<name>.new, then every file staged from the
 * source; the package whole is checked against the index's size and hash. */
static status_t stage(struct run *r, uint8_t jsha[SHA256_BYTES])
{
    struct pk_env *e = r->e;
    const struct pk_job *job = r->job;
    char fresh[PK_PATH];
    pk_db_path(e, job->name, ".new", fresh);
    status_t st = pk_write(e, fresh, job->text, job->len);
    if (st != OK)
        return st;
    struct pk_unpack_use use = { e, true, r->nm->sha256 };
    struct pk_unpack *u = pk_unpack_new(&use);
    if (!u)
        return ERR_NO_MEMORY;
    st = job->src->run(job->src->ctx, pk_unpack_sink, u);
    uint64_t size = 0;
    if (st == OK)
        st = pk_unpack_finish(u, &size, jsha);
    if (st == OK && job->size && (size != job->size || memcmp(jsha, job->sha, SHA256_BYTES)))
        st = ERR_ACCESS_DENIED;   /* not the package the index names */
    if (st != OK)
        pk_say("pkg: %s: %s\n", job->name, pk_unpack_why(u)[0] ? pk_unpack_why(u) : status_str(st));
    pk_unpack_free(u);
    return st;
}

/* init asked to approve the staged programs. */
static status_t approve(struct run *r, char why[PK_WHY])
{
    struct pk_env *e = r->e;
    struct pk_verdict v = { .verdict = ERR_INTERNAL, .program = 0xffffffffu };
    if (!e->ctl.approve)
        return ERR_NOT_SUPPORTED;
    if (pk_cut(e, "approve", false) != PK_GO)
        return ERR_IO;
    status_t st = e->ctl.approve(e->ctl.ctx, PK_APPROVE_STAGED | (e->jam ? PK_APPROVE_JAM : 0),
                                 r->job->text, r->job->len, &v);
    if (st == OK && v.verdict != OK) {
        snprintf(why, PK_WHY, "%s", v.why);
        st = v.verdict;
    }
    return st;
}

/* The transaction up to its commit point; the journal is at `staged`. */
static status_t before_commit(struct run *r, char why[PK_WHY])
{
    uint8_t jsha[SHA256_BYTES];
    struct pk_env *e = r->e;
    status_t st = pk_journal_put(e, r->j);
    if (st == OK)
        st = stage(r, jsha);
    if (st == OK) {
        uint8_t oj[SHA256_BYTES];
        bool keep = r->have_old && pk_sum_get(e, r->job->name, r->osha, oj);
        st = pk_sum_put(e, r->job->name, r->nm->sha256, jsha, keep ? r->osha : NULL,
                        keep ? oj : NULL);
    }
    if (st == OK)
        st = advance(e, r->j, PKG_STEP_STAGED);
    if (st == OK)
        st = approve(r, why);
    if (st == OK)
        st = advance(e, r->j, PKG_STEP_APPROVED);
    return st;
}

static status_t run_install(struct pk_env *e, const struct pk_job *job, char why[PK_WHY])
{
    struct run r = { .e = e, .job = job, .why = why };
    r.nm = calloc(1, sizeof(*r.nm));
    r.j = calloc(1, sizeof(*r.j));
    status_t st = r.nm && r.j ? prepare(&r) : ERR_NO_MEMORY;
    if (st == OK) {
        fill_journal(&r);
        st = before_commit(&r, why);
        if (st != OK && !e->dead && r.j->step < PKG_STEP_APPROVED) {
            status_t back = undo(e, r.j);   /* before the commit point: all of it goes */
            if (back != OK)
                pk_say("pkg: %s: it could not be undone (%s): the next run will\n", job->name,
                       status_str(back));
        } else if (st == OK) {
            st = forward(e, r.j);
        }
    }
    free(r.nm);
    free(r.j);
    return st;
}

/* ---- remove ---------------------------------------------------------------------------- */

static status_t run_remove(struct pk_env *e, const struct pk_job *job)
{
    struct loaded old;
    struct pkg_journal *j = calloc(1, sizeof(*j));
    if (!j)
        return ERR_NO_MEMORY;
    status_t st = load(e, job->name, "", NULL, &old);
    if (st == OK) {
        uint8_t sha[SHA256_BYTES];
        sha256(old.text, old.len, sha);
        j->op = PKG_OP_REMOVE;
        j->step = PKG_STEP_BEGIN;
        j->name = old.m->name;
        j->old_version = old.m->version;
        j->old_revision = old.m->revision;
        memcpy(j->old_manifest, sha, SHA256_BYTES);
        st = pk_journal_put(e, j);
        if (st == OK)
            st = forward_remove(e, j);
        unload(&old);
    }
    free(j);
    return st;
}

status_t pk_run(struct pk_env *e, const struct pk_job *job, char why[PK_WHY])
{
    why[0] = '\0';
    return job->op == PKG_OP_REMOVE ? run_remove(e, job) : run_install(e, job, why);
}
