/* help (?): the commands by category from the table (sh_table.c), or one
 * command's usage and whole help. */
#include "sh.h"

static int help_on(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = sh_alias_of(argv[i]);
        const struct sh_cmd *c = sh_find_cmd(argv[i]);
        if (c) {
            sh_say("usage: %s\n  %s\n", c->usage, c->help);
        } else if (a) {
            sh_say("%s: an alias for: %s\n", argv[i], a);
        } else {
            sh_say("help: no command %s\n", argv[i]);
            return 1;
        }
    }
    return 0;
}

SH_CMD(help)
{
    if (argc > 1)
        return help_on(argc, argv);
    sh_say("Commands: by category (help <command> for its usage)\n");
    for (unsigned k = 0; k < sh_ncategories; k++) {
        sh_say("\033[1m%s\033[0m\n", sh_categories[k]);
        const struct sh_cmd *c;
        for (size_t i = 0; (c = sh_cmd_at(i)); i++) {
            if (c->cat != k)
                continue;
            /* The usage, then the first line of the help. */
            const char *h = c->help;
            size_t hl = strchr(h, '\n') ? (size_t)(strchr(h, '\n') - h) : strlen(h);
            sh_say("  %-28s %.*s\n", c->usage, (int)hl, h);
        }
    }
    sh_say("Programs in /boot/bin run by name (utest, contest, ...). Aliases: lspci lsusb ll.\n"
           "Lines: a ; b   a && b   a || b   a | b | c   NAME=value   $NAME   'quotes'  # comment\n"
           "Keys: Tab completes, left/right/home/end, backspace/delete, up/down history,\n"
           "Ctrl+C cancel, Ctrl+L clear, Shift+PageUp/PageDown scroll back. "
           "help <cmd>: details.\n");
    return 0;
}
