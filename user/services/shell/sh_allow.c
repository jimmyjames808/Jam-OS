/* Programs from /data: the owner's approvals (`allow`), their page
 * tables, and the whole-read way of checking a program file
 * (docs/HISTORY.md, M8.6; running one from its pages is sh_dataprog.c).
 *
 * The approvals are lines of /data/etc/allow, each "<sha-256>\t<path>\t
 * <what its list asks for>", written only here. Every program's /data is a
 * view that leaves etc alone (<fsview.h>), so only init and this shell,
 * which hold the whole of /data, can change the file: a program can never
 * approve another, or itself. Whoever holds the stick can edit it on
 * another computer, as they could replace the kernel (decision 11).
 *
 * Beside each approval, its page table: /data/etc/allow.d/<sha-256>, the
 * SHA-256 of each page of the approved bytes (sh_image.h), which the
 * kernel checks every page of a running program against. `allow` writes
 * the table before the line (a line is never there without its table,
 * except one written before tables were), and a table goes when no line
 * names its hash any more (`allow -r`, or a new approval of the path).
 * Files with the same bytes share one table.
 *
 * The whole read (sh_program_file): the file is read into a VMO only we
 * hold, made executable (vmo_make_exec, which needs RIGHT_ROOT_VMEX: this
 * shell's root, no program's) and from then on can't change; its hash, its
 * list and its page table are read from those very bytes. `allow` uses it,
 * and so does running a program whose mount has no pages to give, or whose
 * approval has no table yet, or which is refused (the refusal is explained
 * from those bytes): it runs only if a line holds exactly that path and
 * that hash, so a program changed after it was allowed is refused, and
 * nothing can change it between the check and the start. */
#include <sha256.h>
#include <wants.h>
#include "sh.h"
#include "sh_image.h"

#define ALLOW_FILE "/data/etc/allow"
#define ALLOW_DIR  "/data/etc"
#define TABLE_DIR  "/data/etc/allow.d"
#define TABLE_CHUNK (64u << 10)   /* a table's hashes read at a time */
#define ALLOW_HEAD "# Programs on /data the owner allowed to run (the shell's `allow`):\n" \
                   "# <sha-256>\\t<path>\\t<what it asked for>\n"
#define PROGRAM_MAX (64ull << 20)   /* spawn's own limit, and VMO_VERIFIED_MAX */
#define HEX_LEN     (2 * SHA256_BYTES)

bool sh_on_data(const char *abs)
{
    return !strncmp(abs, "/data/", 6) && abs[6];
}

/* One line of the file: its fields, cut in place. false: not an approval. */
static bool split_line(char *line, char **hash, char **path, char **text)
{
    char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (line[0] == '#' || !t1 || !t2 || t1 - line != 2 * SHA256_BYTES)
        return false;
    *t1 = *t2 = '\0';
    *hash = line;
    *path = t1 + 1;
    *text = t2 + 1;
    return true;
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
        if (split_line(line, &hash, &path, &text)) {
            fn(hash, path, text, ctx);
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
 * needs any more go. *dropped: how many lines went. */
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
    *dropped = 0;
    for (char *line = all; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char path[SH_PATH_MAX];
        if (line_path(line, path) && drop(path, ctx))
            memcpy(gone + (size_t)(*dropped)++ * HEX_LEN, line, HEX_LEN);
        else if (line[0] != '#' && line[0])
            n += (size_t)snprintf(out + n, cap - n, "%s\n", line);
        line = nl ? nl + 1 : NULL;
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
    return !strcmp(path, ctx);
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
    if (!sh_hex_bytes(hash_hex, sha) || !size || size > PROGRAM_MAX)
        return ERR_INVALID_ARGS;
    /* Mapped and read here: never a pager VMO (a page that failed would
     * kill the shell), only sh_program_file's, whose pages are ours. */
    status_t st = jam_handle_info(vmo, &hi);
    if (st == OK && (hi.type != OBJ_VMO || hi.related_koid))
        st = ERR_NOT_SUPPORTED;
    uint64_t pages = sh_ptable_pages(size), len = pages * PAGE_SIZE, addr = 0;
    size_t bytes = sizeof(struct sh_ptable_head) + pages * SHA256_BYTES;
    uint8_t *buf = st == OK ? malloc(bytes) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ, &addr);
    if (st == OK) {
        sh_ptable_hash((const uint8_t *)(uintptr_t)addr, size,
                       buf + sizeof(struct sh_ptable_head));
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, len);
        sh_ptable_head(size, sha, (struct sh_ptable_head *)buf);
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
    free(buf);
    return st;
}

/* The hashes of the open table f (pages of them, after its header) into
 * a new VMO, from its offset 0, as the kernel takes them. */
static status_t table_hashes(struct jfile *f, uint64_t pages, handle_t *out)
{
    uint64_t bytes = pages * SHA256_BYTES;
    handle_t t;
    uint8_t *chunk = malloc(TABLE_CHUNK);
    status_t st = chunk ? jam_vmo_create(bytes, 0, &t) : ERR_NO_MEMORY;
    if (st != OK) {
        free(chunk);
        return st;
    }
    for (uint64_t at = 0; at < bytes && st == OK; at += TABLE_CHUNK) {
        size_t len = bytes - at < TABLE_CHUNK ? (size_t)(bytes - at) : TABLE_CHUNK, got = 0;
        st = file_read(f, sizeof(struct sh_ptable_head) + at, chunk, len, &got);
        if (st == OK && got != len)
            st = ERR_INVALID_ARGS;   /* shorter than its stat said: broken */
        if (st == OK)
            st = jam_vmo_write(t, at, chunk, len);
    }
    free(chunk);
    if (st != OK) {
        jam_handle_close(t);
        return st;
    }
    *out = t;
    return OK;
}

status_t sh_allow_table_read(const char *hash_hex, uint64_t size, handle_t *table)
{
    uint8_t sha[SHA256_BYTES];
    if (!sh_hex_bytes(hash_hex, sha))
        return ERR_INVALID_ARGS;
    if (!size || size > PROGRAM_MAX)
        return ERR_OUT_OF_RANGE;   /* no approval was ever for such a file */
    char path[SH_PATH_MAX];
    table_path(hash_hex, path);
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;   /* ERR_NOT_FOUND: an approval from before tables */
    struct sh_ptable_head h;
    uint64_t bytes = 0;
    size_t got = 0;
    st = file_stat(&f, &bytes, NULL);
    if (st == OK)
        st = file_read(&f, 0, &h, sizeof(h), &got);
    if (st == OK && got != sizeof(h))
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = sh_ptable_check(&h, bytes, size, sha);
    if (st == OK)
        st = table_hashes(&f, h.pages, table);
    file_close(&f);
    return st;
}

/* ---- the file itself ----------------------------------------------------------------- */

/* Services no program from /data may have, whatever the owner says: with
 * volumes' channel a program gets the filesystems' own channels
 * (volumes.watch: /data unguarded, so it could approve itself); with
 * devmgr's control channel any driver's hardware; with its query channel
 * usb-bus, and through it any USB device's interfaces; with init's, every
 * service and the reboot. The build allows volumes', devmgr's control
 * channel and init's only in user/tests/ (tools/checkwants.py), and
 * a file on /data is never one of the tree's tests. Nor netstack's and the
 * resolver's reserves for the network's own services (net-sys, dns-sys): a
 * program could use them up. Nor the ports below 1024 (net-low):
 * they are where the well-known services live, so a program on them could
 * pose as one of the system's to the network (a web or a time server, the
 * ssh Jam OS will have), or take its port before the service starts; only
 * a boot-image service may have them (tools/checkwants.py). Nor
 * the file server's channel: with it a program could put any file it can
 * read on the network without the listen permission. Nor the root's debug
 * power: `debug_command` panics the machine, crashes it on purpose and runs
 * the stress test. Nor init's sessions (session): each is a whole shell, with
 * init's namespace and the shell's root powers, past every view and grant;
 * the build allows it only for a service, a port or a test
 * (tools/checkwants.py). */
static const char *const refused[] = { SVC_DEVMGR, SVC_DEVMGR_CTL, SVC_VOLUMES, SVC_INIT,
                                       SVC_NET_SYS, SVC_DNS_SYS, SVC_SERVE, SVC_NET_LISTEN_LOW,
                                       SVC_SESSION };

const char *sh_wants_refused(const struct wants *w)
{
    if (w->rights & WANT_RIGHT_DEBUG)
        return "right debug";
    for (unsigned i = 0; i < w->n; i++)
        for (unsigned k = 0; k < sizeof(refused) / sizeof(refused[0]); k++)
            if (!strncmp(w->grant[i], "/svc/", 5) && !strcmp(w->grant[i] + 5, refused[k]))
                return refused[k];
    return NULL;
}

/* sh_image_wants' reader over bytes in memory (a mapping of our own VMO). */
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

status_t sh_program_file(const char *path, handle_t *vmo, uint64_t *size, char *hex,
                         struct wants *w)
{
    handle_t raw, x;
    uint64_t n, addr = 0;
    if (!sh_root())
        return ERR_ACCESS_DENIED;   /* a shell init didn't start: no root, no VMEX */
    status_t st = file_read_vmo(path, PROGRAM_MAX, &raw, &n);
    if (st != OK)
        return st;
    st = jam_vmo_make_exec(raw, sh_root(), &x);   /* raw is ours alone, and unmapped */
    if (st != OK) {
        if (st != ERR_BAD_STATE)
            jam_handle_close(raw);   /* refused before it was taken */
        return st;
    }
    uint64_t len = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), x, 0, len ? len : PAGE_SIZE, VMAR_READ,
                      &addr);
    if (st == OK) {
        struct mem_image m = { (const uint8_t *)(uintptr_t)addr, n };
        uint8_t digest[SHA256_BYTES];
        sha256(m.bytes, n, digest);
        sha256_hex(digest, hex);
        /* Read as a run from the pages reads it: the same rules, the same
         * answer. */
        st = sh_image_wants(mem_read, &m, n, w);
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
    if (strcmp(path, f->path))
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
