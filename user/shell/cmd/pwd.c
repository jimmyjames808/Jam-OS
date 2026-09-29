/* pwd: the current directory. */
#include "../sh.h"

SH_CMD(pwd)
{
    (void)argc;
    (void)argv;
    sh_say("%s\n", sh_cwd());
    return 0;
}
