/* head: the first N lines (default 10) of a file or the pipe. */
#include "../sh.h"

SH_CMD(head)
{
    uint64_t want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!sh_count_opt(argc, argv, &i, &want)) {
            sh_tty("usage: head [-n N] [file]\n");
            return 2;
        }
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("head", argc, argv, i, &d, &n))
        return 1;
    struct sh_lines ls = { d, d + n };
    const char *s;
    size_t len;
    for (uint64_t k = 0; k < want && sh_next_line(&ls, &s, &len); k++)
        sh_put_line(s, len);
    return 0;
}
