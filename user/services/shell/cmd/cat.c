/* cat: print files, or the pipe. A binary file is refused (hexdump it). */
#include "../sh.h"

static bool binary(const char *d, size_t n)
{
    for (size_t i = 0; i < n && i < 1024; i++)
        if (!d[i])
            return true;
    return false;
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
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (!sh_input("cat", argc, argv, i, &d, &n)) {
            st = 1;
            continue;
        }
        if (binary(d, n)) {
            sh_tty("cat: %s: a binary file (%lu bytes); try hexdump %s | head\n", argv[i],
                   (unsigned long)n, argv[i]);
            st = 1;
            continue;
        }
        sh_put(d, n);
        if (n && d[n - 1] != '\n' && !sh_piped())
            sh_put("\n", 1);
    }
    return st;
}
