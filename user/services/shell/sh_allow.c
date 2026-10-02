/* Programs from /data: the owner's approvals (`allow`) and running one
 * (docs/history/M8.6-SVC.md, "allow" and "Running a program from /data").
 *
 * The approvals are lines of /data/etc/allow, each "<sha-256>\t<path>\t
 * <what its list asks for>", written only here. Every program's /data is a
 * view that leaves etc alone (<fsview.h>), so only init and this shell,
 * which hold the whole of /data, can change the file: a program can never
 * approve another, or itself. Whoever holds the stick can edit it on
 * another computer, as they could replace the kernel (decision 11).
 *
 * Running: the file is read into a VMO only we hold, made executable
 * (vmo_make_exec, which needs RIGHT_ROOT_VMEX: this shell's root, no
 * program's) and from then on can't change; its hash and its list are
 * read from those very bytes, and it runs only if a line holds exactly
 * that path and that hash. So a program changed after it was allowed is
 * refused, and nothing can change it between the check and the start. */
#include <sha256.h>
#include <wants.h>
#include "sh.h"

#define ALLOW_FILE "/data/etc/allow"
#define ALLOW_DIR  "/data/etc"
#define ALLOW_HEAD "# Programs on /data the owner allowed to run (the shell's `allow`):\n" \
                   "# <sha-256>\\t<path>\\t<what it asked for>\n"
#define PROGRAM_MAX (64ull << 20)   /* spawn's own limit for a program read from a file */

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

/* The approvals as they are, without those `drop` says to drop, plus one
 * new line (NULL: none), written back whole. *dropped: how many went. */
static status_t rewrite(bool (*drop)(const char *path, void *ctx), void *ctx, const char *add,
                        unsigned *dropped)
{
    status_t st;
    char *all = read_allow(&st);
    if (st != OK && st != ERR_NOT_FOUND)
        return st;
    size_t cap = (all ? strlen(all) : 0) + (add ? strlen(add) : 0) + sizeof(ALLOW_HEAD) + 2;
    char *out = malloc(cap);
    if (!out) {
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
            ++*dropped;
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

status_t sh_allow_add(const char *path, const char *hash_hex, const char *text)
{
    char line[SH_PATH_MAX + 2 * SHA256_BYTES + WANTS_TEXT_MAX + 4];
    unsigned dropped;
    snprintf(line, sizeof(line), "%s\t%s\t%s", hash_hex, path, text[0] ? text : "nothing");
    return rewrite(same_path, (void *)path, line, &dropped);   /* the old line for it goes */
}

status_t sh_allow_remove(const char *name, unsigned *removed)
{
    return rewrite(named, (void *)name, NULL, removed);
}

/* ---- the file itself ----------------------------------------------------------------- */

/* Services no program from /data may have, whatever the owner says: with
 * devmgr's control channel a program gets the filesystems' own channels
 * (DEVMGR_MOUNTS: /data unguarded, so it could approve itself) and any
 * driver's hardware; with its query channel usb-bus, and through it any
 * USB device's interfaces; with init's, every service and the reboot. The
 * build allows the last two only in user/tests/ (tools/checkwants.py), and
 * a file on /data is never one of the tree's tests. The root's debug power
 * either: `debug_command` panics the machine, crashes it on purpose and
 * runs the stress test. */
static const char *const refused[] = { SVC_DEVMGR, SVC_DEVMGR_CTL, SVC_INIT };

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
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)addr;
        uint8_t digest[SHA256_BYTES];
        sha256(bytes, n, digest);
        sha256_hex(digest, hex);
        st = wants_read(bytes, n, w);
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

bool sh_allowed_program(const char *path, handle_t *vmo, uint64_t *size, struct wants *w)
{
    char hex[2 * SHA256_BYTES + 1];
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
