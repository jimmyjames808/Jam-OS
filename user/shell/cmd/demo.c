/* demo: bin/demo, fractals with a thread per CPU on the screen it borrows
 * from the console (console.lend_screen) until it ends (default 76 s) or a
 * key is pressed. */
#include "../sh.h"

SH_CMD(demo)
{
    struct sys_info si;
    uint32_t cpus = jam_sys_info(sh_root(), &si) == OK && si.cpu_count ? si.cpu_count : 1;
    const char *secs = argc > 1 ? argv[1] : "76";
    char c[24], s[24], d[16];
    snprintf(c, sizeof(c), "cpus=%u", cpus);
    snprintf(s, sizeof(s), "seconds=%s", secs);
    snprintf(d, sizeof(d), "demo");
    char *av[] = { d, c, s, NULL };
    sh_say("demo: fractals on %u CPUs for %s s; any key stops it\n", cpus, secs);
    return sh_run_program(3, av);
}
