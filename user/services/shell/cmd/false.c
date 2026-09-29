/* false: status 1. */
#include "../sh.h"

SH_CMD(false)
{
    (void)argc;
    (void)argv;
    return 1;
}
