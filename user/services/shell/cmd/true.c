/* true: status 0, whatever words follow it (POSIX's true ignores its
 * operands: `true x` is 0 too). */
#include "sh.h"

SH_CMD(true)
{
    (void)argc;
    (void)argv;
    return 0;
}
