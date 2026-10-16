/* ln: another name for a file. ln <target> <name> makes a hard link: the
 * same file (the same inode) under a second name, within one mount, never
 * to a directory. ln -s <target> <name> makes a symbolic link: a small file
 * that holds the text <target>, which means the same as that path wherever
 * the link is looked at from (a relative target is relative to the link's
 * directory). If <name> is a directory, the link goes into it, under the
 * target's own name. Only a volume that has links (/jam, other ext volumes)
 * can: the stick's FAT says so. */
#include "sh.h"

SH_CMD(ln)
{
    int first = 1;
    bool symbolic = false;
    if (first < argc && !strcmp(argv[first], "-s")) {
        symbolic = true;
        first++;
    }
    if (argc - first != 2 || (argv[first][0] == '-' && argv[first][1])) {
        sh_tty("usage: ln [-s] <target> <name>\n");
        return 2;
    }
    const char *target = argv[first], *name = argv[first + 1];
    char from[SH_PATH_MAX], to[SH_PATH_MAX];
    status_t st = OK;
    if (!symbolic && !sh_resolve(target, from, sizeof(from)))
        st = ERR_INVALID_ARGS;
    else if (symbolic)
        snprintf(from, sizeof(from), "%s", target);   /* the text, as typed */
    if (st == OK && !sh_dest(symbolic ? sh_basename(target) : from, name, to, sizeof(to)))
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = symbolic ? fs_symlink(target, to) : fs_link(from, to);
    if (st == ERR_NOT_SUPPORTED) {
        sh_tty("ln: %s: no %slinks here (only an ext volume such as /jam has them%s)\n", name,
               symbolic ? "symbolic " : "hard ", symbolic ? "" : ", and both names must be on it");
        return 1;
    }
    if (st != OK) {
        sh_tty("ln: %s: %s\n", name, st == ERR_IS_DIR ? "a directory can't have a hard link"
                                                       : sh_why(st));
        return 1;
    }
    return 0;
}
