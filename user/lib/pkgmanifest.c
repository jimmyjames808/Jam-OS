/* A package's manifest (<pkgfmt.h>, the plan's section 6): `jamos-pkg 1`,
 * its lines in one fixed order, read after its signature has been
 * checked. Every line is matched against its grammar in its place and
 * every count, size and order is bounded; the paths must be ones FAT32
 * can hold side by side; each program is a non-empty file of mode 755
 * (and every such file a program) with a list a package may carry and a
 * table line of the right size. Nothing is repaired: one line out of
 * place refuses the manifest. */
#include "pkgint.h"

static const char *const known_keys[] = {
    "runs", "built-with", "file", "program", "table", "signature", "name", "version", "revision",
    "abi", "licence", "summary", NULL,
};

/* The next line must be `key <one field>`. */
static bool one_field(struct pkg_lines *ls, const char *key, struct pkg_str *f)
{
    struct pkg_str line, w[1];
    return pkg_next_line(ls, &line) && pkg_words(line, key, w, 1, 1, NULL) && ((*f = w[0]), true);
}

static status_t header_lines(struct pkg_lines *ls, struct pkg_manifest *m, const char **why)
{
    struct pkg_str f, line;
    NEED(one_field(ls, "name", &m->name) && pkg_f_name(m->name), "the name line");
    NEED(one_field(ls, "version", &m->version) && pkg_f_version(m->version), "the version line");
    NEED(one_field(ls, "revision", &f) && pkg_f_revision(f, &m->revision), "the revision line");
    NEED(one_field(ls, "abi", &f) && pkg_f_abi(f, m->abi), "the abi line");
    NEED(pkg_next_line(ls, &line) && pkg_rest(line, "licence", &m->licence) &&
             pkg_f_licence(m->licence), "the licence line");
    NEED(pkg_next_line(ls, &line) && pkg_rest(line, "summary", &m->summary) &&
             pkg_f_text(m->summary), "the summary line");
    return pkg_extensions(ls, known_keys, why);
}

static status_t dep_lines(struct pkg_lines *ls, struct pkg_manifest *m, const char **why)
{
    struct pkg_str line, w[3];
    unsigned n;
    m->nruns = m->nbuilt = 0;
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "runs")) {
        pkg_next_line(ls, &line);
        NEED(pkg_words(line, "runs", w, 1, 2, &n) && pkg_f_name(w[0]) &&
                 (n == 1 || pkg_f_version(w[1])), "a runs line");
        NEED(m->nruns < PKG_DEPS_MAX, "too many runs lines");
        NEED(m->nruns == 0 || pkg_increasing(m->runs[m->nruns - 1].name, w[0]),
             "runs lines not in strictly increasing order");
        NEED(pkg_str_cmp(w[0], m->name), "a package that runs itself");
        m->runs[m->nruns].name = w[0];
        m->runs[m->nruns].least = n == 2 ? w[1] : (struct pkg_str){ NULL, 0 };
        m->nruns++;
    }
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "built-with")) {
        pkg_next_line(ls, &line);
        NEED(m->nbuilt < PKG_DEPS_MAX, "too many built-with lines");   /* before b is written */
        struct pkg_built *b = &m->built_with[m->nbuilt];
        NEED(pkg_words(line, "built-with", w, 2, 3, &n) && pkg_f_name(w[0]) &&
                 pkg_f_version(w[1]) && (n == 2 || pkg_f_revision(w[2], &b->revision)),
             "a built-with line");
        NEED(m->nbuilt == 0 || pkg_increasing(m->built_with[m->nbuilt - 1].name, w[0]),
             "built-with lines not in strictly increasing order");
        b->name = w[0];
        b->version = w[1];
        b->revision = n == 3 ? b->revision : 0;
        m->nbuilt++;
    }
    return OK;
}

/* Files by FAT32's name order: ASCII letters folded, then bytewise. */
static int fold_cmp(struct pkg_str a, struct pkg_str b)
{
    uint32_t n = a.n < b.n ? a.n : b.n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)a.p[i], y = (uint8_t)b.p[i];
        x = x >= 'A' && x <= 'Z' ? (uint8_t)(x + 32) : x;
        y = y >= 'A' && y <= 'Z' ? (uint8_t)(y + 32) : y;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

/* Sort idx[0..n) by the files' folded paths (Shell sort: no recursion). */
static void sort_folded(const struct pkg_manifest *m, uint16_t *idx, unsigned n)
{
    for (unsigned gap = n / 2; gap; gap /= 2)
        for (unsigned i = gap; i < n; i++) {
            uint16_t v = idx[i];
            unsigned j = i;
            while (j >= gap && fold_cmp(m->files[idx[j - gap]].path, m->files[v].path) > 0) {
                idx[j] = idx[j - gap];
                j -= gap;
            }
            idx[j] = v;
        }
}

static bool has_folded(const struct pkg_manifest *m, const uint16_t *idx, struct pkg_str p)
{
    unsigned lo = 0, hi = m->nfiles;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = fold_cmp(m->files[idx[mid]].path, p);
        if (!c)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* FAT32 keeps one file per name whatever the case, and no file where
 * another path needs a directory. */
static status_t clash_check(struct pkg_manifest *m, const char **why)
{
    uint16_t idx[PKG_FILES_MAX];   /* 8 KB */
    for (unsigned i = 0; i < m->nfiles; i++)
        idx[i] = (uint16_t)i;
    sort_folded(m, idx, m->nfiles);
    for (unsigned i = 1; i < m->nfiles; i++)
        NEED(fold_cmp(m->files[idx[i - 1]].path, m->files[idx[i]].path),
             "twice, or differs from another path only in case");
    for (unsigned i = 0; i < m->nfiles; i++) {
        struct pkg_str p = m->files[i].path;
        for (uint32_t k = 1; k < p.n; k++)
            NEED(p.p[k] != '/' || !has_folded(m, idx, (struct pkg_str){ p.p, k }),
                 "a file is named where another path needs a directory");
    }
    return OK;
}

static status_t file_lines(struct pkg_lines *ls, struct pkg_manifest *m, const char **why)
{
    struct pkg_str line, w[4];
    m->nfiles = 0;
    m->total = 0;
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "file")) {
        struct pkg_file *f = &m->files[m->nfiles];
        uint64_t size;
        pkg_next_line(ls, &line);
        NEED(m->nfiles < PKG_FILES_MAX, "too many file lines");
        NEED(pkg_words(line, "file", w, 4, 4, NULL), "a file line");
        NEED(!pkg_path_problem(w[0]), "a file line's path");
        NEED(pkg_str_eq(w[1], "644") || pkg_str_eq(w[1], "755"), "a file line's mode");
        NEED(pkg_f_size(w[2], PKG_FILE_MAX, &size) && pkg_f_sha(w[3], f->sha256),
             "a file line's size or SHA-256");
        NEED(m->nfiles == 0 || pkg_increasing(m->files[m->nfiles - 1].path, w[0]),
             "file lines not in strictly increasing order");
        f->path = w[0];
        f->mode = pkg_str_eq(w[1], "755") ? 0755 : 0644;
        f->program = PKG_NO_PROGRAM;
        f->size = size;
        m->total += size;
        NEED(m->total <= PKG_FILES_TOTAL_MAX, "the files are over the package's total");
        m->nfiles++;
    }
    NEED(m->nfiles > 0, "no file lines");
    return clash_check(m, why);
}

/* The index of the file called path (files are strictly increasing), or -1. */
static int find_file(const struct pkg_manifest *m, struct pkg_str path)
{
    unsigned lo = 0, hi = m->nfiles;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = pkg_str_cmp(m->files[mid].path, path);
        if (!c)
            return (int)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static status_t program_line(struct pkg_manifest *m, struct pkg_str line, const char **why)
{
    struct pkg_str w[2];
    uint8_t list[PKG_LIST_MAX];
    size_t n;
    struct pkg_program *p = &m->programs[m->nprograms];
    NEED(m->nprograms < PKG_PROGRAMS_MAX, "too many program lines");
    NEED(pkg_split_rest(line, "program", w, 2), "a program line");
    NEED(pkg_list_decode(w[1], list, &n) == OK, "a program's list");
    if (pkg_list_refused(list, n))
        return pkg_fail(why, ERR_ACCESS_DENIED, "a program asks for what a package may not have");
    NEED(m->nprograms == 0 || pkg_increasing(m->programs[m->nprograms - 1].path, w[0]),
         "program lines not in strictly increasing order");
    int fi = find_file(m, w[0]);
    NEED(fi >= 0 && m->files[fi].mode == 0755 && m->files[fi].size > 0,
         "a program that is not a non-empty file of mode 755");
    p->path = w[0];
    p->list = w[1];
    p->file = (uint32_t)fi;
    m->files[fi].program = (uint16_t)m->nprograms;
    m->nprograms++;
    return OK;
}

static status_t table_line(struct pkg_manifest *m, unsigned i, struct pkg_str line,
                           const char **why)
{
    struct pkg_program *p = &m->programs[i];
    struct pkg_str w[3];
    NEED(pkg_words(line, "table", w, 3, 3, NULL) && !pkg_str_cmp(w[0], p->path),
         "a table line, or not in the programs' order");
    NEED(pkg_f_size(w[1], pkg_table_size(PKG_FILE_MAX), &p->table_size) &&
             p->table_size == pkg_table_size(m->files[p->file].size),
         "a table line's size is not its file's table's");
    NEED(pkg_f_sha(w[2], p->table_sha256), "a table line's SHA-256");
    return OK;
}

static status_t program_lines(struct pkg_lines *ls, struct pkg_manifest *m, const char **why)
{
    struct pkg_str line;
    unsigned executable = 0;
    status_t st;
    m->nprograms = 0;
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "program")) {
        pkg_next_line(ls, &line);
        if ((st = program_line(m, line, why)) != OK)
            return st;
    }
    for (unsigned i = 0; i < m->nfiles; i++)
        executable += m->files[i].mode == 0755;
    NEED(executable == m->nprograms, "a file of mode 755 with no program line");
    for (unsigned i = 0; i < m->nprograms; i++) {
        NEED(pkg_next_line(ls, &line), "not one table line per program");
        if ((st = table_line(m, i, line, why)) != OK)
            return st;
    }
    return OK;
}

static status_t manifest_body(struct pkg_lines *ls, struct pkg_manifest *m, const char **why)
{
    struct pkg_str line;
    status_t st = header_lines(ls, m, why);
    if (st == OK)
        st = dep_lines(ls, m, why);
    if (st == OK)
        st = file_lines(ls, m, why);
    if (st == OK)
        st = program_lines(ls, m, why);
    if (st != OK)
        return st;
    NEED(!pkg_peek_line(ls, &line), "a line out of place");
    return OK;
}

status_t pkg_manifest_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                            struct pkg_manifest *m, const char **why)
{
    struct pkg_lines ls;
    if (!key)
        return pkg_fail(why, ERR_INVALID_ARGS, "no key");
    status_t st = pkg_signed_text(text, len, PKG_FIRST_PACKAGE, PKG_MANIFEST_MAX, key, &ls,
                                  m->sha256, why);
    return st == OK ? manifest_body(&ls, m, why) : st;
}

status_t pkg_manifest_parse_unchecked(const void *text, size_t len, struct pkg_manifest *m,
                                      const char **why)
{
    struct pkg_lines ls;
    status_t st = pkg_signed_text(text, len, PKG_FIRST_PACKAGE, PKG_MANIFEST_MAX, NULL, &ls,
                                  m->sha256, why);
    return st == OK ? manifest_body(&ls, m, why) : st;
}
