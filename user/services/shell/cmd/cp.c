/* cp: copy a file, to a new name or into a directory (on any mount). */
#include "sh.h"

#define CHUNK (64u << 10)

/* Everything from in to out. */
static status_t copy(struct jfile *in, struct jfile *out)
{
    uint8_t *buf = malloc(CHUNK);
    if (!buf)
        return ERR_NO_MEMORY;
    status_t st = OK;
    size_t got = 0, put = 0;
    for (uint64_t off = 0; st == OK; off += got) {
        if (sh_interrupted())
            st = ERR_CANCELED;
        else
            st = file_read(in, off, buf, CHUNK, &got);
        if (st != OK || !got)
            break;
        st = file_write(out, off, buf, got, &put);
        if (st == OK && put < got)
            st = ERR_NO_SPACE;
    }
    free(buf);
    return st;
}

SH_CMD(cp)
{
    char from[SH_PATH_MAX], to[SH_PATH_MAX];
    bool dir = false;
    uint64_t size;
    if (argc != 3) {
        sh_tty("usage: cp <from> <to>\n");
        return 2;
    }
    if (!sh_resolve(argv[1], from, sizeof(from)) || !sh_dest(from, argv[2], to, sizeof(to))) {
        sh_tty("cp: the path is too long\n");
        return 1;
    }
    status_t st = sh_stat(from, &dir, &size);
    if (st == OK && dir) {
        sh_tty("cp: %s: a directory (only files are copied)\n", argv[1]);
        return 1;
    }
    if (st == OK && !strcmp(from, to)) {
        sh_tty("cp: %s and %s are the same file\n", argv[1], argv[2]);
        return 1;
    }
    struct jfile in, out;
    if (st == OK)
        st = file_open(from, FS_READ, &in);
    if (st != OK) {
        sh_tty("cp: %s: %s\n", argv[1], sh_why(st));
        return 1;
    }
    st = file_open(to, FS_WRITE | FS_CREATE | FS_TRUNCATE, &out);
    if (st == OK) {
        st = copy(&in, &out);
        file_close(&out);
    }
    file_close(&in);
    if (st != OK)
        sh_tty("cp: %s: %s\n", argv[2], sh_why(st));
    return st == OK ? 0 : 1;
}
