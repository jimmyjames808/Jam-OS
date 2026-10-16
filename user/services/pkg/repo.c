/* The repository (docs/M13.7-PLAN.md sections 7 and 8): static files that
 * tools/ports.py repo made and tools/ports-server.py serves.
 *
 *   INDEX    the signed catalogue, with a serial that only grows
 *   VULNS    the signed advisories
 *   pkg/...  the packages, at names derived from the index (never read from it)
 *
 * The transport is plain HTTP and is trusted for nothing: the signatures
 * are what count. The index is verified against the ports key (this build's
 * /boot/ports.pub) before a field of it is read, and refused if its serial is
 * below the highest accepted (db/serial), the simplest form of TUF's
 * rollback rule: a replayed old index would offer old, vulnerable packages.
 * A package is held to the size and SHA-256 its index line names. */
#include "pkg.h"

/* A body collected into memory, up to a bound. */
struct buf {
    uint8_t *p;
    size_t   n, cap;
};

static status_t buf_sink(void *ctx, const void *data, size_t n)
{
    struct buf *b = ctx;
    if (n > b->cap - b->n)
        return ERR_OUT_OF_RANGE;
    memcpy(b->p + b->n, data, n);
    b->n += n;
    return OK;
}

status_t pk_repo_set(struct pk_repo *r, const char *url, char why[160])
{
    status_t st = http_url_parse(url, &r->base);
    if (st == ERR_NOT_SUPPORTED)
        snprintf(why, 160, "%s: only http:// is spoken (https needs TLS, which Jam OS does not "
                 "have)", url);
    else if (st != OK)
        snprintf(why, 160, "%s is not an address pkg can use (http://host[:port][/dir])", url);
    size_t n = strlen(r->base.path);
    if (st == OK && n + 1 < sizeof(r->base.path) && r->base.path[n - 1] != '/') {
        r->base.path[n] = '/';
        r->base.path[n + 1] = '\0';
    }
    return st;
}

/* rel of the repository into new memory (NUL-terminated past its length). */
static status_t get_text(const struct pk_repo *r, const char *rel, uint64_t max, uint8_t **text,
                         size_t *len, char why[160])
{
    struct buf b = { malloc(max + 1), 0, max };
    struct pk_get g = { .max = max };
    if (!b.p)
        return ERR_NO_MEMORY;
    status_t st = pk_get_run(&r->base, rel, &g, buf_sink, &b);
    if (st != OK) {
        snprintf(why, 160, "%s", g.why[0] ? g.why : status_str(st));
        free(b.p);
        return st;
    }
    b.p[b.n] = '\0';
    *text = b.p;
    *len = b.n;
    return OK;
}

/* A signed list refused: in words. */
static void refused_words(status_t st, const char *what, const char *why, char out[160])
{
    if (st == ERR_BAD_STATE)
        snprintf(out, 160, "the repository's %s is older than one already accepted: refused "
                 "(a repository made again from the start: `pkg update -f`)", what);
    else if (st == ERR_ACCESS_DENIED)
        snprintf(out, 160, "the repository's %s is not signed by this build's ports key", what);
    else
        snprintf(out, 160, "the repository's %s is refused: %s", what,
                 why ? why : status_str(st));
}

status_t pk_repo_index(const struct pk_repo *r, struct pk_env *e, bool force,
                       struct pkg_index *x, uint8_t **text, char why[160])
{
    struct pkg_last idx, vul;
    size_t len;
    const char *reason = NULL;
    if (!e->have_key) {
        snprintf(why, 160, "this build has no ports key, so it takes no repository");
        return ERR_ACCESS_DENIED;
    }
    status_t st = pk_last_get(e, &idx, &vul);
    if (st != OK) {
        snprintf(why, 160, "%s/db/serial is damaged: remove it by hand to start over", e->prefix);
        return st;
    }
    st = get_text(r, "INDEX", PKG_INDEX_MAX, text, &len, why);
    if (st != OK)
        return st;
    st = pkg_index_parse(*text, len, e->key, force ? NULL : &idx, x, &reason);
    if (st != OK) {
        refused_words(st, "index", reason, why);
        free(*text);
        return st;
    }
    if (e->abi[0] && strcmp(e->abi, x->abi)) {
        snprintf(why, 160, "the repository's packages are built for another version of Jam OS "
                 "(abi %s, this one is %s)", x->abi, e->abi);
        free(*text);
        return ERR_ACCESS_DENIED;
    }
    if (x->serial != idx.serial || memcmp(x->sha256, idx.sha256, SHA256_BYTES)) {
        idx.serial = x->serial;
        memcpy(idx.sha256, x->sha256, SHA256_BYTES);
        (void)pk_last_put(e, &idx, &vul);   /* not kept: asked for again next time */
    }
    return OK;
}

status_t pk_repo_vulns(const struct pk_repo *r, struct pk_env *e, struct pkg_vulns *v,
                       uint8_t **text, char why[160])
{
    struct pkg_last idx, vul;
    size_t len;
    const char *reason = NULL;
    if (!e->have_key) {
        snprintf(why, 160, "this build has no ports key, so it takes no repository");
        return ERR_ACCESS_DENIED;
    }
    status_t st = pk_last_get(e, &idx, &vul);
    if (st != OK) {
        snprintf(why, 160, "%s/db/serial is damaged: remove it by hand to start over", e->prefix);
        return st;
    }
    st = get_text(r, "VULNS", PKG_VULNS_MAX, text, &len, why);
    if (st != OK)
        return st;
    st = pkg_vulns_parse(*text, len, e->key, &vul, v, &reason);
    if (st != OK) {
        refused_words(st, "list of advisories", reason, why);
        free(*text);
        return st;
    }
    if (v->serial != vul.serial || memcmp(v->sha256, vul.sha256, SHA256_BYTES)) {
        vul.serial = v->serial;
        memcpy(vul.sha256, v->sha256, SHA256_BYTES);
        (void)pk_last_put(e, &idx, &vul);
    }
    return OK;
}

/* ---- a package on the repository ------------------------------------------------------- */

status_t pk_remote_make(struct pk_remote *rm, const struct pk_repo *r, const struct pkg_ipkg *ip,
                        const char *label)
{
    memset(rm, 0, sizeof(*rm));
    rm->repo = r;
    rm->size = ip->size;
    rm->label = label;
    return pkg_repo_path('p', ip->name, ip->version, ip->revision, ip->sha256, rm->rel,
                         sizeof(rm->rel));
}

/* A quarter of the package has come: say so. */
static void progress(void *ctx, uint64_t got, uint64_t total)
{
    struct pk_remote *rm = ctx;
    unsigned q = total ? (unsigned)(got * 4 / total) : 0;
    if (q > rm->said && q < 4) {
        rm->said = q;
        pk_say("%u%% of %s\n", q * 25, rm->label);
    }
}

static status_t remote_run(void *ctx, pk_feed_fn feed, void *fctx)
{
    struct pk_remote *rm = ctx;
    struct pk_get g = { .want = rm->size, .max = rm->size };
    if (!rm->quiet) {
        g.progress = progress;
        g.progress_ctx = rm;
    }
    rm->said = 0;
    status_t st = pk_get_run(&rm->repo->base, rm->rel, &g, feed, fctx);
    if (st != OK && g.why[0])
        pk_say("pkg: %s: %s\n", rm->label, g.why);
    return st;
}

void pk_remote_source(struct pk_remote *rm, struct pk_source *src)
{
    *src = (struct pk_source){ rm, remote_run };
}
