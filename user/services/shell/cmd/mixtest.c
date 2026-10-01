/* mixtest: the mixer checks (bin/mixtest, whose list asks for devmgr's
 * channels, the mixer's and init's control channel, since it kills the
 * mixer once) and their result line. */
#include "sh.h"

SH_CMD(mixtest)
{
    return sh_run_test_program(argc, argv);
}
