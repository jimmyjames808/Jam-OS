/* jamext: the commands' work on an open volume. Each change is its own
 * journal transaction (or a megabyte of a file's data), so a tool killed
 * between two leaves a volume the next mount replays whole. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "jamext.h"

struct ext_time jx_time(const struct jx *x)
{
    return (struct ext_time){ .sec = x->o.now, .nsec = 0 };
}

struct ext_attrs jx_attrs(struct jx *x, uint16_t mode)
{
    return (struct ext_attrs){ .mode = mode, .uid = JX_USER, .gid = JX_USER,
                               .generation = jx_random(x), .now = jx_time(x) };
}

/* ---- names ------------------------------------------------------------------------------- */

static struct ext_name loc_name(const struct jx_loc *loc)
{
    return (struct ext_name){ .s = loc->name, .len = loc->name_len };
}

/* The directories above path's last name, made (mode 0755). */
static status_t make_parents(struct jx *x, const char *path)
{
    char *dir = strdup(path);
    if (!dir)
        return ERR_NO_MEMORY;
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = '\0';
    char *slash = strrchr(dir, '/');
    status_t st = OK;
    if (slash && slash != dir) {
        *slash = '\0';
        st = jx_mkdir(x, dir, 0755, true);
    }
    free(dir);
    return st;
}

/* One name made under loc->dir: mode includes the type. */
static status_t create_at(struct jx *x, struct jx_loc *loc, uint16_t mode, struct ext_inode *out)
{
    struct ext_attrs a = jx_attrs(x, mode);
    struct ext_name n = loc_name(loc);
    struct ext_inode tmp;
    if (!jx_begin(x))
        return ERR_BAD_STATE;
    status_t st = ext_create(&x->fs, &loc->dir, &n, &a, out ? out : &tmp);
    if (st != OK) {
        jx_abort(x);
        return st;
    }
    return jx_commit(x) ? OK : ERR_IO;
}

status_t jx_mkdir(struct jx *x, const char *path, uint16_t mode, bool parents)
{
    struct jx_loc loc;
    if (!parents) {
        status_t st = jx_locate(x, path, false, &loc);
        if (st != OK)
            return st;
        if (loc.found)
            return ERR_ALREADY_EXISTS;
        return create_at(x, &loc, (uint16_t)(EXT_S_IFDIR | (mode & 07777)), NULL);
    }
    /* Each prefix of the path in turn. */
    size_t len = strlen(path);
    char *p = malloc(len + 2);
    if (!p)
        return ERR_NO_MEMORY;
    status_t st = OK;
    for (size_t i = 0; i <= len && st == OK; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        if (i == 0)
            continue;            /* the root */
        memcpy(p, path, i);
        p[i] = '\0';
        if (i > 0 && p[i - 1] == '/')
            continue;
        st = jx_locate(x, p, true, &loc);
        if (st != OK)
            break;
        if (loc.found) {
            if (ext_itype(&loc.in) != EXT_FT_DIR)
                st = ERR_NOT_DIR;
            continue;
        }
        st = create_at(x, &loc, (uint16_t)(EXT_S_IFDIR | (mode & 07777)), NULL);
    }
    free(p);
    return st;
}

/* ---- removing ---------------------------------------------------------------------------- */

status_t jx_drop(struct jx *x, uint32_t ino)
{
    for (;;) {
        struct ext_time now = jx_time(x);
        if (!jx_begin(x))
            return ERR_BAD_STATE;
        status_t st = ext_orphan_drop(&x->fs, ino, &now);
        if (st != OK && st != ERR_SHOULD_WAIT) {
            jx_abort(x);
            return st;
        }
        if (!jx_commit(x))
            return ERR_IO;
        if (st == OK)
            return OK;
    }
}

/* Remove the name in loc (a directory only when `dir`), and what it leaves. */
static status_t unlink_at(struct jx *x, struct jx_loc *loc, bool dir)
{
    struct ext_name n = loc_name(loc);
    struct ext_time now = jx_time(x);
    uint32_t orphan = 0;
    if (!jx_begin(x))
        return ERR_BAD_STATE;
    status_t st = ext_unlink(&x->fs, &loc->dir, &n, dir ? EXT_UNLINK_DIR : 0, &now, &orphan);
    if (st != OK) {
        jx_abort(x);
        return st;
    }
    if (!jx_commit(x))
        return ERR_IO;
    return orphan ? jx_drop(x, orphan) : OK;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in directory dir (not "." and ".."), sorted; free with free_names. */
static status_t list_names(struct jx *x, struct ext_inode *dir, char ***out, size_t *count)
{
    size_t cap = 64, n = 0;
    char **v = malloc(cap * sizeof(*v));
    if (!v)
        return ERR_NO_MEMORY;
    struct ext_dentry d;
    status_t st = OK;
    for (uint64_t pos = 0;;) {
        st = ext_dir_next(&x->fs, dir, &pos, &d);
        if (st != OK || !d.ino)
            break;
        if (!strcmp(d.name, ".") || !strcmp(d.name, ".."))
            continue;
        if (n == cap) {
            char **grown = realloc(v, (cap *= 2) * sizeof(*v));
            if (!grown) {
                st = ERR_NO_MEMORY;
                break;
            }
            v = grown;
        }
        v[n] = strdup(d.name);
        if (!v[n]) {
            st = ERR_NO_MEMORY;
            break;
        }
        n++;
    }
    if (st != OK) {
        for (size_t i = 0; i < n; i++)
            free(v[i]);
        free(v);
        return st;
    }
    qsort(v, n, sizeof(*v), cmp_str);
    *out = v;
    *count = n;
    return OK;
}

static void free_names(char **v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static status_t remove_tree(struct jx *x, struct jx_loc *loc)
{
    if (ext_itype(&loc->in) != EXT_FT_DIR)
        return unlink_at(x, loc, false);
    char **names;
    size_t n;
    status_t st = list_names(x, &loc->in, &names, &n);
    if (st != OK)
        return st;
    for (size_t i = 0; i < n && st == OK; i++) {
        struct jx_loc child = *loc;
        struct ext_name nm = { .s = names[i], .len = (uint32_t)strlen(names[i]) };
        struct ext_dentry d;
        st = ext_lookup(&x->fs, &loc->in, &nm, &d);
        if (st == OK)
            st = ext_iget(&x->fs, d.ino, 0, &child.in);
        if (st != OK)
            break;
        child.dir = loc->in;
        memcpy(child.name, names[i], nm.len + 1);
        child.name_len = nm.len;
        child.found = true;
        st = remove_tree(x, &child);
        loc->in = child.dir;      /* the directory, current */
    }
    free_names(names, n);
    if (st != OK)
        return st;
    return unlink_at(x, loc, true);
}

status_t jx_rm(struct jx *x, const char *path, bool recursive)
{
    struct jx_loc loc;
    status_t st = jx_locate(x, path, false, &loc);
    if (st != OK)
        return st;
    if (!loc.found)
        return ERR_NOT_FOUND;
    if (!loc.name_len)
        return ERR_INVALID_ARGS;       /* the root */
    if (ext_itype(&loc.in) == EXT_FT_DIR)
        return recursive ? remove_tree(x, &loc) : ERR_IS_DIR;
    return unlink_at(x, &loc, false);
}

/* ---- file data --------------------------------------------------------------------------- */

static bool zero_block(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* Fill buf with up to n bytes from fd; the number read. */
static ssize_t read_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

status_t jx_write_fd(struct jx *x, struct ext_inode *in, int fd, uint64_t size)
{
    struct ext_fs *fs = &x->fs;
    uint32_t bs = fs->block_size;
    uint8_t *buf = malloc(JX_CHUNK);
    if (!buf)
        return ERR_NO_MEMORY;
    status_t st = OK;
    uint64_t off = 0;
    while (off < size && st == OK) {
        size_t want = size - off < JX_CHUNK ? (size_t)(size - off) : JX_CHUNK;
        ssize_t got = read_full(fd, buf, want);
        if (got < 0) {
            fprintf(stderr, "jamext: reading the file: %s\n", strerror(errno));
            st = ERR_IO;
            break;
        }
        if (got == 0)
            break;                  /* shorter than it was: the size is what was read */
        if (!jx_begin(x)) {
            st = ERR_BAD_STATE;
            break;
        }
        for (size_t i = 0; i < (size_t)got && st == OK;) {
            size_t n = (size_t)got - i < bs ? (size_t)got - i : bs;
            if (zero_block(buf + i, n)) {   /* a hole */
                i += bs;
                continue;
            }
            size_t j = i + bs;               /* the run of blocks that are not */
            while (j < (size_t)got) {
                size_t m = (size_t)got - j < bs ? (size_t)got - j : bs;
                if (zero_block(buf + j, m))
                    break;
                j += bs;
            }
            size_t len = (j < (size_t)got ? j : (size_t)got) - i, done = 0;
            st = ext_write(fs, in, off + i, buf + i, len, &done);
            if (st == OK && done < len)
                st = ERR_NO_SPACE;
            i = j;
        }
        if (st != OK) {
            jx_abort(x);
            break;
        }
        if (!jx_commit(x)) {
            st = ERR_IO;
            break;
        }
        off += (uint64_t)got;
        if ((size_t)got < want)
            break;
    }
    free(buf);
    if (st != OK)
        return st;
    if (ext_isize(in) < off) {          /* it ends in a hole */
        bool done = false;
        if (!jx_begin(x))
            return ERR_BAD_STATE;
        st = ext_truncate(fs, in, off, &done);
        if (st != OK || !done) {
            jx_abort(x);
            return st != OK ? st : ERR_IO;
        }
        if (!jx_commit(x))
            return ERR_IO;
    }
    return OK;
}

/* The file's modification and change times, now. */
static status_t stamp(struct jx *x, struct ext_inode *in)
{
    struct ext_time t = jx_time(x);
    if (!jx_begin(x))
        return ERR_BAD_STATE;
    status_t st = ext_itime_set(in, EXT_MTIME, &t);
    if (st == OK)
        st = ext_itime_set(in, EXT_CTIME, &t);
    if (st == OK)
        st = ext_iput(&x->fs, in);
    if (st != OK) {
        jx_abort(x);
        return st;
    }
    return jx_commit(x) ? OK : ERR_IO;
}

status_t jx_put(struct jx *x, const char *host, const char *path, uint16_t mode)
{
    int fd = 0;
    FILE *tmp = NULL;
    uint64_t size = 0;
    if (strcmp(host, "-")) {
        fd = open(host, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "jamext: %s: %s\n", host, strerror(errno));
            return ERR_NOT_FOUND;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "jamext: %s: not a regular file\n", host);
            close(fd);
            return ERR_INVALID_ARGS;
        }
        size = (uint64_t)st.st_size;
    } else {                              /* standard input: kept in a temporary file */
        tmp = tmpfile();
        if (!tmp)
            return ERR_NO_SPACE;
        char b[65536];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), stdin)) > 0)
            if (fwrite(b, 1, n, tmp) != n) {
                fclose(tmp);
                return ERR_NO_SPACE;
            }
        fflush(tmp);
        fd = fileno(tmp);
        size = (uint64_t)ftello(tmp);
        lseek(fd, 0, SEEK_SET);
    }
    struct jx_loc loc;
    status_t st = make_parents(x, path);
    if (st == OK)
        st = jx_locate(x, path, false, &loc);
    if (st == OK && !loc.name_len)
        st = ERR_IS_DIR;
    if (st == OK && loc.found) {
        if (ext_itype(&loc.in) == EXT_FT_DIR)
            st = ERR_IS_DIR;
        else
            st = unlink_at(x, &loc, false);     /* replaced */
    }
    struct ext_inode in;
    bool made = false;
    if (st == OK) {
        st = create_at(x, &loc, (uint16_t)(EXT_S_IFREG | (mode & 07777)), &in);
        made = st == OK;
    }
    if (st == OK)
        st = jx_write_fd(x, &in, fd, size);
    if (st == OK)
        st = stamp(x, &in);
    if (st != OK && made) {             /* no half file is left behind */
        struct jx_loc again;
        if (jx_locate(x, path, false, &again) == OK && again.found)
            (void)unlink_at(x, &again, false);
    }
    if (tmp)
        fclose(tmp);
    else
        close(fd);
    return st;
}

status_t jx_get(struct jx *x, const char *path, const char *host)
{
    struct jx_loc loc;
    status_t st = jx_locate(x, path, true, &loc);
    if (st != OK)
        return st;
    if (!loc.found)
        return ERR_NOT_FOUND;
    if (ext_itype(&loc.in) == EXT_FT_DIR)
        return ERR_IS_DIR;
    if (ext_itype(&loc.in) != EXT_FT_REG)
        return ERR_NOT_SUPPORTED;
    FILE *out = strcmp(host, "-") ? fopen(host, "wb") : stdout;
    if (!out) {
        fprintf(stderr, "jamext: %s: %s\n", host, strerror(errno));
        return ERR_IO;
    }
    uint8_t *buf = malloc(JX_CHUNK);
    if (!buf) {
        if (out != stdout)
            fclose(out);
        return ERR_NO_MEMORY;
    }
    uint64_t size = ext_isize(&loc.in);
    for (uint64_t off = 0; off < size && st == OK;) {
        size_t n = 0, want = size - off < JX_CHUNK ? (size_t)(size - off) : JX_CHUNK;
        st = ext_read(&x->fs, &loc.in, off, buf, want, &n);
        if (st != OK || !n)
            break;
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "jamext: %s: %s\n", host, strerror(errno));
            st = ERR_IO;
        }
        off += n;
    }
    free(buf);
    if (out == stdout)
        fflush(out);
    else if (fclose(out) != 0 && st == OK)
        st = ERR_IO;
    return st;
}

/* ---- links ------------------------------------------------------------------------------- */

status_t jx_ln(struct jx *x, const char *target, const char *path, bool symbolic)
{
    struct jx_loc dest;
    status_t st = make_parents(x, path);
    if (st == OK)
        st = jx_locate(x, path, false, &dest);
    if (st != OK)
        return st;
    struct jx_loc src;
    if (!symbolic) {
        st = jx_locate(x, target, false, &src);
        if (st != OK)
            return st;
        if (!src.found)
            return ERR_NOT_FOUND;
    }
    if (dest.found && ext_itype(&dest.in) == EXT_FT_DIR) {    /* into the directory */
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        size_t n = strlen(base);
        if (!n || n > EXT_NAME_MAX)
            return ERR_INVALID_ARGS;
        struct ext_dentry d;
        struct ext_name nm = { .s = base, .len = (uint32_t)n };
        struct ext_inode dir = dest.in;
        if (ext_lookup(&x->fs, &dir, &nm, &d) == OK)
            return ERR_ALREADY_EXISTS;
        dest.dir = dir;
        memcpy(dest.name, base, n + 1);
        dest.name_len = (uint32_t)n;
        dest.found = false;
    }
    if (dest.found)
        return ERR_ALREADY_EXISTS;
    if (!dest.name_len)
        return ERR_ALREADY_EXISTS;
    struct ext_name n = loc_name(&dest);
    if (!jx_begin(x))
        return ERR_BAD_STATE;
    if (symbolic) {
        struct ext_name t = { .s = target, .len = (uint32_t)strlen(target) };
        struct ext_attrs a = jx_attrs(x, 0777);
        struct ext_inode out;
        st = ext_symlink(&x->fs, &dest.dir, &n, &t, &a, &out);
    } else {
        struct ext_time now = jx_time(x);
        st = ext_link(&x->fs, &dest.dir, &n, &src.in, &now);
    }
    if (st != OK) {
        jx_abort(x);
        return st;
    }
    return jx_commit(x) ? OK : ERR_IO;
}

status_t jx_chmod(struct jx *x, const char *path, uint16_t mode)
{
    struct jx_loc loc;
    status_t st = jx_locate(x, path, true, &loc);
    if (st != OK)
        return st;
    if (!loc.found)
        return ERR_NOT_FOUND;
    struct ext_inode *in = &loc.in;
    in->d.mode = (uint16_t)((in->d.mode & EXT_S_IFMT) | (mode & 07777));
    struct ext_time t = jx_time(x);
    if (!jx_begin(x))
        return ERR_BAD_STATE;
    st = ext_itime_set(in, EXT_CTIME, &t);
    if (st == OK)
        st = ext_iput(&x->fs, in);
    if (st != OK) {
        jx_abort(x);
        return st;
    }
    return jx_commit(x) ? OK : ERR_IO;
}

/* ---- listing ----------------------------------------------------------------------------- */

static void mode_string(const struct ext_inode *in, char out[11])
{
    uint16_t m = in->d.mode;
    switch (ext_itype(in)) {
    case EXT_FT_REG:     out[0] = '-'; break;
    case EXT_FT_DIR:     out[0] = 'd'; break;
    case EXT_FT_SYMLINK: out[0] = 'l'; break;
    case EXT_FT_CHR:     out[0] = 'c'; break;
    case EXT_FT_BLK:     out[0] = 'b'; break;
    case EXT_FT_FIFO:    out[0] = 'p'; break;
    case EXT_FT_SOCK:    out[0] = 's'; break;
    default:             out[0] = '?'; break;
    }
    for (int i = 0; i < 9; i++)
        out[1 + i] = (m >> (8 - i) & 1) ? "rwxrwxrwx"[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 01) ? 't' : 'T';
    out[10] = '\0';
}

static void list_one(struct jx *x, const char *name, struct ext_inode *in, bool longf)
{
    if (!longf) {
        printf("%s%s\n", name, ext_itype(in) == EXT_FT_DIR ? "/" : "");
        return;
    }
    char mode[11], when[32] = "-", target[4097] = "";
    mode_string(in, mode);
    struct ext_time t;
    ext_itime(in, EXT_MTIME, &t);
    time_t secs = (time_t)t.sec;
    struct tm tm;
    if (gmtime_r(&secs, &tm))
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    if (ext_itype(in) == EXT_FT_SYMLINK) {
        size_t n = 0;
        if (ext_readlink(&x->fs, in, target + 4, sizeof(target) - 5, &n) == OK) {
            memcpy(target, " -> ", 4);
            target[4 + n] = '\0';
        }
    }
    printf("%s %3u %5u %5u %10llu %s %s%s\n", mode, (unsigned)in->d.links_count, ext_iuid(in),
           ext_igid(in), (unsigned long long)ext_isize(in), when, name, target);
}

static status_t list_dir(struct jx *x, const char *shown, struct ext_inode *dir, bool longf,
                         bool recursive, bool heading)
{
    char **names;
    size_t n;
    status_t st = list_names(x, dir, &names, &n);
    if (st != OK)
        return st;
    if (heading)
        printf("%s:\n", shown);
    struct ext_inode *kids = calloc(n ? n : 1, sizeof(*kids));
    if (!kids) {
        free_names(names, n);
        return ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < n && st == OK; i++) {
        struct ext_dentry d;
        struct ext_name nm = { .s = names[i], .len = (uint32_t)strlen(names[i]) };
        st = ext_lookup(&x->fs, dir, &nm, &d);
        if (st == OK)
            st = ext_iget(&x->fs, d.ino, 0, &kids[i]);
        if (st == OK)
            list_one(x, names[i], &kids[i], longf);
    }
    for (size_t i = 0; i < n && st == OK && recursive; i++) {
        if (ext_itype(&kids[i]) != EXT_FT_DIR)
            continue;
        char *sub = malloc(strlen(shown) + strlen(names[i]) + 2);
        if (!sub) {
            st = ERR_NO_MEMORY;
            break;
        }
        sprintf(sub, "%s%s%s", shown, shown[strlen(shown) - 1] == '/' ? "" : "/", names[i]);
        printf("\n");
        st = list_dir(x, sub, &kids[i], longf, true, true);
        free(sub);
    }
    free(kids);
    free_names(names, n);
    return st;
}

status_t jx_ls(struct jx *x, const char *path, bool longf, bool recursive)
{
    struct jx_loc loc;
    status_t st = jx_locate(x, path, true, &loc);
    if (st != OK)
        return st;
    if (!loc.found)
        return ERR_NOT_FOUND;
    if (ext_itype(&loc.in) == EXT_FT_DIR)
        return list_dir(x, path, &loc.in, longf, recursive, recursive);
    list_one(x, path, &loc.in, longf);
    return OK;
}
