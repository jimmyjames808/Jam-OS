/* Lines of text and line counts for the text commands (head, tail, grep,
 * sort, uniq). */
#include "sh.h"

bool sh_next_line(struct sh_lines *l, const char **s, size_t *n)
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

void sh_put_line(const char *s, size_t n)
{
    sh_put(s, n);
    sh_put("\n", 1);
}

bool sh_count_opt(int argc, char **argv, int *i, uint64_t *n)
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
