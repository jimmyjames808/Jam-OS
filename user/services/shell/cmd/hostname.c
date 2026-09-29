/* hostname: $HOSTNAME. */
#include "sh.h"

SH_CMD(hostname)
{
    (void)argc;
    (void)argv;
    const char *h = sh_getvar("HOSTNAME");
    sh_say("%s\n", h ? h : "jamos");
    return 0;
}
