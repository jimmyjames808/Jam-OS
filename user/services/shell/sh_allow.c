/* Programs from /data: the owner's approvals (`allow`), their page
 * tables, and the whole-read way of checking a program file
 * (docs/HISTORY.md, M8.6; running one from its pages is sh_dataprog.c).
 *
 * The approvals are lines of /data/etc/allow, each "<sha-256>\t<path>\t
 * <what its list asks for>", written here, and by init for a package's
 * programs with a fourth field, the package (pkgctl.approve; `allow -l`
 * shows it, and `allow -r` or `allow <file>` never drops such a line:
 * `pkg remove` does, through init). Every program's /data is a
 * view that leaves etc alone (<fsview.h>), so only init and this shell,
 * which hold the whole of /data, can change the file: a program can never
 * approve another, or itself. Whoever holds the stick can edit it on
 * another computer, as they could replace the kernel (decision 11).
 *
 * Beside each approval, its page table: /data/etc/allow.d/<sha-256>, the
 * SHA-256 of each page of the approved bytes (<dataprog.h>), which the
 * kernel checks every page of a running program against. `allow` writes
 * the table before the line (a line is never there without its table,
 * except one written before tables were), and a table goes when no line
 * names its hash any more (`allow -r`, or a new approval of the path).
 * Files with the same bytes share one table.
 *
 * The whole read (sh_program_file): the file is read into a VMO only we
 * hold and hashed as it is read, then made executable (vmo_make_exec,
 * which needs RIGHT_ROOT_VMEX: this shell's root, no program's) and from
 * then on can't change; its hash, its list and its page table are of
 * those very bytes. A program can be as big as Chromium's (DP_PROGRAM_MAX,
 * 1 GiB; docs/CR-PLAN.md section 6): one of PROGRESS_MIN or more shows a
 * line while it is read, and Ctrl+C stops it. What it costs: the whole
 * file in memory while it is checked (until `allow`'s answer), charged to
 * the shell's job, and a table of 32 bytes a page (8 MiB at 1 GiB) in a
 * VMO of its own while it is written. `allow` uses it,
 * and so does running a program whose mount has no pages to give, or whose
 * approval has no table yet, or which is refused (the refusal is explained
 * from those bytes): it runs only if a line holds exactly that path and
 * that hash, so a program changed after it was allowed is refused, and
 * nothing can change it between the check and the start. */
#include <dataprog.h>
#include <sha256.h>
#include <wants.h>
#include "sh.h"

#define ALLOW_FILE DP_ALLOW_FILE
#define ALLOW_DIR  DP_ALLOW_DIR
#define TABLE_DIR  DP_TABLE_DIR
#define ALLOW_HEAD "# Programs on /data the owner allowed to run (the shell's `allow`):\n" \
                   "# <sha-256>\\t<path>\\t<what it asked for>[\\tpkg=<package>]\n"
#define PROGRAM_MAX  DP_PROGRAM_MAX
#define HEX_LEN      DP_HEX_LEN
#define READ_CHUNK   (1u << 20)      /* bytes read and hashed at a time */
#define PROGRESS_MIN (32ull << 20)   /* a file this big shows a line while it is read */

/* The places a program may be allowed and run from: /data, /jam (the Jam
 * OS drive's ext data partition, M13.6) and /pkg (init's read-only view of
 * the packages' prefix, M13.7's PO14), all of dp_on_data's. Approvals stay
 * in /data/etc/allow either way. */
bool sh_on_data(const char *abs)
{
    return dp_on_data(abs);
}

/* An approval's path, copied out of a line left whole; false: not one. */
static bool line_path(const char *line, char path[SH_PATH_MAX])
{
    const char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (line[0] == '#' || !t1 || !t2 || t1 - line != 2 * SHA256_BYTES ||
        (size_t)(t2 - t1 - 1) >= SH_PATH_MAX)
        return false;
    memcpy(path, t1 + 1, (size_t)(t2 - t1 - 1));
    path[t2 - t1 - 1] = '\0';
    return true;
}

/* Is the line (left whole) a package's (a tag after its list,
 * dp_allow_tag)? */
static bool line_tagged(const char *line)
{
    const char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    const char *t = t2 ? strrchr(t2 + 1, '\t') : NULL;
    return t && !strncmp(t + 1, "pkg=", 4);
}

/* The file's text, a copy the caller frees (NULL and *st: none, or why). */
static char *read_allow(status_t *st)
{
    const void *data;
    uint64_t size;
    *st = sh_read(ALLOW_FILE, &data, &size);
    char *copy = *st == OK ? malloc(size + 1) : NULL;
    if (*st == OK && !copy)
        *st = ERR_NO_MEMORY;
    if (copy) {
        memcpy(copy, data, size);
        copy[size] = '\0';
    }
    return copy;
}

int sh_allow_each(void (*fn)(const char *hash, const char *path, const char *text, void *ctx),
                  void *ctx)
{
    status_t st;
    char *all = read_allow(&st);
    int n = 0;
    for (char *line = all; line && *line;) {
        char *nl = strchr(line, '\n'), *hash, *path, *text;
        if (nl)
            *nl = '\0';
        if (dp_allow_split(line, &hash, &path, &text)) {
            /* A package's line says so (init wrote it: `pkg remove` takes it back). */
            static char shown[WANTS_TEXT_MAX + 160];
            const char *tag = dp_allow_tag(text);
            if (tag)
                snprintf(shown, sizeof(shown), "%s [package %s]", text, tag);
            fn(hash, path, tag ? shown : text, ctx);
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(all);
    return st == OK || st == ERR_NOT_FOUND ? n : st;
}

/* Does a line of text (the approvals as written back) name this hash? */
static bool hash_named(const char *text, const char *hex)
{
    for (const char *line = text; line && *line;) {
        if (!strncmp(line, hex, HEX_LEN) && line[HEX_LEN] == '\t')
            return true;
        line = strchr(line, '\n');
        line = line ? line + 1 : NULL;
    }
    return false;
}

static void table_path(const char *hex, char path[SH_PATH_MAX])
{
    snprintf(path, SH_PATH_MAX, TABLE_DIR "/%.*s", HEX_LEN, hex);
}

/* The tables of the dropped lines' hashes (gone: n of them, HEX_LEN
 * bytes each) that no line in kept names any more. One that won't go
 * stays behind, unused: nothing reads a table without its line. */
static void drop_tables(const char *gone, unsigned n, const char *kept)
{
    for (unsigned i = 0; i < n; i++) {
        char hex[HEX_LEN + 1], path[SH_PATH_MAX];
        memcpy(hex, gone + (size_t)i * HEX_LEN, HEX_LEN);
        hex[HEX_LEN] = '\0';
        table_path(hex, path);
        if (!hash_named(kept, hex))
            (void)fs_unlink(path);   /* none (an approval from before tables): fine */
    }
}

/* The approvals as they are, without those `drop` says to drop, plus one
 * new line (NULL: none), written back whole; then the tables no line
 * needs any more go. *dropped: how many lines went. ERR_BAD_STATE, and
 * nothing written, if a line to drop is a package's: only init drops
 * those (pkgctl.revoke, for `pkg remove`). */
static status_t rewrite(bool (*drop)(const char *path, void *ctx), void *ctx, const char *add,
                        unsigned *dropped)
{
    status_t st;
    char *all = read_allow(&st);
    if (st != OK && st != ERR_NOT_FOUND)
        return st;
    size_t had = all ? strlen(all) : 0;
    size_t cap = had + (add ? strlen(add) : 0) + sizeof(ALLOW_HEAD) + 2;
    char *out = malloc(cap), *gone = malloc(had + 1);   /* a dropped line is longer than its hash */
    if (!out || !gone) {
        free(out);
        free(gone);
        free(all);
        return ERR_NO_MEMORY;
    }
    size_t n = (size_t)snprintf(out, cap, "%s", ALLOW_HEAD);
    bool package = false;
    *dropped = 0;
    for (char *line = all; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char path[SH_PATH_MAX];
        if (line_path(line, path) && drop(path, ctx)) {
            package |= line_tagged(line);
            memcpy(gone + (size_t)(*dropped)++ * HEX_LEN, line, HEX_LEN);
        } else if (line[0] != '#' && line[0]) {
            n += (size_t)snprintf(out + n, cap - n, "%s\n", line);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (package) {
        free(gone);
        free(out);
        free(all);
        *dropped = 0;
        return ERR_BAD_STATE;
    }
    if (add)
        n += (size_t)snprintf(out + n, cap - n, "%s\n", add);
    free(all);
    st = fs_mkdir(ALLOW_DIR);
    if (st == ERR_ALREADY_EXISTS)
        st = OK;
    if (st == OK)
        st = sh_write(ALLOW_FILE, out, n, FS_TRUNCATE);
    if (st == OK)
        st = fs_sync(ALLOW_FILE);
    if (st == OK)
        drop_tables(gone, *dropped, out);
    free(gone);
    free(out);
    return st;
}

static bool same_path(const char *path, void *ctx)
{
    return dp_path_same(path, ctx);
}

/* name is the path, or its last name. */
static bool named(const char *path, void *ctx)
{
    return !strcmp(path, ctx) || !strcmp(sh_basename(path), ctx);
}

status_t sh_allow_add(const char *path, handle_t vmo, uint64_t size, const char *hash_hex,
                      const char *text)
{
    char line[SH_PATH_MAX + HEX_LEN + WANTS_TEXT_MAX + 4];
    unsigned dropped;
    status_t st = sh_allow_table_write(hash_hex, vmo, size);   /* the table before its line */
    if (st != OK)
        return st;
    snprintf(line, sizeof(line), "%s\t%s\t%s", hash_hex, path, text[0] ? text : "nothing");
    return rewrite(same_path, (void *)path, line, &dropped);   /* the old line for it goes */
}

status_t sh_allow_remove(const char *name, unsigned *removed)
{
    return rewrite(named, (void *)name, NULL, removed);
}

/* ---- page tables --------------------------------------------------------------------- */

static status_t mkdir_ok(const char *path)
{
    status_t st = fs_mkdir(path);
    return st == ERR_ALREADY_EXISTS ? OK : st;
}

status_t sh_allow_table_write(const char *hash_hex, handle_t vmo, uint64_t size)
{
    uint8_t sha[SHA256_BYTES];
    struct handle_info hi;
    if (!dp_hex_bytes(hash_hex, sha) || !size || size > PROGRAM_MAX)
        return ERR_INVALID_ARGS;
    /* Mapped and read here: never a pager VMO (a page that failed would
     * kill the shell), only sh_program_file's, whose pages are ours. */
    status_t st = jam_handle_info(vmo, &hi);
    if (st == OK && (hi.type != OBJ_VMO || hi.related_koid))
        st = ERR_NOT_SUPPORTED;
    handle_t self = startup_handle(SR_SELF_VMAR), tv = HANDLE_INVALID;
    uint64_t pages = dp_ptable_pages(size), len = pages * PAGE_SIZE, addr = 0, at = 0;
    size_t bytes = sizeof(struct dp_ptable_head) + pages * SHA256_BYTES;
    uint64_t tlen = (bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* The table in a VMO of its own (8 MiB for the biggest program), not
     * the heap, whose pages would stay the shell's for good. */
    if (st == OK)
        st = jam_vmo_create(tlen, 0, &tv);
    if (st == OK)
        st = jam_vmar_map(self, tv, 0, tlen, VMAR_READ | VMAR_WRITE, &at);
    uint8_t *buf = (uint8_t *)(uintptr_t)at;
    if (st == OK)
        st = jam_vmar_map(self, vmo, 0, len, VMAR_READ, &addr);
    if (st == OK) {
        dp_ptable_hash((const uint8_t *)(uintptr_t)addr, size,
                       buf + sizeof(struct dp_ptable_head));
        jam_vmar_unmap(self, addr, len);
        dp_ptable_head(size, sha, (struct dp_ptable_head *)buf);
    }
    char path[SH_PATH_MAX];
    table_path(hash_hex, path);
    if (st == OK)
        st = mkdir_ok(ALLOW_DIR);
    if (st == OK)
        st = mkdir_ok(TABLE_DIR);
    if (st == OK)
        st = sh_write(path, buf, bytes, FS_TRUNCATE);
    if (st == OK)
        st = fs_sync(path);
    if (at)
        jam_vmar_unmap(self, at, tlen);
    if (tv)
        jam_handle_close(tv);
    return st;
}

/* ---- the file itself ----------------------------------------------------------------- */

/* The list of what no program from /data may have is libos's
 * (<dataprog.h>), shared with init's /svc/exec. */
const char *sh_wants_refused(const struct wants *w)
{
    return dp_wants_refused(w);
}

/* dp_image_wants' reader over bytes in memory (a mapping of our own VMO). */
struct mem_image {
    const uint8_t *bytes;   /* the file */
    uint64_t       size;    /* its bytes */
};

static status_t mem_read(void *ctx, uint64_t off, void *dst, uint64_t len)
{
    const struct mem_image *m = ctx;
    if (off > m->size || len > m->size - off)
        return ERR_INVALID_ARGS;
    memcpy(dst, m->bytes + off, len);
    return OK;
}

/* The tenths of a file of total bytes that off has reached, said on the
 * progress line from *shown on. */
static void progress(uint64_t off, uint64_t total, unsigned *shown)
{
    bool said = false;
    for (; *shown < 10 && off * 10 >= total * (*shown + 1); ++*shown, said = true)
        sh_tty(" %u%%", (*shown + 1) * 10);
    if (said)
        sh_flush();
}

/* The file at path read whole into a new VMO that only we hold (*raw,
 * unmapped; *size bytes, rounded up to whole pages), its SHA-256 taken as
 * it is read: the bytes hashed are the bytes written, and nothing else
 * can write the VMO. A file of PROGRESS_MIN or more says how far it got on a
 * line of its own; Ctrl+C stops it (ERR_CANCELED). ERR_OUT_OF_RANGE: past
 * PROGRAM_MAX; ERR_IO: it shrank as it was read. */
static status_t read_whole(const char *path, handle_t *raw, uint64_t *size,
                           uint8_t digest[SHA256_BYTES])
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    handle_t self = startup_handle(SR_SELF_VMAR), v = HANDLE_INVALID;
    uint64_t total = 0, len = 0, addr = 0;
    st = file_stat(&f, &total, NULL);
    if (st == OK && total > PROGRAM_MAX)
        st = ERR_OUT_OF_RANGE;
    if (st == OK) {
        len = total ? (total + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1) : PAGE_SIZE;
        st = jam_vmo_create(len, 0, &v);
    }
    if (st == OK)
        st = jam_vmar_map(self, v, 0, len, VMAR_READ | VMAR_WRITE, &addr);
    uint8_t *bytes = (uint8_t *)(uintptr_t)addr;
    bool show = st == OK && total >= PROGRESS_MIN;
    unsigned shown = 0;
    if (show) {
        sh_tty("checking %s (%lu MiB):", path, (unsigned long)(total >> 20));
        sh_flush();
    }
    struct sha256 s;
    sha256_init(&s);
    for (uint64_t off = 0; st == OK && off < total;) {
        size_t want = total - off < READ_CHUNK ? (size_t)(total - off) : READ_CHUNK, got = 0;
        st = file_read(&f, off, bytes + off, want, &got);
        if (st == OK && got != want)
            st = ERR_IO;   /* the file shrank under us */
        if (st == OK)
            sha256_add(&s, bytes + off, got);
        off += got;
        if (st == OK && sh_interrupted())
            st = ERR_CANCELED;
        if (show && st == OK)
            progress(off, total, &shown);
    }
    if (show)
        sh_tty(st == OK ? "\n" : " stopped\n");
    file_close(&f);
    if (addr)
        jam_vmar_unmap(self, addr, len);   /* vmo_make_exec takes it unmapped */
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        return st;
    }
    sha256_done(&s, digest);
    *raw = v;
    *size = total;
    return OK;
}

status_t sh_program_file(const char *path, handle_t *vmo, uint64_t *size, char *hex,
                         struct wants *w)
{
    handle_t raw, x;
    uint64_t n, addr = 0;
    uint8_t digest[SHA256_BYTES];
    if (!sh_root())
        return ERR_ACCESS_DENIED;   /* a shell init didn't start: no root, no VMEX */
    status_t st = read_whole(path, &raw, &n, digest);
    if (st != OK)
        return st;
    st = jam_vmo_make_exec(raw, sh_root(), &x);   /* raw is ours alone, and unmapped */
    if (st != OK) {
        if (st != ERR_BAD_STATE)
            jam_handle_close(raw);   /* refused before it was taken */
        return st;
    }
    sha256_hex(digest, hex);
    uint64_t len = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), x, 0, len ? len : PAGE_SIZE, VMAR_READ,
                      &addr);
    if (st == OK) {
        struct mem_image m = { (const uint8_t *)(uintptr_t)addr, n };
        /* Read as a run from the pages reads it: the same rules, the same
         * answer. */
        st = dp_image_wants(mem_read, &m, n, w);
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, len ? len : PAGE_SIZE);
    }
    if (st != OK) {
        jam_handle_close(x);
        return st;
    }
    *vmo = x;
    *size = n;
    return OK;
}

/* Is there a line for path, and with this hash? */
struct find {
    const char *path, *hex;   /* what is looked for */
    bool        path_seen;    /* a line for the path */
    bool        match;        /* ... with the hash */
};

static void find_line(const char *hash, const char *path, const char *text, void *ctx)
{
    struct find *f = ctx;
    (void)text;
    if (!dp_path_same(path, f->path))
        return;
    f->path_seen = true;
    f->match |= !strcmp(hash, f->hex);
}

bool sh_allowed_whole(const char *path, handle_t *vmo, uint64_t *size, struct wants *w)
{
    char hex[HEX_LEN + 1];
    if (!sh_on_data(path)) {
        sh_tty("run: %s: only programs in /boot and on /data can run (copy it to /data first)\n",
               path);
        return false;
    }
    status_t st = sh_program_file(path, vmo, size, hex, w);
    if (st == ERR_ACCESS_DENIED) {
        sh_tty("run: %s: this shell can't run programs from /data (only the one init starts "
               "can)\n", path);
        return false;
    }
    if (st != OK) {
        sh_tty("run: %s: %s\n", path, st == ERR_INVALID_ARGS ? "not a program it can run" :
                                       sh_why(st));
        return false;
    }
    const char *bad = sh_wants_refused(w);
    if (bad) {
        /* An approval written before this rule, or on another computer. */
        sh_tty("run: %s: asks for %s, which no program from /data may have\n", path, bad);
        jam_handle_close(*vmo);
        return false;
    }
    struct find f = { .path = path, .hex = hex };
    int n = sh_allow_each(find_line, &f);
    if (n >= 0 && f.match)
        return true;
    if (n < 0)
        sh_tty("run: %s: can't read " ALLOW_FILE " (%s)\n", path, sh_why(n));
    else if (f.path_seen)
        sh_tty("run: %s changed since it was allowed: `allow %s` again\n", path, path);
    else
        sh_tty("run: %s is not allowed to run: `allow %s` first\n", path, path);
    jam_handle_close(*vmo);
    return false;
}
