/* find: every file and directory below a directory, or those whose names
 * contain -name's text. */
#include "sh.h"

static bool name_has(const char *name, const char *part)
{
    size_t nl = strlen(part), l = strlen(name);
    for (size_t k = 0; k + nl <= l; k++)
        if (!strncmp(name + k, part, nl))
            return true;
    return false;
}

static void find_in(const char *abs, const char *name_part, int depth)
{
    if (depth > 16 || sh_interrupted())
        return;
    struct sh_dirent *e = calloc(SH_DIR_MAX, sizeof(*e));
    if (!e)
        return;
    int n = sh_readdir(abs, e, SH_DIR_MAX);
    for (int i = 0; i < n; i++) {
        char path[SH_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s%s", abs, strcmp(abs, "/") ? "/" : "", e[i].name);
        if (!name_part || name_has(e[i].name, name_part))
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
