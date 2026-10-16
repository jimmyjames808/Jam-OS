/* bin/pkg's file calls (pkg.h). Every change to the packages' prefix goes
 * through a function here, and each asks pk_cut first: a test sets
 * `cut` to take the power away before the n-th change and sees what a
 * cut at that point leaves for the settle to find. A change is one file
 * call, so a two-step one (a rename over an existing file, which FAT
 * can't do) is two changes, with a cut point between them, as on the
 * disk.
 *
 * The reads (pk_read, pk_hash_file, pk_exists) are not changes and are
 * never cut. */
#include <stdarg.h>
#include <sha256.h>
#include "pkg.h"

void pk_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void pk_path(const struct pk_env *e, struct pkg_str rel, const char *suffix, char *out)
{
    snprintf(out, PK_PATH, "%s/%.*s%s", e->prefix, (int)rel.n, rel.p, suffix);
}

void pk_db_path(const struct pk_env *e, const char *name, const char *suffix, char *out)
{
    snprintf(out, PK_PATH, "%s/db/%s%s", e->prefix, name, suffix);
}

int pk_cut(struct pk_env *e, const char *what, bool tearable)
{
    if (e->dead)
        return PK_DIE;
    e->ops++;
    int c = e->cut ? e->cut(e->cut_ctx, e->ops, what) : 0;
    if (!c)
        return PK_GO;
    e->dead = true;
    return c == 2 && tearable ? PK_TEAR : PK_DIE;
}

status_t pk_write(struct pk_env *e, const char *path, const void *data, size_t n)
{
    int c = pk_cut(e, "write", true);
    if (c == PK_DIE)
        return ERR_IO;
    struct jfile f;
    size_t done = 0, m = c == PK_TEAR ? n / 2 : n;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    st = m ? file_write(&f, 0, data, m, &done) : OK;
    if (st == OK && done != m)
        st = ERR_IO;
    if (st == OK && c == PK_GO)
        st = file_sync(&f);
    file_close(&f);
    return c == PK_TEAR ? ERR_IO : st;
}

status_t pk_remove(struct pk_env *e, const char *path)
{
    if (pk_cut(e, "remove", false) != PK_GO)
        return ERR_IO;
    status_t st = fs_unlink(path);
    return st == ERR_NOT_FOUND ? OK : st;
}

status_t pk_rename(struct pk_env *e, const char *from, const char *to)
{
    if (pk_cut(e, "rename", false) != PK_GO)
        return ERR_IO;
    return fs_rename(from, to);
}

status_t pk_move(struct pk_env *e, const char *from, const char *to)
{
    status_t st = pk_remove(e, to);
    return st == OK ? pk_rename(e, from, to) : st;
}

status_t pk_mkdirs(struct pk_env *e, const char *file)
{
    char dir[PK_PATH];
    size_t skip = strlen(e->prefix) + 1;
    if (strlen(file) <= skip)
        return OK;
    if (fs_stat(e->prefix, NULL, NULL, NULL) != OK) {   /* the prefix itself, the first time */
        if (pk_cut(e, "mkdir", false) != PK_GO)
            return ERR_IO;
        status_t st = fs_mkdir(e->prefix);
        if (st != OK && st != ERR_ALREADY_EXISTS)
            return st;
    }
    for (const char *s = strchr(file + skip, '/'); s; s = strchr(s + 1, '/')) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(s - file), file);
        if (fs_stat(dir, NULL, NULL, NULL) == OK)
            continue;
        if (pk_cut(e, "mkdir", false) != PK_GO)
            return ERR_IO;
        status_t st = fs_mkdir(dir);
        if (st != OK && st != ERR_ALREADY_EXISTS)
            return st;
    }
    return OK;
}

status_t pk_flush(struct pk_env *e)
{
    return e->dead ? ERR_IO : fs_sync(e->prefix);
}

status_t pk_read(const char *path, uint64_t max, uint8_t **data, uint64_t *n)
{
    struct jfile f;
    uint64_t size = 0;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    st = file_stat(&f, &size, NULL);
    if (st == OK && size > max)
        st = ERR_OUT_OF_RANGE;
    uint8_t *buf = st == OK ? malloc(size + 1) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    for (uint64_t at = 0; st == OK && at < size;) {
        size_t got = 0;
        st = file_read(&f, at, buf + at, (size_t)(size - at), &got);
        if (st == OK && !got)
            st = ERR_IO;   /* shorter than it said */
        at += got;
    }
    file_close(&f);
    if (st != OK) {
        free(buf);
        return st;
    }
    buf[size] = '\0';
    *data = buf;
    *n = size;
    return OK;
}

bool pk_exists(const char *path)
{
    return fs_stat(path, NULL, NULL, NULL) == OK;
}

#define HASH_CHUNK (64u * 1024)

status_t pk_hash_file(const char *path, uint64_t *size, uint8_t sha[SHA256_BYTES])
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    uint8_t *buf = malloc(HASH_CHUNK);
    if (!buf) {
        file_close(&f);
        return ERR_NO_MEMORY;
    }
    struct sha256 h;
    sha256_init(&h);
    uint64_t at = 0;
    for (;;) {
        size_t got = 0;
        st = file_read(&f, at, buf, HASH_CHUNK, &got);
        if (st != OK || !got)
            break;
        sha256_add(&h, buf, got);
        at += got;
    }
    file_close(&f);
    free(buf);
    if (st != OK)
        return st;
    sha256_done(&h, sha);
    *size = at;
    return OK;
}

bool pk_ends_fold(struct pkg_str s, const char *suffix)
{
    size_t k = strlen(suffix);
    if (s.n < k)
        return false;
    for (size_t i = 0; i < k; i++) {
        char c = s.p[s.n - k + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

bool pk_name_reserved(struct pkg_str name)
{
    return pkg_str_eq(name, "serial") || pkg_str_eq(name, "journal") ||
           pk_ends_fold(name, ".new") || pk_ends_fold(name, ".sum");
}
