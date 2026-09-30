/* Paths and files for the shell's commands: a thin layer over libos's
 * namespace (<os.h> "files"), which holds the mounts init gave the shell
 * (/boot, and /data and /esp once the stick's filesystems are up) and
 * takes the ones init sends later.
 *
 * What the shell adds is its own: a current directory, "~", and paths
 * relative to it. Those are resolved here as text ("cd /data; cd .." is
 * "/"), so what reaches libos is always absolute and free of "..". */
#include "sh.h"

static char cwd[SH_PATH_MAX] = "/boot";
static void *held;   /* the file sh_read last handed out */

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

bool sh_join(const char *dir, const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    return n > 0 && (size_t)n < cap;
}

const char *sh_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1])
            base = p + 1;
    return base;
}

const char *sh_bootfs_name(const char *abs)
{
    return !strncmp(abs, "/boot/", 6) && abs[6] ? abs + 6 : NULL;
}

bool sh_dest(const char *from_abs, const char *to, char *out, size_t cap)
{
    char abs[SH_PATH_MAX];
    bool dir;
    uint64_t size;
    if (!sh_resolve(to, abs, sizeof(abs)))
        return false;
    if (sh_stat(abs, &dir, &size) == OK && dir)
        return sh_join(abs, sh_basename(from_abs), out, cap);
    if (strlen(abs) + 1 > cap)
        return false;
    memcpy(out, abs, strlen(abs) + 1);
    return true;
}

bool sh_is_mount(const char *abs)
{
    return !strchr(abs + 1, '/');
}

const char *sh_why(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:      return "no such file or directory";
    case ERR_ALREADY_EXISTS: return "it exists already";
    case ERR_ACCESS_DENIED:  return "read-only";
    case ERR_NO_SPACE:       return "no space left";
    case ERR_PEER_CLOSED:    return "its filesystem is gone";
    case ERR_INVALID_ARGS:   return "not a valid name";
    case ERR_WRONG_TYPE:     return "is a directory";
    case ERR_BAD_STATE:      return "in use, or a directory with entries";
    case ERR_CANCELED:       return "interrupted";
    default:                 return status_str(st);
    }
}

status_t sh_stat(const char *abs, bool *dir, uint64_t *size)
{
    return fs_stat(abs, size, dir, NULL);
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
    bool dir;
    uint64_t size;
    if (sh_stat(abs, &dir, &size) != OK || !dir)
        return -1;
    int n = 0;
    for (uint32_t i = 0; n < cap; i++) {
        struct fs_entry e;
        if (fs_readdir(abs, i, &e) != OK)
            break;   /* past the last, or the mount is gone: what we have */
        memcpy(out[n].name, e.name, sizeof(out[n].name));
        out[n].dir = e.is_dir;
        out[n].size = e.size;
        n++;
    }
    sort_ents(out, n);
    return n;
}

status_t sh_read(const char *abs, const void **data, uint64_t *size)
{
    struct jfile f;
    status_t st = file_open(abs, FS_READ, &f);
    if (st != OK)
        return st;
    uint64_t n = 0;
    size_t got = 0;
    st = file_stat(&f, &n, NULL);
    if (st == OK && n > SH_FILE_MAX)
        st = ERR_OUT_OF_RANGE;
    char *buf = st == OK ? malloc(n + 1) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    if (st == OK)
        st = file_read(&f, 0, buf, n, &got);
    file_close(&f);
    if (st != OK) {
        free(buf);
        return st;
    }
    buf[got] = '\0';
    free(held);
    held = buf;
    *data = buf;
    *size = got;
    return OK;
}

status_t sh_write(const char *abs, const void *data, size_t n, uint32_t how)
{
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(abs, FS_WRITE | FS_CREATE | how, &f);
    if (st != OK)
        return st;
    if (n)
        st = file_write(&f, 0, data, n, &done);
    if (st == OK && done < n)
        st = ERR_NO_SPACE;
    file_close(&f);
    return st;
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
