/* unset: remove shell variables. */
#include "../sh.h"

SH_CMD(unset)
{
    for (int i = 1; i < argc; i++)
        sh_unsetvar(argv[i]);
    return 0;
}
