/* set: list the shell variables, or set some (set NAME value, or
 * set NAME=value ...). */
#include "sh.h"

static void list(void)
{
    for (int i = 0; i < SH_MAX_VARS; i++) {
        const char *name, *value;
        bool exported;
        if (sh_var_at(i, &name, &value, &exported))
            sh_say("%s%s=%s\n", exported ? "export " : "", name, value);
    }
}

SH_CMD(set)
{
    if (argc == 1) {
        list();
        return 0;
    }
    if (argc == 3 && !sh_assignment(argv[1])) {
        for (const char *p = argv[1]; *p; p++)
            if (!sh_name_char(*p, p == argv[1])) {
                sh_tty("set: %s: not a variable name\n", argv[1]);
                return 1;
            }
        sh_setvar(argv[1], argv[2], -1);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        size_t l = sh_assignment(argv[i]);
        if (!l || l >= SH_NAME_MAX) {
            sh_tty("usage: set NAME value, or set NAME=value\n");
            return 1;
        }
        char name[SH_NAME_MAX];
        memcpy(name, argv[i], l);
        name[l] = '\0';
        sh_setvar(name, argv[i] + l + 1, -1);
    }
    return 0;
}
