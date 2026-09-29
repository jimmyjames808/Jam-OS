/* env (printenv): the exported variables, a program's environment. */
#include "../sh.h"

SH_CMD(env)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < SH_MAX_VARS; i++) {
        const char *name, *value;
        bool exported;
        if (sh_var_at(i, &name, &value, &exported) && exported)
            sh_say("%s=%s\n", name, value);
    }
    return 0;
}
