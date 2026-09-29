/* type (which): what each name is: an alias, a builtin, or a program in
 * /boot/bin. */
#include "sh.h"

SH_CMD(type)
{
    int st = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = sh_alias_of(argv[i]);
        char path[SH_PATH_MAX];
        const void *d;
        uint64_t n;
        snprintf(path, sizeof(path), "/boot/bin/%s", argv[i]);
        if (a)
            sh_say("%s is an alias for %s\n", argv[i], a);
        else if (sh_find_cmd(argv[i]))
            sh_say("%s is a shell builtin\n", argv[i]);
        else if (!strchr(argv[i], '/') && sh_read(path, &d, &n) == OK)
            sh_say("%s is %s\n", argv[i], path);
        else {
            sh_say("%s: not found\n", argv[i]);
            st = 1;
        }
    }
    return st;
}
