/* jamext: a host directory as the files of a volume (`mkimage`, `import`).
 *
 * Everything belongs to the one user. Permission bits: a directory 0755, a
 * file 0644 or, when the host's has any execute bit, 0755, whatever the
 * umask of the person who made the files (so two people get the same
 * image); --keep-modes copies the host's bits as they are. Times are the
 * image's own clock unless --keep-times. Symbolic links are kept as they
 * are (their targets are not followed, and may lead out of the image);
 * files that are one file on the host (hard links) are one file in the
 * image; sockets, devices and pipes are skipped, said on stderr. */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jamext.h"

struct seen {
    dev_t dev;
    ino_t ino;
    char *path;
};

struct import {
    struct seen *seen;
    size_t nseen, capseen;
    uint64_t count;
};

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a host directory, sorted; false if it can't be read. */
static bool host_names(const char *dir, char ***out, size_t *count)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "jamext: %s: %s\n", dir, strerror(errno));
        return false;
    }
    size_t cap = 64, n = 0;
    char **v = malloc(cap * sizeof(*v));
    struct dirent *e;
    while (v && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n == cap) {
            char **grown = realloc(v, (cap *= 2) * sizeof(*v));
            if (!grown) {
                free(v);
                v = NULL;
                break;
            }
            v = grown;
        }
        v[n] = strdup(e->d_name);
        if (!v[n]) {
            v = NULL;
            break;
        }
        n++;
    }
    closedir(d);
    if (!v) {
        fprintf(stderr, "jamext: out of memory\n");
        return false;
    }
    qsort(v, n, sizeof(*v), cmp_str);
    *out = v;
    *count = n;
    return true;
}

static void free_names(char **v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 2);
    if (!s)
        return NULL;
    memcpy(s, a, la);
    size_t n = la;
    if (la && a[la - 1] != '/')
        s[n++] = '/';
    memcpy(s + n, b, lb + 1);
    return s;
}

static uint16_t pick_mode(const struct jx *x, const struct stat *st)
{
    if (x->o.keep_modes)
        return (uint16_t)(st->st_mode & 0777);
    if (S_ISDIR(st->st_mode))
        return 0755;
    return (st->st_mode & 0111) ? 0755 : 0644;
}

static status_t import_dir(struct jx *x, struct import *im, const char *host, const char *img)
{
    char **names;
    size_t n;
    if (!host_names(host, &names, &n))
        return ERR_IO;
    status_t st = OK;
    for (size_t i = 0; i < n && st == OK; i++) {
        char *hp = join(host, names[i]), *ip = join(img, names[i]);
        struct stat s;
        if (!hp || !ip) {
            st = ERR_NO_MEMORY;
        } else if (lstat(hp, &s) != 0) {
            fprintf(stderr, "jamext: %s: %s\n", hp, strerror(errno));
            st = ERR_IO;
        } else {
            int64_t keep_now = x->o.now;
            if (x->o.keep_times)
                x->o.now = (int64_t)s.st_mtime;
            if (S_ISDIR(s.st_mode)) {
                st = jx_mkdir(x, ip, pick_mode(x, &s), false);
                if (st == OK)
                    st = import_dir(x, im, hp, ip);
            } else if (S_ISLNK(s.st_mode)) {
                char target[4097];
                ssize_t len = readlink(hp, target, sizeof(target) - 1);
                if (len < 0) {
                    fprintf(stderr, "jamext: %s: %s\n", hp, strerror(errno));
                    st = ERR_IO;
                } else {
                    target[len] = '\0';
                    st = jx_ln(x, target, ip, true);
                }
            } else if (S_ISREG(s.st_mode)) {
                struct seen *same = NULL;
                if (s.st_nlink > 1)
                    for (size_t k = 0; k < im->nseen; k++)
                        if (im->seen[k].dev == s.st_dev && im->seen[k].ino == s.st_ino)
                            same = &im->seen[k];
                if (same) {
                    st = jx_ln(x, same->path, ip, false);
                } else {
                    st = jx_put(x, hp, ip, pick_mode(x, &s));
                    if (st == OK && s.st_nlink > 1) {
                        if (im->nseen == im->capseen) {
                            size_t cap = im->capseen ? im->capseen * 2 : 16;
                            struct seen *grown = realloc(im->seen, cap * sizeof(*grown));
                            if (!grown) {
                                st = ERR_NO_MEMORY;
                            } else {
                                im->seen = grown;
                                im->capseen = cap;
                            }
                        }
                        if (st == OK)
                            im->seen[im->nseen++] = (struct seen){ s.st_dev, s.st_ino,
                                                                   strdup(ip) };
                    }
                }
            } else {
                fprintf(stderr, "jamext: %s: skipped (not a file, directory or link)\n", hp);
                x->o.now = keep_now;
                free(hp);
                free(ip);
                continue;
            }
            x->o.now = keep_now;
            if (st == OK)
                im->count++;
            else
                fprintf(stderr, "jamext: %s: %s\n", hp, jx_why(st));
        }
        free(hp);
        free(ip);
    }
    free_names(names, n);
    return st;
}

status_t jx_import(struct jx *x, const char *host, const char *path, uint64_t *count)
{
    struct stat s;
    if (stat(host, &s) != 0 || !S_ISDIR(s.st_mode)) {
        fprintf(stderr, "jamext: %s: not a directory\n", host);
        return ERR_NOT_DIR;
    }
    struct import im = { 0 };
    status_t st = jx_mkdir(x, path, 0755, true);
    if (st == OK)
        st = import_dir(x, &im, host, path);
    for (size_t i = 0; i < im.nseen; i++)
        free(im.seen[i].path);
    free(im.seen);
    if (count)
        *count = im.count;
    return st;
}

/* ---- how big a volume ----------------------------------------------------------------- */

static void measure(const char *host, uint64_t *data, uint64_t *names)
{
    char **v;
    size_t n;
    if (!host_names(host, &v, &n))
        return;
    for (size_t i = 0; i < n; i++) {
        char *hp = join(host, v[i]);
        struct stat s;
        if (hp && lstat(hp, &s) == 0) {
            ++*names;
            if (S_ISDIR(s.st_mode)) {
                *data += 4096;
                measure(hp, data, names);
            } else if (S_ISREG(s.st_mode)) {
                *data += ((uint64_t)s.st_size + 4095) & ~(uint64_t)4095;
            } else if (S_ISLNK(s.st_mode)) {
                *data += (uint64_t)s.st_size >= 60 ? 4096 : 0;
            }
        }
        free(hp);
    }
    free_names(v, n);
}

bool jx_size_for(const char *host, uint64_t *out, uint32_t *inodes)
{
    struct stat s;
    if (stat(host, &s) != 0 || !S_ISDIR(s.st_mode)) {
        fprintf(stderr, "jamext: %s: not a directory\n", host);
        return false;
    }
    uint64_t data = 0, names = 0;
    measure(host, &data, &names);
    /* The data, a tenth for the trees and tables, the journal (4 MiB or
     * more), the first groups' metadata and lost+found, and some room. */
    uint64_t bytes = data + data / 10 + names * 512 + (24u << 20);
    *out = (bytes + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    *inodes = (uint32_t)(names + names / 8 + 64);
    return true;
}
