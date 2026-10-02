/* sha256sum: the SHA-256 of each file (or the pipe), as the Mac's
 * `shasum -a 256` prints it: hex digest, two spaces, the name. A file is
 * read a piece at a time, so its size doesn't matter. */
#include <sha256.h>
#include "sh.h"

#define CHUNK (256u << 10)

static void say_digest(struct sha256 *s, const char *name)
{
    uint8_t d[SHA256_BYTES];
    char hex[2 * SHA256_BYTES + 1];
    sha256_done(s, d);
    sha256_hex(d, hex);
    sh_say("%s  %s\n", hex, name);
}

/* One file argument; 0, 1 (said) or 130. */
static int sum_file(const char *arg, char *buf)
{
    char abs[SH_PATH_MAX];
    struct jfile f;
    bool dir = false;
    uint64_t size = 0;
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? sh_stat(abs, &dir, &size) : ERR_NOT_FOUND;
    if (st == ERR_NOT_FOUND) {
        sh_tty("sha256sum: %s: no such file\n", arg);
        return 1;
    }
    if (st == OK && dir) {
        sh_tty("sha256sum: %s: is a directory\n", arg);
        return 1;
    }
    if (st == OK)
        st = file_open(abs, FS_READ, &f);
    if (st != OK) {
        sh_tty("sha256sum: %s: can't read it (%s)\n", arg, sh_why(st));
        return 1;
    }
    struct sha256 s;
    sha256_init(&s);
    size_t got = 0;
    for (uint64_t off = 0;; off += got) {
        if (sh_interrupted()) {
            file_close(&f);
            return 130;
        }
        st = file_read(&f, off, buf, CHUNK, &got);
        if (st != OK || !got)
            break;
        sha256_add(&s, buf, got);
    }
    file_close(&f);
    if (st != OK) {
        sh_tty("sha256sum: %s: can't read it (%s)\n", arg, sh_why(st));
        return 1;
    }
    say_digest(&s, arg);
    return 0;
}

SH_CMD(sha256sum)
{
    if (argc < 2) {
        const char *d;
        size_t n;
        if (!sh_input("sha256sum", argc, argv, 1, &d, &n))
            return 1;
        struct sha256 s;
        sha256_init(&s);
        sha256_add(&s, d, n);
        say_digest(&s, "-");
        return 0;
    }
    char *buf = malloc(CHUNK);
    if (!buf) {
        sh_tty("sha256sum: out of memory\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc && ret != 130; i++) {
        int r = sum_file(argv[i], buf);
        if (r)
            ret = r;
    }
    free(buf);
    return ret;
}
