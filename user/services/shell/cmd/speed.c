/* speed: the network's throughput against tools/speed.py on the Mac. It is
 * bin/speed (user/apps/speed), run as a helper (sh_run_helper: its list,
 * the network with the listen permission and the resolver; its lines
 * printed as the shell's), so Ctrl+C stops it cleanly: it says so and ends
 * with 130, as `fetch` does. */
#include "sh.h"

#define SPEED_PATH "bin/speed"

SH_CMD(speed)
{
    struct spawn_handle x[2];   /* sh_run_helper adds its two */
    return sh_run_helper(SPEED_PATH, argc, (const char *const *)argv, x, 0);
}
