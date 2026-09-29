/* whoami: $USER (there is one user: jam). */
#include "sh.h"

SH_CMD(whoami)
{
    (void)argc;
    (void)argv;
    const char *u = sh_getvar("USER");
    sh_say("%s\n", u ? u : "jam");
    return 0;
}
