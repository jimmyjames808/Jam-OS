/* free: memory total, used and free (from sys_info). */
#include "sh.h"

SH_CMD(free)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    if (!sh_sysinfo(&s, "free"))
        return 1;
    char t[24], u[24], f[24], c[24];
    uint64_t total = s.mem_total_pages * 4096, fr = s.mem_free_pages * 4096;
    sh_say("            total        used        free\n");
    sh_say("Mem:   %10s  %10s  %10s\n", sh_human(total, t, sizeof(t)),
           sh_human(total - fr, u, sizeof(u)), sh_human(fr, f, sizeof(f)));
    sh_say("(free includes %s of cached thread stacks)\n",
           sh_human(s.stack_cache_pages * 4096, c, sizeof(c)));
    return 0;
}
