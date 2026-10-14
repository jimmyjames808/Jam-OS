/* pkgcheck: the ports formats' C readers (user/lib/pkg*.c, the very files
 * bin/pkg and init link) on the Mac, with the sanitisers on, held to the
 * answers the Python reference gave (tools/portslib/pkgverify.py):
 *
 *     pkgcheck <vectors dir> [<fuzz dir> [<iterations>]]
 *
 * It runs utest's checks (user/tests/utest/pkgcases.c, pkgfuzz.c, pkgjnl.c,
 * pkgmisc.c: the vectors of ports/vectors/ against CASES, the small parts,
 * the journal, and <iterations> mutated texts and archives, default 10000)
 * and then, with a fuzz directory made by `tools/ports.py pkgfuzz`, every
 * line of its CASES: texts and archives the Python tool mutated and read,
 * the C readers must agree with, accepting nothing it refused and
 * refusing nothing it accepted. Exit 0 and "PASS" if all agree. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkgcases.h"

const struct pkgv_file *pkgv_files;
unsigned pkgv_nfiles;

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = n > 0 ? (size_t)n : 0;
    return b;
}

static int load_vectors(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    struct pkgv_file *files = NULL;
    unsigned n = 0;
    if (!d) {
        perror(dir);
        return -1;
    }
    while ((e = readdir(d))) {
        size_t len;
        if (e->d_name[0] == '.')
            continue;
        uint8_t *b = slurp(dir, e->d_name, &len);
        files = realloc(files, (n + 1) * sizeof(*files));
        if (!b || !files)
            return -1;
        files[n++] = (struct pkgv_file){ strdup(e->d_name), b, len };
    }
    closedir(d);
    pkgv_files = files;
    pkgv_nfiles = n;
    return 0;
}

static int unhex(const char *s, char *out, size_t cap)
{
    size_t n = strlen(s) / 2;
    if (strlen(s) % 2 || n + 1 > cap)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (char)v;
    }
    out[n] = '\0';
    return 0;
}

/* One line of the fuzz directory's CASES. */
static int fuzz_case(const char *dir, char *line)
{
    char kind[32], a[1200], b[1200], c[32];
    int got;
    if (sscanf(line, "%31s %1199s %1199s %31s", kind, a, b, c) < 3)
        return -1;
    if (!strcmp(kind, "version")) {   /* version x<hex a> x<hex b> <sign> */
        char x[600], y[600];
        if (unhex(a + 1, x, sizeof(x)) || unhex(b + 1, y, sizeof(y)))
            return -1;
        got = pkgc_version_cmp(x, y);
        return got == atoi(c) ? 0 : 1;
    }
    if (!strcmp(kind, "path")) {   /* path x<hex path> ok|refused */
        char x[600];
        if (unhex(a + 1, x, sizeof(x)))
            return -1;
        return pkgc_path_ok(x) == !strcmp(b, "ok") ? 0 : 1;
    }
    size_t len;
    uint8_t *data = slurp(dir, a, &len);
    if (!data)
        return -1;
    got = !strcmp(kind, "package") ? pkgc_package_verdict(data, len)
                                   : pkgc_text_verdict(kind, data, len);
    free(data);
    if (got < 0)
        return 1;
    return got == !strcmp(b, "ok") ? 0 : 1;
}

static unsigned fuzz_cases(const char *dir)
{
    char path[1024], line[2600];
    unsigned fails = 0, n = 0;
    snprintf(path, sizeof(path), "%s/CASES", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        n++;
        int r = fuzz_case(dir, line);
        if (r) {
            if (fails < 20)
                printf("pkgcheck: differs from the Python reader: %.120s%s\n", line,
                       r < 0 ? " (case unreadable)" : "");
            fails++;
        }
    }
    fclose(f);
    printf("pkgcheck: %u cases from %s: %u differ\n", n, dir, fails);
    return fails + (n ? 0 : 1);
}

int main(int argc, char **argv)
{
    unsigned iterations = argc > 3 ? (unsigned)atoi(argv[3]) : 10000, fails = 0;
    if (argc < 2 || load_vectors(argv[1])) {
        fprintf(stderr, "usage: pkgcheck <vectors dir> [<fuzz dir> [<iterations>]]\n");
        return 2;
    }
    fails += pkgc_vectors();
    fails += pkgc_misc();
    fails += pkgc_journal(0x6a6f75726e616c, iterations);
    fails += pkgc_fuzz(0x66757a7a, iterations);
    if (argc > 2)
        fails += fuzz_cases(argv[2]);
    printf("pkgcheck: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
