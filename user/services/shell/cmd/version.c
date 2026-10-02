/* version: the Jam OS version, the git commit the build was made from
 * (bootfs's build.txt), and whether the kernel tests are built in. */
#include "sh.h"

SH_CMD(version)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    if (!sh_sysinfo(&s, "version"))
        return 1;
    char git[48];
    sh_build_git(git, sizeof(git));
    sh_say("Jam OS %s, git %s%s\n", s.version, git,
           s.flags & SYSINFO_KTESTS ? " (kernel tests built in)" : "");
    return 0;
}
