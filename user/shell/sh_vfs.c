/* Paths for the shell's file commands (ls cd cat find ...).
 *
 * One tree: "/" holds mount points, each a small set of operations. Today
 * there is one, /boot = the bootfs image (bin/..., drv/..., init.cfg; files
 * only, read-only, directories implied by the '/' in the names). M8's
 * filesystem mounts /data beside it: a new entry in `mounts` with the same
 * three operations (stat, readdir, read) over its channel protocol. */
#include <jam/bootfs.h>
#include "sh.h"

static char cwd[SH_PATH_MAX] = "/boot";

const char *sh_cwd(void)
{
    return cwd;
}

bool sh_resolve(const char *in, char *out, size_t cap)
{
    char tmp[SH_PATH_MAX * 2];
    const char *home = sh_getvar("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home)
        snprintf(tmp, sizeof(tmp), "%s%s", home, in + 1);
    else if (in[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", in);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
    /* Components onto out, handling . and .. */
    size_t n = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t l = (size_t)(e - p);
        if (!l)
            break;
        if (l == 1 && p[0] == '.') {
            /* nothing */
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;   /* the slash */
        } else {
            if (n + 1 + l + 1 > cap)
                return false;
            out[n++] = '/';
            memcpy(out + n, p, l);
            n += l;
        }
        p = e;
    }
    if (!n)
        out[n++] = '/';
    out[n] = '\0';
    return true;
}

/* ---- /boot: the bootfs ----------------------------------------------------------- */

static const struct bootfs_entry *boot_entries(uint32_t *count)
{
    const struct bootfs_view *fs;
    if (bootfs_default(&fs) != OK) {
        *count = 0;
        return NULL;
    }
    *count = fs->count;
    return (const struct bootfs_entry *)(fs->base + sizeof(struct bootfs_header));
}

static status_t boot_stat(const char *rel, bool *dir, uint64_t *size)
{
    uint32_t n;
    const struct bootfs_entry *e = boot_entries(&n);
    size_t rl = strlen(rel);
    if (!rl) {
        *dir = true;
        *size = 0;
        return OK;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (strnlen(e[i].name, BOOTFS_NAME_MAX) == BOOTFS_NAME_MAX)
            continue;
        if (!strcmp(e[i].name, rel)) {
            *dir = false;
            *size = e[i].size;
            return OK;
        }
        if (!strncmp(e[i].name, rel, rl) && e[i].name[rl] == '/') {
            *dir = true;
            *size = 0;
            return OK;
        }
    }
    return ERR_NOT_FOUND;
}

static void add_ent(struct sh_dirent *out, int *n, int cap, const char *name, size_t l, bool dir,
                    uint64_t size)
{
    if (l >= sizeof(out[0].name))
        return;
    for (int i = 0; i < *n; i++)
        if (strlen(out[i].name) == l && !strncmp(out[i].name, name, l))
            return;
    if (*n >= cap)
        return;
    memcpy(out[*n].name, name, l);
    out[*n].name[l] = '\0';
    out[*n].dir = dir;
    out[*n].size = size;
    (*n)++;
}

static int boot_readdir(const char *rel, struct sh_dirent *out, int cap)
{
    bool dir;
    uint64_t size;
    if (boot_stat(rel, &dir, &size) != OK || !dir)
        return -1;
    uint32_t count;
    const struct bootfs_entry *e = boot_entries(&count);
    size_t rl = strlen(rel);
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = e[i].name;
        if (strnlen(name, BOOTFS_NAME_MAX) == BOOTFS_NAME_MAX)
            continue;
        if (rl) {
            if (strncmp(name, rel, rl) || name[rl] != '/')
                continue;
            name += rl + 1;
        }
        const char *slash = strchr(name, '/');
        if (slash)
            add_ent(out, &n, cap, name, (size_t)(slash - name), true, 0);
        else
            add_ent(out, &n, cap, name, strlen(name), false, e[i].size);
    }
    return n;
}

static status_t boot_read(const char *rel, const void **data, uint64_t *size)
{
    const struct bootfs_view *fs;
    status_t st = bootfs_default(&fs);
    return st == OK ? bootfs_lookup(fs, rel, data, size) : st;
}

/* ---- the mount table -------------------------------------------------------------- */

struct mount {
    const char *path;   /* "/boot" */
    status_t  (*stat)(const char *rel, bool *dir, uint64_t *size);
    int       (*readdir)(const char *rel, struct sh_dirent *out, int cap);
    status_t  (*read)(const char *rel, const void **data, uint64_t *size);
};

static const struct mount mounts[] = {
    { "/boot", boot_stat, boot_readdir, boot_read },
};
#define NMOUNTS (sizeof(mounts) / sizeof(mounts[0]))

/* The mount abs is on and the path inside it ("" for its root). */
static const struct mount *mount_of(const char *abs, const char **rel)
{
    for (size_t i = 0; i < NMOUNTS; i++) {
        size_t l = strlen(mounts[i].path);
        if (!strncmp(abs, mounts[i].path, l) && (abs[l] == '/' || !abs[l])) {
            *rel = abs[l] ? abs + l + 1 : abs + l;
            return &mounts[i];
        }
    }
    return NULL;
}

const char *sh_bootfs_name(const char *abs)
{
    const char *rel;
    const struct mount *m = mount_of(abs, &rel);
    return m == &mounts[0] ? rel : NULL;
}

status_t sh_stat(const char *abs, bool *dir, uint64_t *size)
{
    if (!strcmp(abs, "/")) {
        *dir = true;
        *size = 0;
        return OK;
    }
    const char *rel;
    const struct mount *m = mount_of(abs, &rel);
    return m ? m->stat(rel, dir, size) : ERR_NOT_FOUND;
}

static void sort_ents(struct sh_dirent *e, int n)
{
    for (int i = 1; i < n; i++) {
        struct sh_dirent t = e[i];
        int j = i;
        while (j > 0 && strcmp(e[j - 1].name, t.name) > 0) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = t;
    }
}

int sh_readdir(const char *abs, struct sh_dirent *out, int cap)
{
    int n;
    if (!strcmp(abs, "/")) {
        n = 0;
        for (size_t i = 0; i < NMOUNTS; i++)
            add_ent(out, &n, cap, mounts[i].path + 1, strlen(mounts[i].path + 1), true, 0);
    } else {
        const char *rel;
        const struct mount *m = mount_of(abs, &rel);
        n = m ? m->readdir(rel, out, cap) : -1;
    }
    if (n > 0)
        sort_ents(out, n);
    return n;
}

status_t sh_read(const char *abs, const void **data, uint64_t *size)
{
    const char *rel;
    const struct mount *m = mount_of(abs, &rel);
    return m && *rel ? m->read(rel, data, size) : ERR_NOT_FOUND;
}

bool sh_chdir(const char *path)
{
    char abs[SH_PATH_MAX];
    bool dir;
    uint64_t size;
    if (!sh_resolve(path, abs, sizeof(abs)) || sh_stat(abs, &dir, &size) != OK || !dir)
        return false;
    memcpy(cwd, abs, strlen(abs) + 1);
    return true;
}
