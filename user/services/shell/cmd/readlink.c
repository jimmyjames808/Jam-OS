/* readlink: the text of a symbolic link (what `ln -s` stored), unchanged;
 * a path that is not a link is a failure with nothing printed. */
#include "sh.h"

SH_CMD(readlink)
{
    if (argc != 2 || (argv[1][0] == '-' && argv[1][1])) {
        sh_tty("usage: readlink <path>\n");
        return 2;
    }
    char abs[SH_PATH_MAX];
    static char target[4097];
    status_t st = sh_resolve(argv[1], abs, sizeof(abs)) ? fs_readlink(abs, target, sizeof(target))
                                                         : ERR_INVALID_ARGS;
    if (st == ERR_NOT_SUPPORTED || st == ERR_INVALID_ARGS || st == ERR_WRONG_TYPE ||
        st == ERR_IS_DIR) {
        sh_tty("readlink: %s: not a symbolic link\n", argv[1]);
        return 1;
    }
    if (st != OK) {
        sh_tty("readlink: %s: %s\n", argv[1], sh_why(st));
        return 1;
    }
    sh_say("%s\n", target);
    return 0;
}
