/* uniq: a file's or the pipe's lines with adjacent repeats dropped; -c
 * with how many there were. */
#include "../sh.h"

static void emit(const char *line, size_t len, uint64_t run, bool count)
{
    if (count)
        sh_say("%7lu ", (unsigned long)run);
    sh_put_line(line, len);
}

SH_CMD(uniq)
{
    bool count = false;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-c")) {
        count = true;
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("uniq", argc, argv, i, &d, &n))
        return 1;
    struct sh_lines ls = { d, d + n };
    const char *s, *prev = NULL;
    size_t len, plen = 0;
    uint64_t run = 0;
    while (sh_next_line(&ls, &s, &len)) {
        if (prev && len == plen && !memcmp(s, prev, len)) {
            run++;
            continue;
        }
        if (prev)
            emit(prev, plen, run, count);
        prev = s;
        plen = len;
        run = 1;
    }
    if (prev)
        emit(prev, plen, run, count);
    return 0;
}
