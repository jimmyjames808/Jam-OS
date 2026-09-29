/* export: mark variables (or NAME=value, set and marked) as given to the
 * programs `run` starts; no arguments: list them, as env. */
#include "sh.h"

SH_CMD(export)
{
    if (argc == 1)
        return shc_env(argc, argv);
    for (int i = 1; i < argc; i++) {
        size_t l = sh_assignment(argv[i]);
        char name[SH_NAME_MAX];
        if (l && l < sizeof(name)) {
            memcpy(name, argv[i], l);
            name[l] = '\0';
            sh_setvar(name, argv[i] + l + 1, 1);
        } else if (sh_exportvar(argv[i])) {
            /* marked */
        } else if (!l) {
            sh_setvar(argv[i], "", 1);
        }
    }
    return 0;
}
