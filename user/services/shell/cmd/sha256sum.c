/* sha256sum: the SHA-256 of each file (or the pipe), as the Mac's
 * `shasum -a 256` prints it: hex digest, two spaces, the name. A file is
 * read a piece at a time, so its size doesn't matter. */
#include "sh.h"

static void say_digest(const uint8_t d[SHA256_BYTES], const char *name)
{
    char hex[2 * SHA256_BYTES + 1];
    sha256_hex(d, hex);
    sh_say("%s  %s\n", hex, name);
}

/* One file argument; 0, 1 (said) or 130. */
static int sum_file(const char *arg)
{
    char abs[SH_PATH_MAX];
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
    uint8_t d[SHA256_BYTES];
    if (st == OK)
        st = sh_sha256_file(abs, d);
    if (st == ERR_CANCELED)
        return 130;
    if (st != OK) {
        sh_tty("sha256sum: %s: can't read it (%s)\n", arg, sh_why(st));
        return 1;
    }
    say_digest(d, arg);
    return 0;
}

SH_CMD(sha256sum)
{
    if (argc < 2) {
        const char *d;
        size_t n;
        if (!sh_input("sha256sum", argc, argv, 1, &d, &n))
            return 1;
        uint8_t digest[SHA256_BYTES];
        sha256(d, n, digest);
        say_digest(digest, "-");
        return 0;
    }
    int ret = 0;
    for (int i = 1; i < argc && ret != 130; i++) {
        int r = sum_file(argv[i]);
        if (r)
            ret = r;
    }
    return ret;
}
