/* utest: the user-space test suite (bin/utest, with devmgr's channels) and
 * its result line; with arguments, one of utest's child modes. */
#include "sh.h"

SH_CMD(utest)
{
    return sh_run_test_program(argc, argv);
}
