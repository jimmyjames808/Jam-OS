/* speed: the network's throughput against tools/speed.py on the Mac. It is
 * bin/speed (user/apps/speed) run as any program (its list: the network
 * with the listen permission, and the resolver), so Ctrl+C kills it; this
 * command is here for `help`. */
#include "sh.h"

SH_CMD(speed)
{
    return sh_run_program(argc, argv);
}
