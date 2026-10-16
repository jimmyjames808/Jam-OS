/* init's approvals of a package's programs (pkgapprove.h; the protocol is
 * abi/idl/pkgctl.idl, the design docs/M13.7-PLAN.md section 9).
 *
 * What decides that a package's program may run is here, so it trusts
 * nothing bin/pkg says: only the manifest's signature by the ports key,
 * and the bytes it reads back itself. Each program is read whole into a
 * VMO only init holds (file_read_vmo), and every check is of those bytes:
 * the size and SHA-256 the manifest's `file` line names, the page table
 * made from them (pkg_tabler) against the SHA-256 the `table` line names
 * (the table bin/pkg shipped is never read: the one written is init's
 * own), the program's own list (its ELF note, wants_read) against the
 * manifest's `program` list byte for byte (pkg_elf_list_is), and what no
 * program from /data may have (dp_wants_refused, `allow`'s refusals). The
 * file at the path can change afterwards: the line approves the hash, so
 * only these bytes run.
 *
 * All or nothing: every program is checked before anything is written;
 * then the tables (inert without a line naming their hash), then the
 * approvals' file once, whole. A line is
 *
 *     <sha-256>\t<prefix>/<path>\t<its list as shown>\tpkg=<name> <version>-r<revision>
 *
 * (the shell's `allow -l` shows the tag; `allow -r` refuses it). A path
 * that another package's line holds is refused (two packages can't own
 * one program); the same line twice is written once. The owner's own
 * lines (no tag) are left as they are.
 *
 * The approvals' file is written whole (FAT32 has no rename over a
 * file), as the shell's `allow` writes it. The shell and init don't lock
 * it against each other: an `allow` typed while a package installs can
 * lose the other's line (the program is then refused, never wrongly
 * allowed, except that an `allow` racing a revoke can write the revoked
 * line back; see the plan's PO5 notes).
 *
 * The settle (after a cut, at boot) follows the journal of <pkgfmt.h>:
 * the commit point of an install or an upgrade is `approved` (the lines
 * are there: the files are swapped into place, the old version's files
 * and lines go, the manifest is recorded), before it the transaction is
 * undone (the staged files go); a remove goes forward from `begin`. Every
 * step can be done twice, so the journal isn't rewritten as it goes: it
 * is removed at the end, and a cut during the settle settles again. */
#include <stdarg.h>
#include <dataprog.h>
#include <pkgfmt.h>
#include <sha256.h>
#include <wants.h>
#include "pkgapprove.h"

#define PATH_CAP   400   /* a prefix (at most ~100 bytes), '/', a path (255), ".new" */
#define LINE_CAP   (DP_HEX_LEN + 1 + PATH_CAP + 1 + WANTS_TEXT_MAX + 1 + PKGA_TAG_MAX + 2)

/* ---- memory of our own ------------------------------------------------------------- */

/* A VMO mapped into init (a program's bytes, the tables). */
struct mapped {
    handle_t vmo;
    uint64_t len;   /* mapped bytes: whole pages */
    uint8_t *p;
};

static uint64_t pages_of(uint64_t n)
{
    return n ? (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1) : PAGE_SIZE;
}

static void unmap(struct mapped *m)
{
    if (m->p)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)m->p, m->len);
    if (m->vmo)
        jam_handle_close(m->vmo);
    *m = (struct mapped){ 0 };
}

/* vmo (taken) mapped, n bytes of it at least. */
static status_t map_vmo(handle_t vmo, uint64_t n, bool write, struct mapped *m)
{
    uint64_t addr = 0;
    *m = (struct mapped){ .vmo = vmo, .len = pages_of(n) };
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, m->len,
                               VMAR_READ | (write ? VMAR_WRITE : 0), &addr);
    if (st != OK) {
        unmap(m);
        return st;
    }
    m->p = (uint8_t *)(uintptr_t)addr;
    return OK;
}

/* n bytes of new zeroed memory, mapped. */
static status_t map_new(uint64_t n, struct mapped *m)
{
    handle_t v;
    status_t st = jam_vmo_create(pages_of(n), 0, &v);
    return st == OK ? map_vmo(v, n, true, m) : st;
}

/* The file at path (at most max bytes) read whole into memory of our own:
 * *size its bytes. */
static status_t map_file(const char *path, uint64_t max, struct mapped *m, uint64_t *size)
{
    handle_t v;
    status_t st = file_read_vmo(path, max, &v, size);
    return st == OK ? map_vmo(v, *size, false, m) : st;
}

/* ---- files ------------------------------------------------------------------------- */

static status_t write_whole(const char *path, const void *data, size_t n)
{
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    st = file_write(&f, 0, data, n, &done);
    if (st == OK && done != n)
        st = ERR_IO;
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    return st;
}

static status_t mkdir_ok(const char *path)
{
    status_t st = fs_mkdir(path);
    return st == ERR_ALREADY_EXISTS ? OK : st;
}

static status_t unlink_ok(const char *path)
{
    status_t st = fs_unlink(path);
    return st == ERR_NOT_FOUND ? OK : st;
}

/* <prefix>/<p><suffix> into out (PATH_CAP bytes). */
static void under(const struct pkga_place *at, struct pkg_str p, const char *suffix, char *out)
{
    snprintf(out, PATH_CAP, "%s/%.*s%s", at->prefix, (int)p.n, p.p, suffix);
}

/* The approvals' text, NUL-terminated, in a heap copy (*text, the caller
 * frees it; "" if there are none yet). */
static status_t read_allow(const struct pkga_place *at, char **text)
{
    struct mapped m;
    uint64_t size = 0;
    status_t st = map_file(at->allow, DP_ALLOW_MAX, &m, &size);
    if (st == ERR_NOT_FOUND) {
        *text = calloc(1, 1);
        return *text ? OK : ERR_NO_MEMORY;
    }
    if (st != OK)
        return st;
    char *t = malloc(size + 1);
    if (t) {
        memcpy(t, m.p, size);
        t[size] = '\0';
    }
    unmap(&m);
    *text = t;
    return t ? OK : ERR_NO_MEMORY;
}

/* ---- lines ------------------------------------------------------------------------- */

void pkga_tag(struct pkg_str name, struct pkg_str version, uint32_t revision, char *out)
{
    snprintf(out, PKGA_TAG_MAX, "pkg=%.*s %.*s-r%u", (int)name.n, name.p, (int)version.n,
             version.p, revision);
}

/* One line of the approvals (len bytes at s, no '\n') cut into its fields
 * in a copy: false if it is no approval. *tag: without "pkg=", or NULL. */
struct line {
    char  copy[LINE_CAP];
    char *hash, *path, *text, *tag;
};

static bool line_read(const char *s, size_t len, struct line *l)
{
    if (len >= sizeof(l->copy))
        return false;   /* longer than any line written: kept as it is, never judged */
    memcpy(l->copy, s, len);
    l->copy[len] = '\0';
    if (!dp_allow_split(l->copy, &l->hash, &l->path, &l->text))
        return false;
    l->tag = dp_allow_tag(l->text);
    return true;
}

/* Does a tag (without "pkg=") belong to the package called name (its part
 * before the space)? */
static bool tag_names(const char *tag, struct pkg_str name)
{
    return !strncmp(tag, name.p, name.n) && tag[name.n] == ' ';
}

/* Does any line of text start with hex and a tab? */
static bool hash_named(const char *text, const char *hex)
{
    for (const char *s = text; s && *s;) {
        if (!strncmp(s, hex, DP_HEX_LEN) && s[DP_HEX_LEN] == '\t')
            return true;
        s = strchr(s, '\n');
        s = s ? s + 1 : NULL;
    }
    return false;
}

/* The tables of the hashes in gone (n of them) that no line of kept names. */
static void drop_tables(const struct pkga_place *at, const char *gone, unsigned n,
                        const char *kept)
{
    for (unsigned i = 0; i < n; i++) {
        char hex[DP_HEX_LEN + 1], path[PATH_CAP];
        memcpy(hex, gone + (size_t)i * DP_HEX_LEN, DP_HEX_LEN);
        hex[DP_HEX_LEN] = '\0';
        if (hash_named(kept, hex))
            continue;
        snprintf(path, sizeof(path), "%s/%s", at->tables, hex);
        (void)fs_unlink(path);   /* one that won't go stays, unused: no line names it */
    }
}

/* ---- approve ----------------------------------------------------------------------- */

/* What a program that passed leaves for the writing. */
struct passed {
    char     hex[DP_HEX_LEN + 1];
    char     text[WANTS_TEXT_MAX];
    uint64_t table_at, table_len;   /* its table in the tables' memory */
};

static status_t refuse(struct pkga_result *r, status_t st, uint32_t program, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static status_t refuse(struct pkga_result *r, status_t st, uint32_t program, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why, sizeof(r->why), fmt, ap);
    va_end(ap);
    r->verdict = st;
    r->program = program;
    r->programs = 0;
    return st;
}

/* Program i of m, read back and checked; its table into table (its
 * pkg_table_size bytes). */
static status_t check_program(const struct pkga_place *at, const struct pkg_manifest *m,
                              uint32_t i, uint32_t flags, uint8_t *table, struct passed *out,
                              struct pkga_result *r)
{
    const struct pkg_program *p = &m->programs[i];
    const struct pkg_file *f = &m->files[p->file];
    int pn = (int)p->path.n;
    char path[PATH_CAP];
    under(at, p->path, flags & PKGA_STAGED ? ".new" : "", path);
    struct mapped b;
    uint64_t size = 0;
    status_t st = map_file(path, PKG_FILE_MAX, &b, &size);
    if (st != OK)
        return refuse(r, st, i, "%.*s: can't read it (%s)", pn, p->path.p, status_str(st));
    if (size != f->size) {
        unmap(&b);
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s: %lu bytes, the manifest says %lu", pn,
                      p->path.p, (unsigned long)size, (unsigned long)f->size);
    }
    uint8_t digest[SHA256_BYTES], tdigest[SHA256_BYTES];
    sha256(b.p, size, digest);
    if (memcmp(digest, f->sha256, SHA256_BYTES)) {
        unmap(&b);
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s isn't the manifest's (its SHA-256)", pn,
                      p->path.p);
    }
    uint64_t tlen = pkg_table_size(size);
    struct pkg_tabler t;
    pkg_tabler_begin(&t, size, table, NULL);
    bool made = pkg_tabler_add(&t, b.p, size) && pkg_tabler_end(&t);
    if (made)
        sha256(table, tlen, tdigest);
    if (!made || tlen != p->table_size || memcmp(tdigest, p->table_sha256, SHA256_BYTES)) {
        unmap(&b);
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s: its page table isn't the manifest's", pn,
                      p->path.p);
    }
    static struct wants w;   /* one approval at a time (the worker's) */
    uint8_t list[PKG_LIST_MAX];
    size_t n = 0;
    st = wants_read(b.p, size, &w);
    if (st == OK && !w.found)
        st = ERR_INVALID_ARGS;
    if (st != OK) {
        unmap(&b);
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s: not a program Jam OS runs, or no list",
                      pn, p->path.p);
    }
    bool same = pkg_list_decode(p->list, list, &n) == OK && pkg_elf_list_is(b.p, size, list, n);
    unmap(&b);
    if (!same)
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s: its own list isn't the manifest's", pn,
                      p->path.p);
    const char *bad = dp_wants_refused(&w);
    if (bad || !w.posix)
        return refuse(r, ERR_ACCESS_DENIED, i, "%.*s asks for %s, which no program from /data "
                      "may have", pn, p->path.p, bad ? bad : "no posix");
    sha256_hex(digest, out->hex);
    snprintf(out->text, sizeof(out->text), "%s", w.text[0] ? w.text : "nothing");
    out->table_len = tlen;
    return OK;
}

/* The tables, each at <tables>/<hash>. */
static status_t write_tables(const struct pkga_place *at, const struct pkg_manifest *m,
                             const struct passed *ps, const uint8_t *tables)
{
    status_t st = mkdir_ok(at->etc);
    if (st == OK)
        st = mkdir_ok(at->tables);
    for (uint32_t i = 0; st == OK && i < m->nprograms; i++) {
        char path[PATH_CAP];
        snprintf(path, sizeof(path), "%s/%s", at->tables, ps[i].hex);
        st = write_whole(path, tables + ps[i].table_at, ps[i].table_len);
    }
    return st;
}

/* The lines, after those the approvals hold (none twice), unless a path
 * of ours is another package's. */
static status_t write_lines(const struct pkga_place *at, const struct pkg_manifest *m,
                            const struct passed *ps, struct pkga_result *r)
{
    char tag[PKGA_TAG_MAX];
    pkga_tag(m->name, m->version, m->revision, tag);
    uint32_t np = m->nprograms;
    char *all = NULL, *add = malloc((size_t)np * LINE_CAP + 1);
    bool *have = calloc(np ? np : 1, sizeof(bool));
    status_t st = add && have ? read_allow(at, &all) : ERR_NO_MEMORY;
    size_t addn = 0;
    if (st == OK) {
        for (uint32_t i = 0; i < np; i++) {
            char path[PATH_CAP];
            under(at, m->programs[i].path, "", path);
            snprintf(add + (size_t)i * LINE_CAP, LINE_CAP, "%s\t%s\t%s\t%s", ps[i].hex, path,
                     ps[i].text, tag);
        }
        static struct line l;   /* the worker's alone */
        for (const char *s = all; st == OK && *s;) {
            const char *nl = strchr(s, '\n');
            size_t len = nl ? (size_t)(nl - s) : strlen(s);
            if (line_read(s, len, &l)) {
                for (uint32_t i = 0; i < np; i++) {
                    const char *mine = add + (size_t)i * LINE_CAP;
                    char path[PATH_CAP];
                    under(at, m->programs[i].path, "", path);
                    if (strlen(mine) == len && !memcmp(mine, s, len))
                        have[i] = true;
                    else if (!strcmp(l.path, path) && l.tag && !tag_names(l.tag, m->name))
                        st = refuse(r, ERR_ACCESS_DENIED, i, "%s is package %s's", path,
                                    l.tag);
                }
            }
            s = nl ? nl + 1 : s + len;
        }
    }
    size_t had = all ? strlen(all) : 0;
    size_t cap = had + (size_t)np * LINE_CAP + 2;
    char *out = st == OK ? malloc(cap) : NULL;
    if (st == OK && !out)
        st = ERR_NO_MEMORY;
    if (st == OK) {
        memcpy(out, all, had);
        size_t n = had;
        if (n && out[n - 1] != '\n')
            out[n++] = '\n';
        for (uint32_t i = 0; i < np; i++)
            if (!have[i]) {
                n += (size_t)snprintf(out + n, cap - n, "%s\n", add + (size_t)i * LINE_CAP);
                addn++;
            }
        st = addn ? mkdir_ok(at->etc) : OK;
        if (st == OK && addn)
            st = write_whole(at->allow, out, n);
        if (st != OK)
            refuse(r, st, PKGA_NONE, "can't write %s (%s)", at->allow, status_str(st));
    }
    if (st != OK && r->verdict == OK)
        refuse(r, st, PKGA_NONE, "can't read %s (%s)", at->allow, status_str(st));
    free(out);
    free(all);
    free(add);
    free(have);
    return st;
}

status_t pkga_approve(const struct pkga_place *at, const uint8_t key[PKG_KEY_BYTES],
                      const void *text, size_t len, uint32_t flags, struct pkga_result *r)
{
    *r = (struct pkga_result){ .verdict = OK, .program = PKGA_NONE };
    if (flags & ~PKGA_STAGED)
        return refuse(r, ERR_INVALID_ARGS, PKGA_NONE, "unknown flags");
    struct pkg_manifest *m = calloc(1, sizeof(*m));
    if (!m)
        return refuse(r, ERR_NO_MEMORY, PKGA_NONE, "out of memory");
    const char *why = NULL;
    status_t st = pkg_manifest_parse(text, len, key, m, &why);
    if (st != OK) {
        free(m);
        return refuse(r, st, PKGA_NONE, "the manifest: %s", why ? why : status_str(st));
    }
    struct passed *ps = calloc(m->nprograms ? m->nprograms : 1, sizeof(*ps));
    struct mapped tables = { 0 };
    uint64_t total = pkg_tables_bytes(m);
    if (!ps || (total && map_new(total, &tables) != OK)) {
        free(ps);
        free(m);
        return refuse(r, ERR_NO_MEMORY, PKGA_NONE, "out of memory");
    }
    uint64_t off = 0;
    for (uint32_t i = 0; st == OK && i < m->nprograms; i++) {
        ps[i].table_at = off;
        st = check_program(at, m, i, flags, tables.p + off, &ps[i], r);
        off += ps[i].table_len;
    }
    if (st == OK && m->nprograms) {
        st = write_tables(at, m, ps, tables.p);
        if (st != OK)
            refuse(r, st, PKGA_NONE, "can't write the page tables in %s (%s)", at->tables,
                   status_str(st));
    }
    if (st == OK && m->nprograms)
        st = write_lines(at, m, ps, r);
    if (st == OK) {
        r->programs = m->nprograms;
        r->why[0] = '\0';
    }
    unmap(&tables);
    free(ps);
    free(m);
    return r->verdict = st;
}

/* ---- revoke ------------------------------------------------------------------------ */

/* A name or a version as a tag may hold it. */
static bool word_ok(struct pkg_str s)
{
    if (s.n < 1 || s.n > 63)
        return false;
    for (uint32_t i = 0; i < s.n; i++)
        if ((uint8_t)s.p[i] < 0x21 || (uint8_t)s.p[i] > 0x7e)
            return false;
    return true;
}

/* Every line tagged `tag` (with "pkg=") out of the approvals; *lines: how
 * many. Written back only if one went. */
static status_t revoke_tag(const struct pkga_place *at, const char *tag, uint32_t *lines)
{
    char *all = NULL;
    *lines = 0;
    status_t st = read_allow(at, &all);
    if (st != OK)
        return st;
    size_t had = strlen(all);
    char *out = malloc(had + 1), *gone = malloc(had / DP_HEX_LEN * DP_HEX_LEN + DP_HEX_LEN);
    if (!out || !gone) {
        free(out);
        free(gone);
        free(all);
        return ERR_NO_MEMORY;
    }
    static struct line l;   /* the worker's alone */
    size_t n = 0;
    for (const char *s = all; *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (line_read(s, len, &l) && l.tag && !strcmp(l.tag, tag + 4)) {
            memcpy(gone + (size_t)(*lines)++ * DP_HEX_LEN, l.hash, DP_HEX_LEN);
        } else {
            memcpy(out + n, s, len);
            n += len;
            if (nl)
                out[n++] = '\n';
        }
        s = nl ? nl + 1 : s + len;
    }
    out[n] = '\0';
    if (*lines)
        st = write_whole(at->allow, out, n);
    if (st == OK && *lines)
        drop_tables(at, gone, *lines, out);
    if (st != OK)
        *lines = 0;
    free(out);
    free(gone);
    free(all);
    return st;
}

status_t pkga_revoke(const struct pkga_place *at, struct pkg_str name, struct pkg_str version,
                     uint32_t revision, uint32_t *lines)
{
    *lines = 0;
    if (!word_ok(name) || !word_ok(version) || !revision)
        return ERR_INVALID_ARGS;
    char tag[PKGA_TAG_MAX];
    pkga_tag(name, version, revision, tag);
    return revoke_tag(at, tag, lines);
}

/* ---- settle ------------------------------------------------------------------------ */

/* A manifest from db/, its text kept for the manifest's pointers. */
struct loaded {
    struct mapped        text;
    struct pkg_manifest *m;
};

static void unload(struct loaded *l)
{
    unmap(&l->text);
    free(l->m);
    l->m = NULL;
}

/* The manifest at path, if its SHA-256 is want and key signed it. */
static status_t load(const char *path, const uint8_t *key, const uint8_t want[SHA256_BYTES],
                     struct loaded *l)
{
    uint64_t size = 0;
    uint8_t digest[SHA256_BYTES];
    *l = (struct loaded){ 0 };
    status_t st = map_file(path, PKG_MANIFEST_MAX, &l->text, &size);
    if (st != OK)
        return st;
    sha256(l->text.p, size, digest);
    l->m = calloc(1, sizeof(*l->m));
    st = !l->m ? ERR_NO_MEMORY :
         memcmp(digest, want, SHA256_BYTES) ? ERR_ACCESS_DENIED :
         pkg_manifest_parse(l->text.p, size, key, l->m, NULL);
    if (st != OK)
        unload(l);
    return st;
}

/* db/<name><suffix>. */
static void db_path(const struct pkga_place *at, struct pkg_str name, const char *suffix,
                    char *out)
{
    snprintf(out, PATH_CAP, "%s/db/%.*s%s", at->prefix, (int)name.n, name.p, suffix);
}

/* Is l the manifest of (name, version, revision)? */
static bool is_of(const struct loaded *l, struct pkg_str name, struct pkg_str version,
                  uint32_t revision)
{
    return !pkg_str_cmp(l->m->name, name) && !pkg_str_cmp(l->m->version, version) &&
           l->m->revision == revision;
}

/* Each of m's files at `<path>.new` moved to its path (one already there
 * goes first: FAT can't rename over it). One with no .new is taken as
 * moved already. */
static status_t swap_in(const struct pkga_place *at, const struct pkg_manifest *m)
{
    status_t st = OK;
    for (uint32_t i = 0; st == OK && i < m->nfiles; i++) {
        char from[PATH_CAP], to[PATH_CAP];
        under(at, m->files[i].path, ".new", from);
        under(at, m->files[i].path, "", to);
        if (fs_stat(from, NULL, NULL, NULL) != OK)
            continue;
        st = unlink_ok(to);
        if (st == OK)
            st = fs_rename(from, to);
    }
    return st == OK ? fs_sync(at->prefix) : st;
}

/* Each of m's files (keep: not those keep also has, by FAT32's names), at
 * its path, or with ".new" (staged). */
static status_t remove_files(const struct pkga_place *at, const struct pkg_manifest *m,
                             const struct pkg_manifest *keep, bool staged)
{
    status_t st = OK;
    for (uint32_t i = 0; st == OK && i < m->nfiles; i++) {
        bool kept = false;
        for (uint32_t k = 0; keep && !kept && k < keep->nfiles; k++)
            kept = pkg_path_fold_equal(m->files[i].path, keep->files[k].path);
        if (kept)
            continue;
        char path[PATH_CAP];
        under(at, m->files[i].path, staged ? ".new" : "", path);
        st = unlink_ok(path);
    }
    return st == OK ? fs_sync(at->prefix) : st;
}

/* db/<name>.new made db/<name>, if it is still there. */
static status_t record(const struct pkga_place *at, struct pkg_str name)
{
    char now[PATH_CAP], fresh[PATH_CAP];
    db_path(at, name, "", now);
    db_path(at, name, ".new", fresh);
    if (fs_stat(fresh, NULL, NULL, NULL) != OK)
        return OK;
    status_t st = unlink_ok(now);
    if (st == OK)
        st = fs_rename(fresh, now);
    return st == OK ? fs_sync(at->prefix) : st;
}

static status_t revoke_of(const struct pkga_place *at, struct pkg_str name, struct pkg_str version,
                          uint32_t revision)
{
    uint32_t n;
    return pkga_revoke(at, name, version, revision, &n);
}

/* An install or an upgrade before its commit point: the staged files and
 * the new manifest go, and any line the new version got (a cut inside
 * approve's write), unless the same version is the one installed. */
static status_t undo(const struct pkga_place *at, const struct pkg_journal *j,
                     const uint8_t *key)
{
    char fresh[PATH_CAP], now[PATH_CAP];
    db_path(at, j->name, ".new", fresh);
    db_path(at, j->name, "", now);
    struct loaded l;
    status_t st = load(fresh, key, j->manifest, &l);
    if (st == ERR_NO_MEMORY || st == ERR_IO)
        return st;   /* tried again next time */
    /* No manifest (a cut before or while it was written, or one that isn't
     * the journal's): what was staged isn't known, and stays at its .new
     * names, which nothing runs or reads. */
    if (st == OK && is_of(&l, j->name, j->version, j->revision))
        st = remove_files(at, l.m, NULL, true);
    else
        st = OK;
    if (l.m)
        unload(&l);
    if (st != OK)
        return st;
    bool same = j->op == PKG_OP_UPGRADE ?
                !pkg_str_cmp(j->version, j->old_version) && j->revision == j->old_revision :
                fs_stat(now, NULL, NULL, NULL) == OK;
    st = same ? OK : revoke_of(at, j->name, j->version, j->revision);
    return st == OK ? unlink_ok(fresh) : st;
}

/* An install or an upgrade from its commit point on. */
static status_t finish(const struct pkga_place *at, const struct pkg_journal *j,
                       const uint8_t *key)
{
    char fresh[PATH_CAP], now[PATH_CAP];
    db_path(at, j->name, ".new", fresh);
    db_path(at, j->name, "", now);
    struct loaded nw, old = { 0 };
    status_t st = load(fresh, key, j->manifest, &nw);
    if (st == ERR_NOT_FOUND)   /* recorded already (the rename was the last step) */
        st = load(now, key, j->manifest, &nw);
    if (st == OK && !is_of(&nw, j->name, j->version, j->revision)) {
        unload(&nw);
        st = ERR_ACCESS_DENIED;
    }
    if (st != OK)
        return st;
    if (j->op == PKG_OP_UPGRADE && load(now, key, j->old_manifest, &old) == OK &&
        !is_of(&old, j->name, j->old_version, j->old_revision))
        unload(&old);
    if (j->step <= PKG_STEP_APPROVED)
        st = swap_in(at, nw.m);
    if (st == OK && j->step <= PKG_STEP_APPROVED && old.m)
        st = remove_files(at, old.m, nw.m, false);   /* what the new version doesn't have */
    bool same = !pkg_str_cmp(j->version, j->old_version) && j->revision == j->old_revision;
    if (st == OK && j->op == PKG_OP_UPGRADE && j->step <= PKG_STEP_SWAPPED && !same)
        st = revoke_of(at, j->name, j->old_version, j->old_revision);
    if (st == OK)
        st = record(at, j->name);
    unload(&nw);
    if (old.m)
        unload(&old);
    return st;
}

/* A remove, from wherever it was. */
static status_t finish_remove(const struct pkga_place *at, const struct pkg_journal *j,
                              const uint8_t *key)
{
    char now[PATH_CAP];
    db_path(at, j->name, "", now);
    status_t st = j->step <= PKG_STEP_BEGIN ?
                  revoke_of(at, j->name, j->old_version, j->old_revision) : OK;
    if (st != OK || j->step >= PKG_STEP_REMOVED)
        return st == OK ? unlink_ok(now) : st;
    struct loaded old;
    st = load(now, key, j->old_manifest, &old);
    if (st == ERR_NOT_FOUND)
        return OK;   /* recorded already: the files went before it */
    if (st == OK && !is_of(&old, j->name, j->old_version, j->old_revision)) {
        unload(&old);
        st = ERR_ACCESS_DENIED;
    }
    if (st != OK)
        return st;
    st = remove_files(at, old.m, NULL, false);
    unload(&old);
    return st == OK ? unlink_ok(now) : st;
}

void pkga_settle(const struct pkga_place *at, const uint8_t *key, struct pkga_settled *out)
{
    *out = (struct pkga_settled){ .st = OK };
    char path[PATH_CAP], fresh[PATH_CAP];
    snprintf(path, sizeof(path), "%s/db/journal", at->prefix);
    snprintf(fresh, sizeof(fresh), "%s/db/journal.new", at->prefix);
    struct mapped t = { 0 };
    uint64_t size = 0;
    status_t st = map_file(path, PKG_JOURNAL_MAX, &t, &size);
    bool from_new = false;
    if (st == ERR_NOT_FOUND) {   /* bin/pkg was cut between taking journal away and renaming */
        st = map_file(fresh, PKG_JOURNAL_MAX, &t, &size);
        from_new = true;
    }
    if (st == ERR_NOT_FOUND)
        return;
    out->journal = true;
    struct pkg_journal *j = calloc(1, sizeof(*j));
    const char *why = NULL;
    if (st == OK)
        st = j ? pkg_journal_parse(t.p, size, j, &why) : ERR_NO_MEMORY;
    if (st != OK && from_new && st != ERR_NO_MEMORY) {
        /* A journal.new cut short with no journal: the first step never finished,
         * so nothing began. */
        out->journal = false;
        (void)unlink_ok(fresh);
        free(j);
        unmap(&t);
        return;
    }
    if (st != OK) {
        snprintf(out->what, sizeof(out->what), "the journal can't be read (%s): left for pkg",
                 why ? why : status_str(st));
    } else if (!key) {
        st = ERR_NOT_SUPPORTED;
        snprintf(out->what, sizeof(out->what), "a journal, and no ports key: left as it is");
    } else {
        bool fwd = j->op == PKG_OP_REMOVE || j->step >= PKG_STEP_APPROVED;
        st = j->op == PKG_OP_REMOVE ? finish_remove(at, j, key) :
             fwd                    ? finish(at, j, key) : undo(at, j, key);
        if (st == OK)
            st = unlink_ok(fresh);   /* a step bin/pkg wrote but did not publish */
        if (st == OK)
            st = unlink_ok(path);
        out->forward = fwd;
        snprintf(out->what, sizeof(out->what), "%s %.*s %s at %s: %s%s%s",
                 pkg_op_name(j->op), (int)j->name.n, j->name.p,
                 fwd ? "finished" : "undone", pkg_step_name(j->step),
                 st == OK ? "done" : "stopped (", st == OK ? "" : status_str(st),
                 st == OK ? "" : ")");
    }
    out->st = st;
    free(j);
    unmap(&t);
}
