/* grep: the lines of a file or the pipe that match a pattern: text with
 * ^ $ . * (Pike's matcher); -i any case, -v the others, -c count, -n line
 * numbers. Status 1 if nothing matched. */
#include "sh.h"

static bool icase;

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool same(char re, char t)
{
    return re == '.' || re == t || (icase && lower(re) == lower(t));
}

static bool match_here(const char *re, const char *t, const char *end);

static bool match_star(char c, const char *re, const char *t, const char *end)
{
    for (;;) {
        if (match_here(re, t, end))
            return true;
        if (t >= end || !same(c, *t))
            return false;
        t++;
    }
}

static bool match_here(const char *re, const char *t, const char *end)
{
    if (!re[0])
        return true;
    if (re[1] == '*')
        return match_star(re[0], re + 2, t, end);
    if (re[0] == '$' && !re[1])
        return t == end;
    if (t < end && same(re[0], *t))
        return match_here(re + 1, t + 1, end);
    return false;
}

static bool match(const char *re, const char *t, size_t n)
{
    const char *end = t + n;
    if (re[0] == '^')
        return match_here(re + 1, t, end);
    for (;; t++) {
        if (match_here(re, t, end))
            return true;
        if (t >= end)
            return false;
    }
}

static int usage(void)
{
    sh_tty("usage: grep [-i] [-v] [-c] [-n] <pattern> [file]\n");
    return 2;
}

SH_CMD(grep)
{
    bool inv = false, count = false, num = false;
    icase = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'i')
                icase = true;
            else if (*p == 'v')
                inv = true;
            else if (*p == 'c')
                count = true;
            else if (*p == 'n')
                num = true;
            else
                return usage();
        }
    if (i >= argc)
        return usage();
    const char *re = argv[i++];
    const char *d;
    size_t n;
    if (!sh_input("grep", argc, argv, i, &d, &n))
        return 1;
    struct sh_lines ls = { d, d + n };
    const char *s;
    size_t len;
    uint64_t hits = 0, k = 0;
    while (sh_next_line(&ls, &s, &len)) {
        k++;
        if (match(re, s, len) == inv)
            continue;
        hits++;
        if (count)
            continue;
        if (num)
            sh_say("%lu:", (unsigned long)k);
        sh_put_line(s, len);
    }
    if (count)
        sh_say("%lu\n", (unsigned long)hits);
    return hits ? 0 : 1;
}
