/* version: the Jam OS version, and whether the kernel tests are built in. */
#include "../sh.h"

SH_CMD(version)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    if (!sh_sysinfo(&s, "version"))
        return 1;
    sh_say("Jam OS %s%s\n", s.version, s.flags & SYSINFO_KTESTS ? " (kernel tests built in)" : "");
    return 0;
}
