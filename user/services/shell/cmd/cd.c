/* cd: change directory (no argument: $HOME, else /). */
#include "sh.h"

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
