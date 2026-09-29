/* usbtest: the USB checks (bin/usbtest, with devmgr's channels) and their
 * result line. */
#include "sh.h"

SH_CMD(usbtest)
{
    return sh_run_test_program(argc, argv);
}
