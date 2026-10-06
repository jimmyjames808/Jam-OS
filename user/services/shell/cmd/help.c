/* help (?): the everyday commands by category from the table (sh_table.c);
 * `help dev` the developer ones (the hardware's internals, the kernel, the
 * tests); `help <command>` one command's usage and whole help, whichever
 * half it is in. */
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

/* Categories [from, to): each one's commands, the usage and the first
 * line of the help. */
static void list(unsigned from, unsigned to)
{
    for (unsigned k = from; k < to; k++) {
        sh_say("\033[1m%s\033[0m\n", sh_categories[k]);
        const struct sh_cmd *c;
        for (size_t i = 0; (c = sh_cmd_at(i)); i++) {
            if (c->cat != k)
                continue;
            const char *h = c->help;
            size_t hl = strchr(h, '\n') ? (size_t)(strchr(h, '\n') - h) : strlen(h);
            sh_say("  %-28s %.*s\n", c->usage, (int)hl, h);
        }
    }
}

SH_CMD(help)
{
    if (argc == 2 && !strcmp(argv[1], "dev")) {
        sh_say("Commands: for developers (help for the everyday ones; help <command> for one)\n");
        list(sh_neveryday, sh_ncategories);
        sh_say("Test programs in /boot/bin run by name: utest, usbtest, contest, perop, fbbench,\n"
               "wltest, fractal, mixramp, dnstest, nettest, tcptest, updtest. Boot words and the\n"
               "boot menu's Developer entries: docs/TESTING.md.\n");
        return 0;
    }
    if (argc > 1)
        return help_on(argc, argv);
    sh_say("Commands: by category (help <command> for its usage and details)\n");
    list(0, sh_neveryday);
    sh_say("Lines: a ; b   a && b   a || b   a | b | c   NAME=value   $NAME   'quotes'  # comment\n"
           "Keys: Tab completes, left/right/home/end, backspace/delete, up/down history,\n"
           "Ctrl+C cancel, Ctrl+L clear, Shift+PageUp/PageDown scroll back.\n"
           "Aliases: ll (ls -l). help dev: the commands for developers (tests, hardware, the\n"
           "kernel).\n");
    return 0;
}
