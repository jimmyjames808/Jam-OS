/* alias: list the aliases, show some (alias name), or define them
 * (alias ll='ls -l'). */
#include "../sh.h"

static void list(void)
{
    for (int i = 0; i < SH_MAX_ALIAS; i++) {
        const char *name, *value;
        if (sh_alias_at(i, &name, &value))
            sh_say("alias %s='%s'\n", name, value);
    }
}

SH_CMD(alias)
{
    if (argc == 1) {
        list();
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *a = sh_alias_of(argv[i]);
            if (a) {
                sh_say("alias %s='%s'\n", argv[i], a);
            } else {
                sh_tty("alias: %s: not found\n", argv[i]);
                st = 1;
            }
            continue;
        }
        *eq = '\0';
        if (!sh_set_alias(argv[i], eq + 1)) {
            sh_tty("alias: can't define %s\n", argv[i]);
            st = 1;
        }
    }
    return st;
}
