/* false: status 1, whatever words follow it (POSIX's false ignores its
 * operands: `false x` is 1 too). */
#include "sh.h"

SH_CMD(false)
{
    (void)argc;
    (void)argv;
    return 1;
}
