/* What is installed: <prefix>/db/ (docs/M13.7-PLAN.md section 9).
 *
 *   db/<name>          the installed package's signed manifest
 *   db/<name>.new      the manifest of the version being installed (the
 *                      journal names its SHA-256); it becomes db/<name> as
 *                      the transaction's last step
 *   db/<name>.sum      which package a manifest came in: one line
 *                      "<manifest sha256> <package sha256>" per manifest
 *                      (at most the old and the new one in a transaction),
 *                      so a rebuilt package of the same version is seen as
 *                      newer by the index's hash. A line whose manifest is
 *                      not the installed one is stale and ignored.
 *   db/serial          the highest index and VULNS serials accepted, each
 *                      with the SHA-256 of its text (the replay rule of
 *                      <pkgfmt.h>); written as serial.new and renamed, and
 *                      read from serial.new if the rename was cut short
 *   db/journal         the transaction in progress (journal.c)
 *
 * A package called serial, journal, or ending .new or .sum would be one of
 * these files: pk_name_reserved refuses such names. */
#include <sha256.h>
#include "pkg.h"

#define SUM_MAX    1024u    /* a sum file is two lines of 130 bytes */
#define SERIAL_MAX 1024u

static bool hex_ok(const char *p, uint8_t out[SHA256_BYTES])
{
    return pkg_hex((struct pkg_str){ p, 2 * SHA256_BYTES }, out, SHA256_BYTES);
}

/* ---- sums ------------------------------------------------------------------------------- */

bool pk_sum_get(struct pk_env *e, const char *name, const uint8_t msha[SHA256_BYTES],
                uint8_t jsha[SHA256_BYTES])
{
    char path[PK_PATH];
    uint8_t *t;
    uint64_t n;
    pk_db_path(e, name, ".sum", path);
    if (pk_read(path, SUM_MAX, &t, &n) != OK)
        return false;
    bool found = false;
    for (uint64_t i = 0; !found && i + 130 <= n; i += 130) {
        uint8_t m[SHA256_BYTES], j[SHA256_BYTES];
        if (t[i + 64] != ' ' || t[i + 129] != '\n')
            break;
        if (hex_ok((const char *)t + i, m) && hex_ok((const char *)t + i + 65, j) &&
            !memcmp(m, msha, SHA256_BYTES)) {
            memcpy(jsha, j, SHA256_BYTES);
            found = true;
        }
    }
    free(t);
    return found;
}

status_t pk_sum_put(struct pk_env *e, const char *name, const uint8_t m1[SHA256_BYTES],
                    const uint8_t j1[SHA256_BYTES], const uint8_t *m2, const uint8_t *j2)
{
    char path[PK_PATH], text[2 * 130 + 1], hm[65], hj[65];
    int n = 0;
    sha256_hex(m1, hm);
    sha256_hex(j1, hj);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "%s %s\n", hm, hj);
    if (m2 && j2) {
        sha256_hex(m2, hm);
        sha256_hex(j2, hj);
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%s %s\n", hm, hj);
    }
    pk_db_path(e, name, ".sum", path);
    return pk_write(e, path, text, (size_t)n);
}

/* ---- serials ---------------------------------------------------------------------------- */

static bool last_line(const char *p, size_t n, const char *key, struct pkg_last *out)
{
    size_t k = strlen(key);
    if (n < k + 1 + 1 + 1 + 64 || memcmp(p, key, k) || p[k] != ' ')
        return false;
    uint64_t v = 0;
    size_t i = k + 1;
    for (; i < n && p[i] != ' '; i++) {
        if (p[i] < '0' || p[i] > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(p[i] - '0');
    }
    if (i + 1 + 64 != n || p[i] != ' ')
        return false;
    out->serial = v;
    return hex_ok(p + i + 1, out->sha256);
}

status_t pk_last_get(struct pk_env *e, struct pkg_last *index, struct pkg_last *vulns)
{
    char path[PK_PATH];
    uint8_t *t;
    uint64_t n;
    memset(index, 0, sizeof(*index));
    memset(vulns, 0, sizeof(*vulns));
    pk_db_path(e, "serial", "", path);
    status_t st = pk_read(path, SERIAL_MAX, &t, &n);
    if (st == ERR_NOT_FOUND) {
        pk_db_path(e, "serial", ".new", path);   /* the rename was cut short */
        st = pk_read(path, SERIAL_MAX, &t, &n);
    }
    if (st == ERR_NOT_FOUND)
        return OK;
    if (st != OK)
        return st;
    bool bad = false, have = !n;   /* an empty file: nothing accepted yet */
    for (uint64_t i = 0; !bad && i < n;) {
        uint64_t j = i;
        while (j < n && t[j] != '\n')
            j++;
        const char *line = (const char *)t + i;
        if (j == n)
            bad = true;   /* a last line with no end: a write cut short */
        else if (last_line(line, j - i, "index", index) || last_line(line, j - i, "vulns", vulns))
            have = true;
        else
            bad = true;
        i = j + 1;
    }
    free(t);
    if (bad || !have) {
        memset(index, 0, sizeof(*index));
        memset(vulns, 0, sizeof(*vulns));
        return ERR_INVALID_ARGS;
    }
    return OK;
}

status_t pk_last_put(struct pk_env *e, const struct pkg_last *index, const struct pkg_last *vulns)
{
    char path[PK_PATH], fresh[PK_PATH], text[2 * 100], h[65];
    int n = 0;
    if (index->serial) {
        sha256_hex(index->sha256, h);
        n += snprintf(text + n, sizeof(text) - (size_t)n, "index %llu %s\n",
                      (unsigned long long)index->serial, h);
    }
    if (vulns->serial) {
        sha256_hex(vulns->sha256, h);
        n += snprintf(text + n, sizeof(text) - (size_t)n, "vulns %llu %s\n",
                      (unsigned long long)vulns->serial, h);
    }
    pk_db_path(e, "serial", "", path);
    pk_db_path(e, "serial", ".new", fresh);
    status_t st = pk_mkdirs(e, path);
    if (st == OK)
        st = pk_write(e, fresh, text, (size_t)n);
    return st == OK ? pk_move(e, fresh, path) : st;
}

/* ---- the installed set ------------------------------------------------------------------ */

static void copy_str(char *out, struct pkg_str s)
{
    uint32_t n = s.n < PK_NAME - 1 ? s.n : PK_NAME - 1;
    memcpy(out, s.p, n);
    out[n] = '\0';
}

static bool is_db_file(const char *name)
{
    struct pkg_str s = { name, (uint32_t)strlen(name) };
    return !pk_name_reserved(s);
}

/* The summary of one db file into *in (its name is the file's, folded to
 * the manifest's once it reads). A manifest that can't be read is kept as
 * broken, with why. */
static status_t summarise(struct pk_env *e, const char *file, struct pkg_manifest *scratch,
                          struct pk_inst *in)
{
    uint8_t *text;
    uint64_t len;
    memset(in, 0, sizeof(*in));
    snprintf(in->name, sizeof(in->name), "%s", file);
    status_t st = pk_db_load(e, file, "", false, &text, &len, scratch);
    if (st == ERR_NO_MEMORY)
        return st;
    if (st != OK) {
        in->broken = true;
        in->why = st == ERR_OUT_OF_RANGE ? "too big" : st == ERR_NOT_SUPPORTED ?
                  "a format this pkg doesn't know" : "not a manifest";
        return OK;
    }
    sha256(text, len, in->msha);
    free(text);
    if (!pkg_path_fold_equal(scratch->name, (struct pkg_str){ file, (uint32_t)strlen(file) })) {
        in->broken = true;
        in->why = "its manifest names another package";
        return OK;
    }
    copy_str(in->name, scratch->name);
    copy_str(in->version, scratch->version);
    in->revision = scratch->revision;
    in->has_jsha = pk_sum_get(e, in->name, in->msha, in->jsha);
    in->runs = calloc(scratch->nruns ? scratch->nruns : 1, sizeof(*in->runs));
    if (!in->runs)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < scratch->nruns; i++) {
        copy_str(in->runs[i].name, scratch->runs[i].name);
        copy_str(in->runs[i].least, scratch->runs[i].least);
    }
    in->nruns = scratch->nruns;
    return OK;
}

static void sort_set(struct pk_dbset *s)
{
    for (unsigned i = 1; i < s->n; i++) {   /* insertion: a few dozen entries */
        struct pk_inst t = s->v[i];
        unsigned j = i;
        for (; j > 0 && strcmp(s->v[j - 1].name, t.name) > 0; j--)
            s->v[j] = s->v[j - 1];
        s->v[j] = t;
    }
}

status_t pk_db_scan(struct pk_env *e, struct pk_dbset *s)
{
    char dir[PK_PATH];
    struct fs_entry ent;
    unsigned count = 0, cap = 0;
    memset(s, 0, sizeof(*s));
    snprintf(dir, sizeof(dir), "%s/db", e->prefix);
    while (fs_readdir(dir, count, &ent) == OK && count < PKG_PACKAGES_MAX)
        count++;
    if (!count)
        return OK;
    s->v = calloc(count, sizeof(*s->v));
    struct pkg_manifest *scratch = calloc(1, sizeof(*scratch));
    if (!s->v || !scratch) {
        free(s->v);
        free(scratch);
        memset(s, 0, sizeof(*s));
        return ERR_NO_MEMORY;
    }
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < count; i++) {
        if (fs_readdir(dir, i, &ent) != OK)
            break;
        if (!ent.is_dir && is_db_file(ent.name) && cap < count)
            st = summarise(e, ent.name, scratch, &s->v[cap++]);
    }
    free(scratch);
    s->n = cap;
    if (st != OK)
        pk_db_free(s);
    else
        sort_set(s);
    return st;
}

void pk_db_free(struct pk_dbset *s)
{
    for (unsigned i = 0; i < s->n; i++)
        free(s->v[i].runs);
    free(s->v);
    memset(s, 0, sizeof(*s));
}

const struct pk_inst *pk_db_find(const struct pk_dbset *s, struct pkg_str name)
{
    for (unsigned i = 0; i < s->n; i++)
        if (name.n == strlen(s->v[i].name) && !memcmp(name.p, s->v[i].name, name.n))
            return &s->v[i];
    return NULL;
}

status_t pk_db_load(struct pk_env *e, const char *name, const char *suffix, bool check,
                    uint8_t **text, uint64_t *len, struct pkg_manifest *m)
{
    char path[PK_PATH];
    uint8_t *t;
    uint64_t n;
    pk_db_path(e, name, suffix, path);
    status_t st = pk_read(path, PKG_MANIFEST_MAX, &t, &n);
    if (st != OK)
        return st;
    st = check ? pkg_manifest_parse(t, n, e->key, m, NULL)
               : pkg_manifest_parse_unchecked(t, n, m, NULL);
    if (st != OK) {
        free(t);
        return st;
    }
    *text = t;
    *len = n;
    return OK;
}

/* ---- who has which path ---------------------------------------------------------------- */

/* A path's hash, as FAT sees names: ASCII letters in either case alike. */
static uint64_t fold_hash(struct pkg_str p)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < p.n; i++) {
        uint8_t c = (uint8_t)p.p[i];
        h = (h ^ (c >= 'A' && c <= 'Z' ? c + 32u : c)) * 1099511628211ull;
    }
    return h;
}

static void sort_hashes(uint64_t *v, uint32_t n)
{
    for (uint32_t gap = n / 2; gap > 0; gap /= 2)   /* Shell sort: n is at most 4,096 */
        for (uint32_t i = gap; i < n; i++) {
            uint64_t t = v[i];
            uint32_t j = i;
            for (; j >= gap && v[j - gap] > t; j -= gap)
                v[j] = v[j - gap];
            v[j] = t;
        }
}

status_t pk_pathset_make(const struct pkg_manifest *m, struct pk_pathset *s)
{
    s->n = m->nfiles;
    s->m = m;
    s->h = malloc((m->nfiles ? m->nfiles : 1) * sizeof(*s->h));
    if (!s->h)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < m->nfiles; i++)
        s->h[i] = fold_hash(m->files[i].path);
    sort_hashes(s->h, s->n);
    return OK;
}

void pk_pathset_free(struct pk_pathset *s)
{
    free(s->h);
    memset(s, 0, sizeof(*s));
}

const struct pkg_file *pk_pathset_hit(const struct pk_pathset *s, const struct pkg_manifest *o)
{
    for (uint32_t i = 0; i < o->nfiles; i++) {
        uint64_t h = fold_hash(o->files[i].path);
        uint32_t lo = 0, hi = s->n;
        while (lo < hi) {   /* binary search */
            uint32_t mid = (lo + hi) / 2;
            if (s->h[mid] < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == s->n || s->h[lo] != h)
            continue;
        for (uint32_t k = 0; k < s->m->nfiles; k++)   /* the hash agrees: the names too? */
            if (pkg_path_fold_equal(s->m->files[k].path, o->files[i].path))
                return &o->files[i];
    }
    return NULL;
}

status_t pk_db_conflict(struct pk_env *e, const struct pk_dbset *db, const struct pkg_manifest *nm,
                        char owner[PK_NAME], char path[PK_PATH])
{
    struct pk_pathset s;
    struct pkg_manifest *om = calloc(1, sizeof(*om));
    status_t st = om ? pk_pathset_make(nm, &s) : ERR_NO_MEMORY;
    for (unsigned i = 0; st == OK && i < db->n; i++) {
        uint8_t *text;
        uint64_t len;
        if (db->v[i].broken || pkg_str_eq(nm->name, db->v[i].name))
            continue;   /* the package itself, being replaced, owns its files */
        if (pk_db_load(e, db->v[i].name, "", false, &text, &len, om) != OK)
            continue;
        const struct pkg_file *hit = pk_pathset_hit(&s, om);
        if (hit) {
            snprintf(owner, PK_NAME, "%s", db->v[i].name);
            snprintf(path, PK_PATH, "%.*s", (int)hit->path.n, hit->path.p);
            st = ERR_ALREADY_EXISTS;
        }
        free(text);
    }
    if (om && st != ERR_NO_MEMORY)
        pk_pathset_free(&s);
    free(om);
    return st;
}
