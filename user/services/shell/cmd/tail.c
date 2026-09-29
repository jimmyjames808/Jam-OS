/* tail: the last N lines (default 10) of a file or the pipe. */
#include "sh.h"

SH_CMD(tail)
{
    uint64_t want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!sh_count_opt(argc, argv, &i, &want)) {
            sh_tty("usage: tail [-n N] [file]\n");
            return 2;
        }
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("tail", argc, argv, i, &d, &n))
        return 1;
    /* Back from the end over `want` line ends (a final newline doesn't
     * start another line). */
    size_t end = n && d[n - 1] == '\n' ? n - 1 : n;
    size_t start = end;
    uint64_t seen = 0;
    if (want == 0)
        return 0;
    while (start > 0) {
        if (d[start - 1] == '\n' && ++seen == want)
            break;
        start--;
    }
    struct sh_lines ls = { d + start, d + n };
    const char *s;
    size_t len;
    while (sh_next_line(&ls, &s, &len))
        sh_put_line(s, len);
    return 0;
}
