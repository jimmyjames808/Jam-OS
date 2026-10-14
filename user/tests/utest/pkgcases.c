/* The ports formats against ports/vectors (CASES): each vector read by
 * the C readers must get the answer tools/portslib/vectors.py recorded
 * from its Python reference. A package is read twice, whole in memory and
 * as a stream of chunks of random sizes, and the two must agree. Shared
 * by utest and the Mac's tools/pkgcheck.c (see pkgcases.h). */
#include <os.h>
#include <pkgfmt.h>
#include "pkgcases.h"
#include "pkgcases_int.h"

const struct pkgv_file *pkgv_find(const char *name)
{
    for (unsigned i = 0; i < pkgv_nfiles; i++)
        if (!strcmp(pkgv_files[i].name, name))
            return &pkgv_files[i];
    return NULL;
}

/* ---- the work areas and the key ------------------------------------------------------------ */

static struct pkgc_work w;

struct pkgc_work *pkgc_work(void)
{
    if (!w.m) {
        w.m = malloc(sizeof(*w.m));
        w.x = malloc(sizeof(*w.x));
        w.v = malloc(sizeof(*w.v));
        w.j = malloc(sizeof(*w.j));
        const struct pkgv_file *pub = pkgv_find("test.pub");
        if (!w.m || !w.x || !w.v || !w.j || !pub || pub->len != 73 ||
            memcmp(pub->data, "ed25519 ", 8) ||
            !pkg_hex((struct pkg_str){ (const char *)pub->data + 8, 64 }, w.key, 32))
            pkgc_work_free();
    }
    return w.m ? &w : NULL;
}

void pkgc_work_free(void)
{
    free(w.m);
    free(w.x);
    free(w.v);
    free(w.j);
    memset(&w, 0, sizeof(w));
}

uint64_t pkgc_rand(uint64_t *state)
{
    uint64_t x = *state ? *state : 0x9e3779b97f4a7c15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* ---- the stream reader over a whole archive -------------------------------------------------- */

static bool zero(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++)
            return false;
    return true;
}

/* Feed member bytes in chunks of 1..1500 bytes. */
static status_t feed(struct pkg_reader *r, const uint8_t *p, uint64_t n, uint64_t *rng)
{
    while (n) {
        uint64_t take = 1 + pkgc_rand(rng) % 1500;
        take = take < n ? take : n;
        status_t st = pkg_reader_data(r, p, (size_t)take);
        if (st != OK)
            return st;
        p += take;
        n -= take;
    }
    return OK;
}

/* Each program's ELF note must be its list: the installer's check, which
 * the reader leaves to whoever has the program's bytes. */
static status_t stream_lists(const struct pkg_manifest *m, const uint8_t *d, const size_t *at)
{
    for (unsigned i = 0; i < m->nprograms; i++) {
        uint8_t list[PKG_LIST_MAX];
        size_t n;
        if (pkg_list_decode(m->programs[i].list, list, &n) != OK ||
            !pkg_elf_list_is(d + at[m->programs[i].file], m->files[m->programs[i].file].size, list, n))
            return ERR_ACCESS_DENIED;
    }
    return OK;
}

status_t pkgc_stream(const uint8_t *d, size_t len, const char *abi, struct pkg_manifest *m,
                     uint64_t *rng)
{
    struct pkgc_work *k = pkgc_work();
    static size_t at_file[PKG_FILES_MAX];
    uint32_t n;
    if (!k || len < PKG_BLOCK)
        return ERR_INVALID_ARGS;
    status_t st = pkg_first_header(d, &n);
    if (st != OK || len - PKG_BLOCK < n)
        return ERR_INVALID_ARGS;
    if ((st = pkg_manifest_parse(d + PKG_BLOCK, n, k->key, m, NULL)) != OK)
        return st;
    if (abi && !pkg_abi_is(m, abi))
        return ERR_NOT_SUPPORTED;
    size_t at = PKG_BLOCK + n, pad = (PKG_BLOCK - n % PKG_BLOCK) % PKG_BLOCK;
    if (len - at < pad || !zero(d + at, pad))
        return ERR_INVALID_ARGS;
    at += pad;
    uint64_t tb = pkg_tables_bytes(m);
    uint8_t *tables = malloc(tb ? tb : 1);
    struct pkg_reader r;
    if (!tables || (st = pkg_reader_init(&r, m, tables, tb)) != OK) {
        free(tables);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < m->nfiles + m->nprograms && st == OK; i++) {
        struct pkg_member mem;
        if (len - at < PKG_BLOCK || (st = pkg_reader_next(&r, d + at, &mem)) != OK) {
            st = st == OK ? ERR_INVALID_ARGS : st;
            break;
        }
        at += PKG_BLOCK;
        if (len - at < mem.size || len - at - mem.size < mem.padding) {
            st = ERR_INVALID_ARGS;
            break;
        }
        if (i < m->nfiles)
            at_file[i] = at;
        if ((st = feed(&r, d + at, mem.size, rng)) == OK)
            st = pkg_reader_done(&r, d + at + mem.size, mem.padding);
        at += mem.size + mem.padding;
    }
    if (st == OK)
        st = pkg_reader_finish(&r, d + at, len - at > PKG_END_BYTES + 1 ? PKG_END_BYTES + 1 : len - at);
    if (st == OK)
        st = stream_lists(m, d, at_file);
    free(tables);
    return st;
}

/* ---- what an accepted structure must satisfy ---------------------------------------------------- */

static bool str_ok_order(struct pkg_str a, struct pkg_str b)
{
    return pkg_str_cmp(a, b) < 0;
}

bool pkgc_manifest_ok(const struct pkg_manifest *m)
{
    if (m->nfiles < 1 || m->nfiles > PKG_FILES_MAX || m->nprograms > PKG_PROGRAMS_MAX ||
        m->nruns > PKG_DEPS_MAX || m->nbuilt > PKG_DEPS_MAX || m->total > PKG_FILES_TOTAL_MAX)
        return false;
    uint64_t total = 0;
    unsigned executable = 0;
    for (unsigned i = 0; i < m->nfiles; i++) {
        const struct pkg_file *f = &m->files[i];
        total += f->size;
        executable += f->mode == 0755;
        if (pkg_path_problem(f->path) || (f->mode != 0644 && f->mode != 0755) ||
            f->size > PKG_FILE_MAX || (i && !str_ok_order(m->files[i - 1].path, f->path)))
            return false;
        if (f->program != PKG_NO_PROGRAM &&
            (f->program >= m->nprograms || m->programs[f->program].file != i || f->mode != 0755))
            return false;
        if (f->program == PKG_NO_PROGRAM && f->mode == 0755)
            return false;
        for (unsigned k = 0; k < i; k++)
            if (pkg_path_fold_equal(m->files[k].path, f->path))
                return false;
    }
    if (total != m->total || executable != m->nprograms)
        return false;
    for (unsigned i = 0; i < m->nprograms; i++) {
        const struct pkg_program *p = &m->programs[i];
        uint8_t list[PKG_LIST_MAX];
        size_t n;
        if (p->file >= m->nfiles || m->files[p->file].size == 0 ||
            p->table_size != pkg_table_size(m->files[p->file].size) ||
            pkg_str_cmp(p->path, m->files[p->file].path) || (i && !str_ok_order(m->programs[i - 1].path, p->path)) ||
            pkg_list_decode(p->list, list, &n) != OK || pkg_list_refused(list, n))
            return false;
    }
    for (unsigned i = 1; i < m->nruns; i++)
        if (!str_ok_order(m->runs[i - 1].name, m->runs[i].name))
            return false;
    for (unsigned i = 1; i < m->nbuilt; i++)
        if (!str_ok_order(m->built_with[i - 1].name, m->built_with[i].name))
            return false;
    return true;
}

bool pkgc_index_ok(const struct pkg_index *x)
{
    if (x->npackages > PKG_PACKAGES_MAX || x->nsources > PKG_PACKAGES_MAX)
        return false;
    for (unsigned i = 0; i < x->npackages; i++) {
        const struct pkg_ipkg *p = &x->packages[i];
        struct pkg_dep deps[PKG_DEPS_MAX];
        if ((i && !str_ok_order(x->packages[i - 1].name, p->name)) || p->nruns > PKG_DEPS_MAX ||
            p->size < 3 * PKG_BLOCK || p->size > PKG_ARCHIVE_MAX || p->source >= x->nsources)
            return false;
        if (pkg_index_runs(x, p, deps) != p->nruns)
            return false;
        for (unsigned k = 0; k < p->nruns; k++) {
            const struct pkg_ipkg *d = pkg_index_find(x, deps[k].name);
            if (!d || (k && !str_ok_order(deps[k - 1].name, deps[k].name)) ||
                (deps[k].least.n && pkg_version_cmp(d->version, deps[k].least) < 0))
                return false;
        }
    }
    for (unsigned i = 0; i < x->nsources; i++)
        if (!x->sources[i].used || (i && !str_ok_order(x->sources[i - 1].port, x->sources[i].port)))
            return false;
    return true;
}

bool pkgc_vulns_ok(const struct pkg_vulns *v)
{
    if (v->n > PKG_VULNS_ENTRIES_MAX)
        return false;
    for (unsigned i = 1; i < v->n; i++) {
        int c = pkg_str_cmp(v->v[i - 1].component, v->v[i].component);
        if (c > 0 || (c == 0 && !str_ok_order(v->v[i - 1].id, v->v[i].id)))
            return false;
    }
    return true;
}

/* ---- the verdicts --------------------------------------------------------------------------- */

#define VEC_ABI "0123456789abcdef"

int pkgc_package_verdict(const uint8_t *data, size_t len)
{
    struct pkgc_work *k = pkgc_work();
    uint64_t rng = len * 2654435761u + 1;
    if (!k)
        return -1;
    bool whole = pkg_package_verify(data, len, k->key, VEC_ABI, k->m, NULL) == OK;
    bool whole_ok = whole && pkgc_manifest_ok(k->m);
    bool stream = pkgc_stream(data, len, VEC_ABI, k->m, &rng) == OK;
    if (whole != stream || (whole && !whole_ok))
        return -1;
    return whole;
}

int pkgc_text_verdict(const char *kind, const uint8_t *text, size_t len)
{
    struct pkgc_work *k = pkgc_work();
    if (!k)
        return -1;
    if (!strcmp(kind, "manifest"))
        return pkg_manifest_parse_unchecked(text, len, k->m, NULL) == OK;
    if (!strcmp(kind, "index"))
        return pkg_index_parse_unchecked(text, len, k->x, NULL) == OK;
    if (!strcmp(kind, "vulns"))
        return pkg_vulns_parse_unchecked(text, len, k->v, NULL) == OK;
    return -1;
}

int pkgc_version_cmp(const char *a, const char *b)
{
    return pkg_version_cmp((struct pkg_str){ a, (uint32_t)strlen(a) },
                           (struct pkg_str){ b, (uint32_t)strlen(b) });
}

int pkgc_path_ok(const char *path)
{
    return pkg_path_problem((struct pkg_str){ path, (uint32_t)strlen(path) }) == NULL;
}

/* ---- CASES ------------------------------------------------------------------------------------ */

/* A case line's `key=value` option, copied into out; false if absent. */
static bool option(const char *line, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = line; *p; p++) {
        if ((p == line || p[-1] == ' ') && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = 0;
            for (p += kl + 1; *p && *p != ' ' && n + 1 < cap; p++)
                out[n++] = *p;
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

/* last=<serial>[:<sha256>] */
static const struct pkg_last *last_option(const char *line, struct pkg_last *last)
{
    char v[96];
    if (!option(line, "last", v, sizeof(v)))
        return NULL;
    memset(last, 0, sizeof(*last));
    const char *p = v;
    for (; *p && *p != ':'; p++)
        last->serial = last->serial * 10 + (uint64_t)(*p - '0');
    if (*p == ':')
        pkg_hex((struct pkg_str){ p + 1, (uint32_t)strlen(p + 1) }, last->sha256, 32);
    return last;
}

static bool audit_ids(const struct pkg_vulns *v, const struct pkg_manifest *m, char *out, size_t cap)
{
    const struct pkg_vuln *hit[64];
    unsigned n = pkg_audit(v, m, hit, 64);
    if (n > 64)
        return false;
    /* sorted by id, as Python's sorted() */
    for (unsigned i = 1; i < n; i++)
        for (unsigned k = i; k && pkg_str_cmp(hit[k]->id, hit[k - 1]->id) < 0; k--) {
            const struct pkg_vuln *t = hit[k];
            hit[k] = hit[k - 1];
            hit[k - 1] = t;
        }
    size_t at = 0;
    if (n == 0) {
        memcpy(out, "-", 2);
        return true;
    }
    for (unsigned i = 0; i < n; i++) {
        if (at + hit[i]->id.n + 2 > cap)
            return false;
        if (i)
            out[at++] = ',';
        memcpy(out + at, hit[i]->id.p, hit[i]->id.n);
        at += hit[i]->id.n;
    }
    out[at] = '\0';
    return true;
}

/* The first three words of a CASES line. */
static bool case_words(const char *line, char *kind, char *name, char *expect)
{
    char *out[3] = { kind, name, expect };
    const char *p = line;
    for (unsigned i = 0; i < 3; i++) {
        size_t n = 0;
        while (*p == ' ')
            p++;
        while (*p && *p != ' ' && n < 63)
            out[i][n++] = *p++;
        out[i][n] = '\0';
        if (!n)
            return false;
    }
    return true;
}

/* Why the last reader refused, for the failure message. */
static const char *why;

/* The C answer to one CASES line: 1 accepted, 0 refused, -1 a mismatch
 * inside the C readers (or a broken case). */
static int run_case(const char *line)
{
    why = "";
    char kind[64], name[64], expect[64], opt[96];
    struct pkgc_work *k = pkgc_work();
    struct pkg_last last_buf;
    if (!k || !case_words(line, kind, name, expect))
        return -1;
    const struct pkgv_file *f = pkgv_find(name);
    if (!f)
        return -1;
    const struct pkg_last *last = last_option(line, &last_buf);
    if (!strcmp(kind, "package")) {
        char abi[24];
        const char *want = option(line, "abi", abi, sizeof(abi)) ? abi : NULL;
        uint64_t rng = 7;
        bool whole = pkg_package_verify(f->data, f->len, k->key, want, k->m, &why) == OK;
        bool stream = pkgc_stream(f->data, f->len, want, k->m, &rng) == OK;
        return whole != stream ? -1 : whole;
    }
    if (!strcmp(kind, "index"))
        return pkg_index_parse(f->data, f->len, k->key, last, k->x, &why) == OK;
    if (!strcmp(kind, "vulns"))
        return pkg_vulns_parse(f->data, f->len, k->key, last, k->v, &why) == OK;
    if (!strcmp(kind, "inindex")) {
        const struct pkgv_file *p = option(line, "pkg", opt, sizeof(opt)) ? pkgv_find(opt) : NULL;
        if (!p || !option(line, "name", opt, sizeof(opt)) ||
            pkg_index_parse(f->data, f->len, k->key, NULL, k->x, NULL) != OK)
            return -1;
        const struct pkg_ipkg *e = pkg_index_find(k->x, (struct pkg_str){ opt, (uint32_t)strlen(opt) });
        uint8_t sum[32];
        sha256(p->data, p->len, sum);
        return e && pkg_index_check(e, p->len, sum) == OK;
    }
    if (!strcmp(kind, "audit")) {
        char ids[128];
        const struct pkgv_file *p = option(line, "pkg", opt, sizeof(opt)) ? pkgv_find(opt) : NULL;
        if (!p || pkg_vulns_parse(f->data, f->len, k->key, NULL, k->v, NULL) != OK ||
            pkg_package_verify(p->data, p->len, k->key, NULL, k->m, NULL) != OK ||
            !audit_ids(k->v, k->m, ids, sizeof(ids)) || !option(line, "ids", opt, sizeof(opt)))
            return -1;
        return !strcmp(ids, opt);
    }
    return -1;
}

unsigned pkgc_vectors(void)
{
    const struct pkgv_file *cases = pkgv_find("CASES");
    unsigned fails = 0, n = 0;
    char line[256];
    if (!cases || !pkgc_work()) {
        printf("pkgfmt: no vectors (CASES, test.pub)\n");
        return 1;
    }
    for (size_t at = 0; at < cases->len;) {
        size_t l = 0;
        while (at + l < cases->len && cases->data[at + l] != '\n')
            l++;
        if (l && cases->data[at] != '#' && l < sizeof(line)) {
            memcpy(line, cases->data + at, l);
            line[l] = '\0';
            char kind[64], name[64], expect[64];
            int want = case_words(line, kind, name, expect) && !strcmp(expect, "ok");
            int got = run_case(line);
            n++;
            if (got != want) {
                printf("pkgfmt: vector: %s: the C reader says %s (%s)\n", line,
                       got < 0 ? "something inconsistent" : got ? "ok" : "refused",
                       why ? why : "");
                fails++;
            }
        }
        at += l + 1;
    }
    if (n < 40) {
        printf("pkgfmt: only %u cases\n", n);
        fails++;
    }
    return fails;
}
