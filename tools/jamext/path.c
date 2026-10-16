/* jamext: a path walked from the volume's root. */
#include <stdlib.h>
#include <string.h>
#include "jamext.h"

#define MAX_LINKS 40
#define PATH_MAX_BYTES 8192

/* The next component of *rest (skipping slashes), copied to name; *last
 * says nothing follows it. false at the end. */
static bool next_component(const char **rest, char *name, size_t cap, bool *last)
{
    const char *p = *rest;
    while (*p == '/')
        p++;
    if (!*p)
        return false;
    const char *e = p;
    while (*e && *e != '/')
        e++;
    size_t n = (size_t)(e - p);
    if (n >= cap) {
        *rest = e;
        name[0] = '\0';
        return true;   /* too long: the caller finds the empty name */
    }
    memcpy(name, p, n);
    name[n] = '\0';
    const char *q = e;
    while (*q == '/')
        q++;
    *last = !*q;
    *rest = e;
    return true;
}

static status_t read_link(struct jx *x, struct ext_inode *in, char *buf, size_t cap)
{
    size_t n = 0;
    status_t st = ext_readlink(&x->fs, in, buf, cap - 1, &n);
    if (st == OK)
        buf[n] = '\0';
    return st;
}

status_t jx_locate(struct jx *x, const char *path, bool follow_last, struct jx_loc *loc)
{
    struct ext_fs *fs = &x->fs;
    char *work = malloc(PATH_MAX_BYTES);
    if (!work)
        return ERR_NO_MEMORY;
    if (strlen(path) >= PATH_MAX_BYTES) {
        free(work);
        return ERR_NAME_TOO_LONG;
    }
    strcpy(work, path);
    const char *rest = work;
    struct ext_inode root, cur;
    status_t st = ext_iget(fs, EXT_ROOT_INO, 0, &root);
    if (st != OK) {
        free(work);
        return st;
    }
    cur = root;
    memset(loc, 0, sizeof(*loc));
    loc->dir = root;
    loc->in = root;
    loc->found = true;
    unsigned links = 0;
    char name[EXT_NAME_MAX + 2];
    bool last = false;
    char *expanded = NULL;              /* the current work string when a link replaced it */
    bool fresh_link = false;            /* a link was just followed, nothing walked since */
    for (;;) {
        if (!next_component(&rest, name, sizeof(name), &last))
            break;
        fresh_link = false;
        if (!name[0]) {
            st = ERR_NAME_TOO_LONG;
            goto out;
        }
        if (!strcmp(name, ".")) {
            if (last) {
                loc->dir = cur;
                loc->in = cur;
                loc->found = true;
                loc->name_len = 0;
            }
            continue;
        }
        if (ext_itype(&cur) != EXT_FT_DIR) {
            st = ERR_NOT_DIR;
            goto out;
        }
        struct ext_dentry d;
        if (!strcmp(name, "..")) {
            st = ext_dir_lookup(fs, &cur, "..", 2, &d);
        } else {
            struct ext_name nm = { .s = name, .len = (uint32_t)strlen(name) };
            st = ext_lookup(fs, &cur, &nm, &d);
        }
        if (st == ERR_NOT_FOUND) {
            if (!last) {
                st = ERR_NOT_FOUND;
                goto out;
            }
            loc->dir = cur;
            memcpy(loc->name, name, strlen(name) + 1);
            loc->name_len = (uint32_t)strlen(name);
            loc->found = false;
            st = OK;
            goto out;
        }
        if (st != OK)
            goto out;
        struct ext_inode in;
        st = ext_iget(fs, d.ino, 0, &in);
        if (st != OK)
            goto out;
        if (ext_itype(&in) == EXT_FT_SYMLINK && (!last || follow_last)) {
            if (++links > MAX_LINKS) {
                st = ERR_LINK_LOOP;
                goto out;
            }
            char target[4097];
            st = read_link(x, &in, target, sizeof(target));
            if (st != OK)
                goto out;
            /* The target, then what is left of the path. */
            char *joined = malloc(PATH_MAX_BYTES);
            if (!joined) {
                st = ERR_NO_MEMORY;
                goto out;
            }
            if (strlen(target) + strlen(rest) + 2 >= PATH_MAX_BYTES) {
                free(joined);
                st = ERR_NAME_TOO_LONG;
                goto out;
            }
            sprintf(joined, "%s/%s", target, rest);
            free(expanded);
            expanded = joined;
            rest = joined;
            if (target[0] == '/')
                cur = root;
            fresh_link = true;
            continue;
        }
        loc->dir = cur;
        loc->in = in;
        loc->found = true;
        memcpy(loc->name, name, strlen(name) + 1);
        loc->name_len = (uint32_t)strlen(name);
        if (!last)
            cur = in;
    }
    if (fresh_link) {                   /* the link led to a directory and no name after it */
        loc->dir = cur;
        loc->in = cur;
        loc->found = true;
        loc->name_len = 0;
    }
    st = OK;
out:
    free(expanded);
    free(work);
    return st;
}
