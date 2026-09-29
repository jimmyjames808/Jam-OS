/* clear: clear the screen (the console's; Ctrl+L does the same). */
#include <idl/console.h>
#include "../sh.h"

SH_CMD(clear)
{
    (void)argc;
    (void)argv;
    sh_flush();
    console_clear(sh_console());
    return 0;
}
