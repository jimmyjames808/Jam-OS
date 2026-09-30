/* rm: remove files and empty directories. -r removes a directory with
 * everything in it. A mount point itself is never removed. */
#include "sh.h"

#define MAX_DEPTH 16

/* abs and, if it is a directory, all it holds. */
static status_t remove_tree(const char *abs, int depth)
{
    bool dir;
    uint64_t size;
    status_t st = sh_stat(abs, &dir, &size);
    struct fs_entry *e = st == OK && dir ? malloc(sizeof(*e)) : NULL;
    if (st == OK && dir && (!e || depth >= MAX_DEPTH))
        st = e ? ERR_OUT_OF_RANGE : ERR_NO_MEMORY;
    /* Entry 0 until none is left: a removal moves the others down. */
    while (st == OK && dir && fs_readdir(abs, 0, e) == OK) {
        char child[SH_PATH_MAX];
        if (sh_interrupted())
            st = ERR_CANCELED;
        else if (!sh_join(abs, e->name, child, sizeof(child)))
            st = ERR_OUT_OF_RANGE;
        else
            st = remove_tree(child, depth + 1);
    }
    free(e);
    return st == OK ? fs_unlink(abs) : st;
}

/* One argument; false (said) if it wasn't removed. */
static bool remove_one(const char *arg, bool recursive)
{
    char abs[SH_PATH_MAX];
    bool dir = false;
    uint64_t size;
    struct fs_entry *e = malloc(sizeof(*e));
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? sh_stat(abs, &dir, &size)
                                                    : ERR_INVALID_ARGS;
    bool full = st == OK && dir && e && fs_readdir(abs, 0, e) == OK;
    free(e);
    if (st == OK && sh_is_mount(abs)) {
        sh_tty("rm: %s: a mount point: not removed\n", arg);
        return false;
    }
    if (st == OK && full && !recursive) {
        sh_tty("rm: %s: a directory with entries (rm -r removes them too)\n", arg);
        return false;
    }
    if (st == OK)
        st = recursive ? remove_tree(abs, 0) : fs_unlink(abs);
    if (st != OK)
        sh_tty("rm: %s: %s\n", arg, sh_why(st));
    return st == OK;
}

SH_CMD(rm)
{
    int first = 1;
    bool recursive = argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "-rf"));
    if (recursive)
        first = 2;
    if (first >= argc || (argv[first][0] == '-' && argv[first][1])) {
        sh_tty("usage: rm [-r] <path>...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc && !sh_interrupted(); i++)
        if (!remove_one(argv[i], recursive))
            status = 1;
    return status;
}
