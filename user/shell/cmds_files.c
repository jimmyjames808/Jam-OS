/* File and text commands: pwd cd ls find cat hexdump wc head tail grep
 * sort uniq seq. The text ones read a file or, with none given, the pipe
 * (sh_input). Files come through sh_vfs.c (today /boot only). */
#include "sh.h"

/* ---- helpers ---------------------------------------------------------------------- */

/* "-n N" or "-N": the count, advancing *i. */
static bool count_opt(int argc, char **argv, int *i, uint64_t *n)
{
    const char *a = argv[*i];
    if (!strcmp(a, "-n") && *i + 1 < argc) {
        (*i)++;
        return sh_parse_u64(argv[*i], n);
    }
    if (a[0] == '-' && a[1] >= '0' && a[1] <= '9')
        return sh_parse_u64(a + 1, n);
    return false;
}

/* Line k of data: next_line walks them. */
struct lines {
    const char *p, *end;
};

static bool next_line(struct lines *l, const char **s, size_t *n)
{
    if (l->p >= l->end)
        return false;
    const char *e = l->p;
    while (e < l->end && *e != '\n')
        e++;
    *s = l->p;
    *n = (size_t)(e - l->p);
    l->p = e < l->end ? e + 1 : e;
    return true;
}

static void put_line(const char *s, size_t n)
{
    sh_put(s, n);
    sh_put("\n", 1);
}

/* ---- directories -------------------------------------------------------------------- */

SH_CMD(pwd)
{
    (void)argc;
    (void)argv;
    sh_say("%s\n", sh_cwd());
    return 0;
}

SH_CMD(cd)
{
    const char *to = argc > 1 ? argv[1] : sh_getvar("HOME");
    if (!to)
        to = "/";
    if (!sh_chdir(to)) {
        sh_tty("cd: %s: no such directory\n", to);
        return 1;
    }
    return 0;
}

#define LS_MAX 256

static void ls_entries(struct sh_dirent *e, int n, bool longf)
{
    bool tty = !sh_piped();
    if (longf || !tty) {
        for (int i = 0; i < n; i++) {
            char hs[24];
            if (longf)
                sh_say("%10s  ", e[i].dir ? "<dir>" : sh_human(e[i].size, hs, sizeof(hs)));
            if (e[i].dir && tty)
                sh_say("\033[94m%s/\033[0m\n", e[i].name);
            else
                sh_say("%s%s\n", e[i].name, e[i].dir ? "/" : "");
        }
        return;
    }
    /* Columns, as wide as the widest name. */
    size_t w = 1;
    for (int i = 0; i < n; i++)
        if (strlen(e[i].name) + 1 > w)
            w = strlen(e[i].name) + 1;
    unsigned per = (unsigned)(96 / (w + 2));
    if (!per)
        per = 1;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(e[i].name) + e[i].dir;
        if (e[i].dir)
            sh_say("\033[94m%s/\033[0m", e[i].name);
        else
            sh_say("%s", e[i].name);
        bool last = (unsigned)(i + 1) % per == 0 || i + 1 == n;
        if (last)
            sh_say("\n");
        else
            sh_say("%*s", (int)(w + 2 - l), "");
    }
}

SH_CMD(ls)
{
    bool longf = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l')
                longf = true;
            else if (*p != 'a' && *p != '1') {
                sh_tty("usage: ls [-l] [path...]\n");
                return 2;
            }
        }
        first++;
    }
    struct sh_dirent *e = calloc(LS_MAX, sizeof(*e));
    if (!e)
        return 1;
    int st = 0;
    int nargs = argc - first;
    for (int i = first; i < argc || (i == first && nargs == 0); i++) {
        const char *arg = nargs ? argv[i] : ".";
        char abs[SH_PATH_MAX];
        bool dir;
        uint64_t size;
        if (!sh_resolve(arg, abs, sizeof(abs)) || sh_stat(abs, &dir, &size) != OK) {
            sh_tty("ls: %s: no such file or directory\n", arg);
            st = 1;
            continue;
        }
        if (!dir) {
            struct sh_dirent one = { .dir = false, .size = size };
            snprintf(one.name, sizeof(one.name), "%s", arg);
            ls_entries(&one, 1, longf);
            continue;
        }
        if (nargs > 1)
            sh_say("%s:\n", arg);
        int n = sh_readdir(abs, e, LS_MAX);
        if (n > 0)
            ls_entries(e, n, longf);
        if (nargs > 1 && i + 1 < argc)
            sh_say("\n");
    }
    free(e);
    return st;
}

static void find_in(const char *abs, const char *name_part, int depth)
{
    if (depth > 16 || sh_interrupted())
        return;
    struct sh_dirent *e = calloc(LS_MAX, sizeof(*e));
    if (!e)
        return;
    int n = sh_readdir(abs, e, LS_MAX);
    for (int i = 0; i < n; i++) {
        char path[SH_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s%s", abs, strcmp(abs, "/") ? "/" : "", e[i].name);
        bool show = true;
        if (name_part) {
            show = false;
            size_t nl = strlen(name_part), l = strlen(e[i].name);
            for (size_t k = 0; k + nl <= l && !show; k++)
                show = !strncmp(e[i].name + k, name_part, nl);
        }
        if (show)
            sh_say("%s%s\n", path, e[i].dir ? "/" : "");
        if (e[i].dir)
            find_in(path, name_part, depth + 1);
    }
    free(e);
}

SH_CMD(find)
{
    const char *dir = ".", *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc)
            name = argv[++i];
        else if (argv[i][0] != '-')
            dir = argv[i];
        else {
            sh_tty("usage: find [dir] [-name text]\n");
            return 2;
        }
    }
    char abs[SH_PATH_MAX];
    bool isdir;
    uint64_t size;
    if (!sh_resolve(dir, abs, sizeof(abs)) || sh_stat(abs, &isdir, &size) != OK || !isdir) {
        sh_tty("find: %s: no such directory\n", dir);
        return 1;
    }
    find_in(abs, name, 0);
    return 0;
}

/* ---- text ------------------------------------------------------------------------------ */

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

SH_CMD(hexdump)
{
    uint64_t skip = 0, limit = UINT64_MAX;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-C"))
            continue;
        uint64_t *what = !strcmp(argv[i], "-s") ? &skip : !strcmp(argv[i], "-n") ? &limit : NULL;
        if (!what || i + 1 >= argc || !sh_parse_u64(argv[i + 1], what)) {
            sh_tty("usage: hexdump [-s offset] [-n bytes] [file]\n");
            return 2;
        }
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("hexdump", argc, argv, i, &d, &n))
        return 1;
    if (skip > n)
        skip = n;
    uint64_t end = limit < n - skip ? skip + limit : n;
    for (uint64_t off = skip; off < end; off += 16) {
        if ((off & 0xfff) == 0 && sh_interrupted())
            return 130;
        char line[96];
        int k = snprintf(line, sizeof(line), "%08lx  ", (unsigned long)off);
        for (int j = 0; j < 16; j++) {
            if (off + j < end)
                k += snprintf(line + k, sizeof(line) - k, "%02x ", (uint8_t)d[off + j]);
            else
                k += snprintf(line + k, sizeof(line) - k, "   ");
            if (j == 7)
                line[k++] = ' ';
        }
        line[k++] = ' ';
        line[k++] = '|';
        for (int j = 0; j < 16 && off + j < end; j++) {
            char c = d[off + j];
            line[k++] = c >= 0x20 && c < 0x7f ? c : '.';
        }
        line[k++] = '|';
        line[k++] = '\n';
        sh_put(line, (size_t)k);
    }
    sh_say("%08lx\n", (unsigned long)end);
    return 0;
}

SH_CMD(wc)
{
    bool l = false, w = false, c = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l')
                l = true;
            else if (*p == 'w')
                w = true;
            else if (*p == 'c')
                c = true;
            else {
                sh_tty("usage: wc [-l|-w|-c] [file]\n");
                return 2;
            }
        }
    if (!l && !w && !c)
        l = w = c = true;
    const char *d;
    size_t n;
    if (!sh_input("wc", argc, argv, i, &d, &n))
        return 1;
    uint64_t lines = 0, words = 0;
    bool in_word = false;
    for (size_t k = 0; k < n; k++) {
        char ch = d[k];
        if (ch == '\n')
            lines++;
        bool space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
        if (!space && !in_word)
            words++;
        in_word = !space;
    }
    if (l)
        sh_say(l && !w && !c ? "%lu" : "%7lu", (unsigned long)lines);
    if (w)
        sh_say(!l && !c ? "%lu" : " %7lu", (unsigned long)words);
    if (c)
        sh_say(!l && !w ? "%lu" : " %7lu", (unsigned long)n);
    if (i < argc)
        sh_say(" %s", argv[i]);
    sh_say("\n");
    return 0;
}

SH_CMD(head)
{
    uint64_t want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!count_opt(argc, argv, &i, &want)) {
            sh_tty("usage: head [-n N] [file]\n");
            return 2;
        }
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("head", argc, argv, i, &d, &n))
        return 1;
    struct lines ls = { d, d + n };
    const char *s;
    size_t len;
    for (uint64_t k = 0; k < want && next_line(&ls, &s, &len); k++)
        put_line(s, len);
    return 0;
}

SH_CMD(tail)
{
    uint64_t want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!count_opt(argc, argv, &i, &want)) {
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
    struct lines ls = { d + start, d + n };
    const char *s;
    size_t len;
    while (next_line(&ls, &s, &len))
        put_line(s, len);
    return 0;
}

/* ---- grep: text with ^ $ . * (Pike's matcher) -------------------------------------------- */

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
            else {
                sh_tty("usage: grep [-i] [-v] [-c] [-n] <pattern> [file]\n");
                return 2;
            }
        }
    if (i >= argc) {
        sh_tty("usage: grep [-i] [-v] [-c] [-n] <pattern> [file]\n");
        return 2;
    }
    const char *re = argv[i++];
    const char *d;
    size_t n;
    if (!sh_input("grep", argc, argv, i, &d, &n))
        return 1;
    struct lines ls = { d, d + n };
    const char *s;
    size_t len;
    uint64_t hits = 0, k = 0;
    while (next_line(&ls, &s, &len)) {
        k++;
        if (match(re, s, len) == inv)
            continue;
        hits++;
        if (count)
            continue;
        if (num)
            sh_say("%lu:", (unsigned long)k);
        put_line(s, len);
    }
    if (count)
        sh_say("%lu\n", (unsigned long)hits);
    return hits ? 0 : 1;
}

/* ---- sort, uniq, seq ----------------------------------------------------------------------- */

struct line_ref {
    const char *s;
    size_t      n;
};

static bool numeric, reverse;

static int64_t num_of(const struct line_ref *l)
{
    size_t i = 0;
    while (i < l->n && (l->s[i] == ' ' || l->s[i] == '\t'))
        i++;
    bool neg = i < l->n && l->s[i] == '-';
    if (neg)
        i++;
    int64_t v = 0;
    while (i < l->n && l->s[i] >= '0' && l->s[i] <= '9')
        v = v * 10 + (l->s[i++] - '0');
    return neg ? -v : v;
}

static int cmp_lines(const struct line_ref *a, const struct line_ref *b)
{
    int r = 0;
    if (numeric) {
        int64_t x = num_of(a), y = num_of(b);
        r = x < y ? -1 : x > y;
    }
    if (!r) {
        size_t m = a->n < b->n ? a->n : b->n;
        r = memcmp(a->s, b->s, m);
        if (!r)
            r = a->n < b->n ? -1 : a->n > b->n;
    }
    return reverse ? -r : r;
}

static void merge_sort(struct line_ref *v, struct line_ref *tmp, size_t n)
{
    if (n < 2)
        return;
    size_t h = n / 2;
    merge_sort(v, tmp, h);
    merge_sort(v + h, tmp, n - h);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = cmp_lines(&v[j], &v[i]) < 0 ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(*v));
}

SH_CMD(sort)
{
    bool unique = false;
    numeric = reverse = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r')
                reverse = true;
            else if (*p == 'n')
                numeric = true;
            else if (*p == 'u')
                unique = true;
            else {
                sh_tty("usage: sort [-r] [-n] [-u] [file]\n");
                return 2;
            }
        }
    const char *d;
    size_t n;
    if (!sh_input("sort", argc, argv, i, &d, &n))
        return 1;
    size_t count = 0;
    for (size_t k = 0; k < n; k++)
        count += d[k] == '\n';
    count++;
    struct line_ref *v = malloc(count * sizeof(*v)), *tmp = malloc(count * sizeof(*v));
    if (!v || !tmp) {
        free(v);
        free(tmp);
        sh_tty("sort: out of memory\n");
        return 1;
    }
    struct lines ls = { d, d + n };
    size_t m = 0;
    while (m < count && next_line(&ls, &v[m].s, &v[m].n))
        m++;
    merge_sort(v, tmp, m);
    for (size_t k = 0; k < m; k++) {
        if (unique && k && !cmp_lines(&v[k], &v[k - 1]))
            continue;
        put_line(v[k].s, v[k].n);
    }
    free(v);
    free(tmp);
    return 0;
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
    struct lines ls = { d, d + n };
    const char *s, *prev = NULL;
    size_t len, plen = 0;
    uint64_t run = 0;
    while (next_line(&ls, &s, &len)) {
        if (prev && len == plen && !memcmp(s, prev, len)) {
            run++;
            continue;
        }
        if (prev) {
            if (count)
                sh_say("%7lu ", (unsigned long)run);
            put_line(prev, plen);
        }
        prev = s;
        plen = len;
        run = 1;
    }
    if (prev) {
        if (count)
            sh_say("%7lu ", (unsigned long)run);
        put_line(prev, plen);
    }
    return 0;
}

SH_CMD(seq)
{
    uint64_t first = 1, last;
    if (argc == 2 && sh_parse_u64(argv[1], &last)) {
    } else if (argc == 3 && sh_parse_u64(argv[1], &first) && sh_parse_u64(argv[2], &last)) {
    } else {
        sh_tty("usage: seq [first] last\n");
        return 2;
    }
    if (last - first > 1000000 && last > first) {
        sh_tty("seq: at most a million numbers\n");
        return 1;
    }
    for (uint64_t v = first; v <= last; v++) {
        sh_say("%lu\n", (unsigned long)v);
        if ((v & 1023) == 0 && sh_interrupted())
            return 130;
    }
    return 0;
}
