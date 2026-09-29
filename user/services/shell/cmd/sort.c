/* sort: the lines of a file or the pipe, sorted (a stable merge sort);
 * -r reverse, -n by the number each starts with, -u drop repeats. */
#include "sh.h"

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

/* Sort d's lines and print them. */
static int sort_text(const char *d, size_t n, bool unique)
{
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
    struct sh_lines ls = { d, d + n };
    size_t m = 0;
    while (m < count && sh_next_line(&ls, &v[m].s, &v[m].n))
        m++;
    merge_sort(v, tmp, m);
    for (size_t k = 0; k < m; k++) {
        if (unique && k && !cmp_lines(&v[k], &v[k - 1]))
            continue;
        sh_put_line(v[k].s, v[k].n);
    }
    free(v);
    free(tmp);
    return 0;
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
    return sort_text(d, n, unique);
}
