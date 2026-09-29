/* uname: the system's name; -r its version, -a with the machine and CPU. */
#include "sh.h"

SH_CMD(uname)
{
    struct sys_info s;
    if (argc > 1 && strcmp(argv[1], "-a") && strcmp(argv[1], "-r") && strcmp(argv[1], "-s")) {
        sh_tty("usage: uname [-a|-r|-s]\n");
        return 2;
    }
    if (argc == 1 || !strcmp(argv[1], "-s")) {
        sh_say("Jam OS\n");
        return 0;
    }
    if (!sh_sysinfo(&s, "uname"))
        return 1;
    if (!strcmp(argv[1], "-r")) {
        sh_say("%s\n", s.version);
        return 0;
    }
    const char *host = sh_getvar("HOSTNAME");
    sh_say("Jam OS %s %s x86_64 %s\n", host ? host : "jamos", s.version, s.cpu_brand);
    return 0;
}
