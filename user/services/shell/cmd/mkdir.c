/* mkdir: make directories. -p also makes the missing ones on the way, and
 * doesn't mind a directory that is there already. */
#include "sh.h"

static bool is_dir(const char *abs)
{
    bool dir;
    uint64_t size;
    return sh_stat(abs, &dir, &size) == OK && dir;
}

/* abs, after every missing directory above it if parents. */
static status_t make(char *abs, bool parents)
{
    for (char *p = abs + 1; parents && *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        status_t st = is_dir(abs) ? OK : fs_mkdir(abs);
        *p = '/';
        if (st != OK)
            return st;
    }
    status_t st = fs_mkdir(abs);
    return st == ERR_ALREADY_EXISTS && parents && is_dir(abs) ? OK : st;
}

SH_CMD(mkdir)
{
    int first = 1;
    bool parents = argc > 1 && !strcmp(argv[1], "-p");
    if (parents)
        first = 2;
    if (first >= argc || (argv[first][0] == '-' && argv[first][1])) {
        sh_tty("usage: mkdir [-p] <dir>...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        char abs[SH_PATH_MAX];
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? make(abs, parents)
                                                            : ERR_INVALID_ARGS;
        if (st != OK) {
            sh_tty("mkdir: %s: %s\n", argv[i], sh_why(st));
            status = 1;
        }
    }
    return status;
}
