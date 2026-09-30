/* hdatest: the HD Audio output stream checks (bin/hdatest, with devmgr's
 * channels) and their result line. */
#include "sh.h"

SH_CMD(hdatest)
{
    return sh_run_test_program(argc, argv);
}
