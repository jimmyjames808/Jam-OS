/* The repository's INDEX (`jamos-ports 1`) and VULNS (`jamos-vulns 1`)
 * and what `pkg` does with them (<pkgfmt.h>, the plan's sections 7 and
 * 13): read after the signature, each line against its grammar in its
 * fixed place, the order and counts bounded, the rules across lines
 * (every dependency resolves inside the index, every package has its
 * source line), and the replay rule on the serial. The repository's file
 * names are derived here, never read from it. */
#include "pkgint.h"

static const char *const index_keys[] = {
    "package", "runs", "src", "signature", "serial", "made", "abi", NULL,
};
static const char *const vulns_keys[] = { "vuln", "signature", "serial", "made", NULL };

#define ARCHIVE_MIN (3u * PKG_BLOCK)   /* a package is at least its manifest header and two end blocks */

static bool next_words(struct pkg_lines *ls, const char *key, struct pkg_str *w, unsigned lo,
                       unsigned hi, unsigned *n)
{
    struct pkg_str line;
    return pkg_next_line(ls, &line) && pkg_words(line, key, w, lo, hi, n);
}

/* `serial`, `made` (and for the index `abi`) lines, then extensions. */
static status_t head_lines(struct pkg_lines *ls, const struct pkg_last *last,
                           const uint8_t sha[SHA256_BYTES], uint64_t *serial, char made[21],
                           char *abi, const char *const *known, const char **why)
{
    struct pkg_str line, w[1];
    NEED(pkg_next_line(ls, &line), "a text cut short");
    status_t st = pkg_serial_line(line, last, sha, serial, why);
    if (st != OK)
        return st;
    NEED(next_words(ls, "made", w, 1, 1, NULL) && pkg_f_made(w[0], made), "the made line");
    NEED(!abi || (next_words(ls, "abi", w, 1, 1, NULL) && pkg_f_abi(w[0], abi)), "the abi line");
    return pkg_extensions(ls, known, why);
}

/* ---- the index ------------------------------------------------------------------------- */

static status_t runs_lines(struct pkg_lines *ls, const struct pkg_index *x, struct pkg_ipkg *p,
                           const char **why)
{
    struct pkg_str line, w[3], prev = { NULL, 0 };
    unsigned n;
    p->nruns = 0;
    p->runs_at = (uint32_t)(ls->p - x->text);
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "runs")) {
        pkg_next_line(ls, &line);
        NEED(pkg_words(line, "runs", w, 2, 3, &n) && !pkg_str_cmp(w[0], p->name) &&
                 pkg_f_name(w[1]) && (n == 2 || pkg_f_version(w[2])), "a runs line");
        NEED(p->nruns < PKG_DEPS_MAX, "too many runs lines");
        NEED(p->nruns == 0 || pkg_increasing(prev, w[1]), "runs lines not in order");
        NEED(pkg_str_cmp(w[1], p->name), "a package that runs itself");
        prev = w[1];
        p->nruns++;
    }
    return OK;
}

static status_t package_line(struct pkg_lines *ls, struct pkg_index *x, const char **why)
{
    struct pkg_str line, w[6];
    struct pkg_ipkg *p = &x->packages[x->npackages];
    NEED(x->npackages < PKG_PACKAGES_MAX, "too many packages");
    pkg_next_line(ls, &line);
    NEED(pkg_words(line, "package", w, 6, 6, NULL) && pkg_f_name(w[0]) && pkg_f_version(w[1]) &&
             pkg_f_revision(w[2], &p->revision) && pkg_f_size(w[3], PKG_ARCHIVE_MAX, &p->size) &&
             pkg_f_sha(w[4], p->sha256) && pkg_f_name(w[5]), "a package line");
    NEED(p->size >= ARCHIVE_MIN, "a package too small to be one");
    NEED(x->npackages == 0 || pkg_increasing(x->packages[x->npackages - 1].name, w[0]),
         "package lines not in strictly increasing order (or a package twice)");
    p->name = w[0];
    p->version = w[1];
    p->port = w[5];
    p->source = 0;
    status_t st = runs_lines(ls, x, p, why);
    if (st == OK)
        x->npackages++;
    return st;
}

static status_t src_lines(struct pkg_lines *ls, struct pkg_index *x, const char **why)
{
    struct pkg_str line, w[5];
    x->nsources = 0;
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "src")) {
        struct pkg_isrc *s = &x->sources[x->nsources];
        pkg_next_line(ls, &line);
        NEED(x->nsources < PKG_PACKAGES_MAX, "too many src lines");
        NEED(pkg_words(line, "src", w, 5, 5, NULL) && pkg_f_name(w[0]) && pkg_f_version(w[1]) &&
                 pkg_f_revision(w[2], &s->revision) &&
                 pkg_f_size(w[3], PKG_ARCHIVE_MAX, &s->size) && pkg_f_sha(w[4], s->sha256),
             "a src line");
        NEED(x->nsources == 0 || pkg_increasing(x->sources[x->nsources - 1].port, w[0]),
             "src lines not in strictly increasing order");
        s->port = w[0];
        s->version = w[1];
        s->used = false;
        x->nsources++;
    }
    return OK;
}

const struct pkg_ipkg *pkg_index_find(const struct pkg_index *x, struct pkg_str name)
{
    unsigned lo = 0, hi = x->npackages;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = pkg_str_cmp(x->packages[mid].name, name);
        if (!c)
            return &x->packages[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

unsigned pkg_index_runs(const struct pkg_index *x, const struct pkg_ipkg *p,
                        struct pkg_dep out[PKG_DEPS_MAX])
{
    struct pkg_lines ls = { x->text + p->runs_at, x->text + x->len };
    struct pkg_str line, w[3];
    unsigned n = 0, k;
    while (n < p->nruns && pkg_next_line(&ls, &line) && pkg_words(line, "runs", w, 2, 3, &k)) {
        out[n].name = w[1];
        out[n].least = k == 3 ? w[2] : (struct pkg_str){ NULL, 0 };
        n++;
    }
    return n;
}

/* The rules across lines: every dependency is a package of the index at
 * the least version it asks for; every package's source is listed at its
 * version and revision, and every source is some package's. */
static status_t index_whole(struct pkg_index *x, const char **why)
{
    for (unsigned i = 0; i < x->npackages; i++) {
        struct pkg_ipkg *p = &x->packages[i];
        struct pkg_dep deps[PKG_DEPS_MAX];
        unsigned n = pkg_index_runs(x, p, deps);
        for (unsigned k = 0; k < n; k++) {
            const struct pkg_ipkg *d = pkg_index_find(x, deps[k].name);
            NEED(d, "a package runs one the index does not have");
            NEED(!deps[k].least.n || pkg_version_cmp(d->version, deps[k].least) >= 0,
                 "a package runs a version of another the index does not have");
        }
        unsigned lo = 0, hi = x->nsources;
        bool found = false;
        while (lo < hi && !found) {
            unsigned mid = lo + (hi - lo) / 2;
            int c = pkg_str_cmp(x->sources[mid].port, p->port);
            if (!c) {
                found = true;
                p->source = mid;
            } else if (c < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        NEED(found, "a package has no src line");
        struct pkg_isrc *s = &x->sources[p->source];
        NEED(!pkg_str_cmp(s->version, p->version) && s->revision == p->revision,
             "a src line is not the package's version and revision");
        s->used = true;
    }
    for (unsigned i = 0; i < x->nsources; i++)
        NEED(x->sources[i].used, "a src line no package uses");
    return OK;
}

static status_t index_body(struct pkg_lines *ls, struct pkg_index *x, const struct pkg_last *last,
                           const char **why)
{
    struct pkg_str line;
    status_t st = head_lines(ls, last, x->sha256, &x->serial, x->made, x->abi, index_keys, why);
    x->npackages = 0;
    while (st == OK && pkg_peek_line(ls, &line) && pkg_line_is(line, "package"))
        st = package_line(ls, x, why);
    if (st == OK)
        st = src_lines(ls, x, why);
    if (st != OK)
        return st;
    NEED(!pkg_peek_line(ls, &line), "a line out of place");
    return index_whole(x, why);
}

static status_t index_read(const void *text, size_t len, const uint8_t *key,
                           const struct pkg_last *last, struct pkg_index *x, const char **why)
{
    struct pkg_lines ls;
    status_t st = pkg_signed_text(text, len, PKG_FIRST_INDEX, PKG_INDEX_MAX, key, &ls, x->sha256,
                                  why);
    x->text = text;
    x->len = len;
    return st == OK ? index_body(&ls, x, last, why) : st;
}

status_t pkg_index_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                         const struct pkg_last *last, struct pkg_index *x, const char **why)
{
    if (!key)
        return pkg_fail(why, ERR_INVALID_ARGS, "no key");
    return index_read(text, len, key, last, x, why);
}

status_t pkg_index_parse_unchecked(const void *text, size_t len, struct pkg_index *x,
                                   const char **why)
{
    return index_read(text, len, NULL, NULL, x, why);
}

status_t pkg_index_check(const struct pkg_ipkg *p, uint64_t size, const uint8_t sha256[SHA256_BYTES])
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < SHA256_BYTES; i++)
        diff |= (uint8_t)(p->sha256[i] ^ sha256[i]);
    return p->size == size && !diff ? OK : ERR_ACCESS_DENIED;
}

/* ---- repository names ------------------------------------------------------------------------ */

status_t pkg_repo_path(char kind, struct pkg_str name, struct pkg_str version, uint32_t revision,
                       const uint8_t sha256[SHA256_BYTES], char *out, size_t cap)
{
    struct pkg_out o = { out, cap, 0 };
    if (kind != 'p' && kind != 's')
        return ERR_INVALID_ARGS;
    pkg_put_str(&o, kind == 'p' ? "pkg/" : "src/");
    pkg_put_bytes(&o, name.p, name.n);
    pkg_put_str(&o, "-");
    pkg_put_bytes(&o, version.p, version.n);
    pkg_put_str(&o, "-r");
    pkg_put_uint(&o, revision);
    pkg_put_str(&o, "-");
    pkg_put_hex(&o, sha256, 8);
    pkg_put_str(&o, kind == 'p' ? ".jpk" : ".src.tar");
    if (o.n >= cap)
        return ERR_BUFFER_TOO_SMALL;
    out[o.n] = '\0';
    return OK;
}

/* ---- VULNS ----------------------------------------------------------------------------------- */

static status_t vuln_line(struct pkg_str line, struct pkg_vuln *e, const char **why)
{
    struct pkg_str w[6];
    NEED(pkg_split_rest(line, "vuln", w, 6), "a vuln line");
    NEED(pkg_f_name(w[0]), "a vuln's component");
    NEED(pkg_str_eq(w[1], "-") || pkg_f_version(w[1]), "a vuln's first affected version");
    NEED(pkg_str_eq(w[2], "-") || pkg_f_version(w[2]), "a vuln's fixed version");
    e->fixrev = 0;
    NEED(pkg_str_eq(w[3], "-") || (!pkg_str_eq(w[2], "-") && pkg_f_revision(w[3], &e->fixrev)),
         "a vuln's fixrev (a revision of the fixed version, or '-')");
    NEED(pkg_f_id(w[4]) && pkg_f_text(w[5]), "a vuln's id or summary");
    e->component = w[0];
    e->first = pkg_str_eq(w[1], "-") ? (struct pkg_str){ NULL, 0 } : w[1];
    e->fixed = pkg_str_eq(w[2], "-") ? (struct pkg_str){ NULL, 0 } : w[2];
    e->id = w[4];
    e->summary = w[5];
    return OK;
}

static status_t vulns_body(struct pkg_lines *ls, struct pkg_vulns *v, const struct pkg_last *last,
                           const char **why)
{
    struct pkg_str line;
    status_t st = head_lines(ls, last, v->sha256, &v->serial, v->made, NULL, vulns_keys, why);
    v->n = 0;
    while (st == OK && pkg_peek_line(ls, &line) && pkg_line_is(line, "vuln")) {
        struct pkg_vuln *e = &v->v[v->n];
        pkg_next_line(ls, &line);
        NEED(v->n < PKG_VULNS_ENTRIES_MAX, "too many vuln lines");
        if ((st = vuln_line(line, e, why)) != OK)
            break;
        if (v->n) {
            const struct pkg_vuln *a = &v->v[v->n - 1];
            int c = pkg_str_cmp(a->component, e->component);
            NEED(c < 0 || (c == 0 && pkg_increasing(a->id, e->id)),
                 "vuln lines not in order (by component, then id)");
        }
        v->n++;
    }
    if (st != OK)
        return st;
    NEED(!pkg_peek_line(ls, &line), "a line out of place");
    return OK;
}

static status_t vulns_read(const void *text, size_t len, const uint8_t *key,
                           const struct pkg_last *last, struct pkg_vulns *v, const char **why)
{
    struct pkg_lines ls;
    status_t st = pkg_signed_text(text, len, PKG_FIRST_VULNS, PKG_VULNS_MAX, key, &ls, v->sha256,
                                  why);
    return st == OK ? vulns_body(&ls, v, last, why) : st;
}

status_t pkg_vulns_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                         const struct pkg_last *last, struct pkg_vulns *v, const char **why)
{
    if (!key)
        return pkg_fail(why, ERR_INVALID_ARGS, "no key");
    return vulns_read(text, len, key, last, v, why);
}

status_t pkg_vulns_parse_unchecked(const void *text, size_t len, struct pkg_vulns *v,
                                   const char **why)
{
    return vulns_read(text, len, NULL, NULL, v, why);
}

bool pkg_vuln_affected(const struct pkg_vuln *e, struct pkg_str version, uint32_t revision)
{
    if (e->first.n && pkg_version_cmp(version, e->first) < 0)
        return false;
    if (!e->fixed.n)
        return true;
    int c = pkg_version_cmp(version, e->fixed);
    return c < 0 || (c == 0 && e->fixrev > 0 && revision < e->fixrev);
}

unsigned pkg_audit(const struct pkg_vulns *v, const struct pkg_manifest *m,
                   const struct pkg_vuln **out, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < v->n; i++) {
        const struct pkg_vuln *e = &v->v[i];
        bool hit = !pkg_str_cmp(e->component, m->name) &&
                   pkg_vuln_affected(e, m->version, m->revision);
        if (hit && n++ < max)
            out[n - 1] = e;
        for (unsigned k = 0; k < m->nbuilt; k++) {
            const struct pkg_built *b = &m->built_with[k];
            if (!pkg_str_cmp(e->component, b->name) && pkg_vuln_affected(e, b->version, b->revision)
                && n++ < max)
                out[n - 1] = e;
        }
    }
    return n;
}
