/* cat: print files, or the pipe. A file is read and printed a piece at a
 * time, so its size doesn't matter. A binary file is refused (hexdump it). */
#include "sh.h"

#define CHUNK (64u << 10)

static bool binary(const char *d, size_t n)
{
    for (size_t i = 0; i < n && i < 1024; i++)
        if (!d[i])
            return true;
    return false;
}

/* f, of `size` bytes, to the output through buf (CHUNK bytes). */
static status_t stream(struct jfile *f, const char *arg, uint64_t size, char *buf)
{
    size_t got = 0;
    char last = '\n';
    for (uint64_t off = 0;; off += got) {
        if (sh_interrupted())
            return ERR_CANCELED;
        status_t st = file_read(f, off, buf, CHUNK, &got);
        if (st != OK)
            return st;
        if (!got)
            break;
        if (off == 0 && binary(buf, got)) {
            sh_tty("cat: %s: a binary file (%lu bytes); try hexdump %s | head\n", arg,
                   (unsigned long)size, arg);
            return ERR_WRONG_TYPE;
        }
        sh_put(buf, got);
        last = buf[got - 1];
    }
    if (last != '\n' && !sh_piped())
        sh_put("\n", 1);
    return OK;
}

/* One file argument; false (said) if it wasn't printed. */
static bool cat_file(const char *arg, char *buf)
{
    char abs[SH_PATH_MAX];
    struct jfile f;
    bool dir = false;
    uint64_t size = 0;
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? sh_stat(abs, &dir, &size) : ERR_NOT_FOUND;
    if (st == ERR_NOT_FOUND) {
        sh_tty("cat: %s: no such file\n", arg);
        return false;
    }
    if (st == OK && dir) {
        sh_tty("cat: %s: is a directory\n", arg);
        return false;
    }
    if (st == OK)
        st = file_open(abs, FS_READ, &f);
    if (st == OK) {
        st = stream(&f, arg, size, buf);
        file_close(&f);
        if (st == ERR_WRONG_TYPE || st == ERR_CANCELED)
            return false;   /* said already */
    }
    if (st != OK)
        sh_tty("cat: %s: can't read it (%s)\n", arg, sh_why(st));
    return st == OK;
}

SH_CMD(cat)
{
    const char *d;
    size_t n;
    if (argc < 2) {
        if (!sh_input("cat", argc, argv, 1, &d, &n))
            return 1;
        sh_put(d, n);
        return 0;
    }
    char *buf = malloc(CHUNK);
    if (!buf) {
        sh_tty("cat: out of memory\n");
        return 1;
    }
    int st = 0;
    for (int i = 1; i < argc; i++)
        if (!cat_file(argv[i], buf))
            st = 1;
    free(buf);
    return st;
}
