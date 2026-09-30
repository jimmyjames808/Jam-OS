/* mv: rename a file or directory, or move it into a directory, within one
 * mount. */
#include "sh.h"

SH_CMD(mv)
{
    char from[SH_PATH_MAX], to[SH_PATH_MAX];
    if (argc != 3) {
        sh_tty("usage: mv <from> <to>\n");
        return 2;
    }
    if (!sh_resolve(argv[1], from, sizeof(from)) || !sh_dest(from, argv[2], to, sizeof(to))) {
        sh_tty("mv: the path is too long\n");
        return 1;
    }
    status_t st = fs_rename(from, to);
    if (st == ERR_NOT_SUPPORTED)
        sh_tty("mv: %s and %s are on two mounts: cp it, then rm it\n", argv[1], argv[2]);
    else if (st != OK)
        sh_tty("mv: %s: %s\n", st == ERR_NOT_FOUND ? argv[1] : argv[2], sh_why(st));
    return st == OK ? 0 : 1;
}
