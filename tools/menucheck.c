/* menucheck: the PC's check of a boot menu (<bootmenu.h>), on the Mac.
 * `make check` runs it on boot/limine.conf with the files a stick has
 * once `update -w` has written a build (its own and the previous one), so
 * a menu the PC's `update -w` would refuse fails the check here first;
 * tools/update-server.py runs it on the menu it serves and says the
 * verdict in its log.
 *
 *     menucheck <menu> [<file>...]
 *
 * Each <file> is a path on the ESP that is there ("/boot/jamos.elf");
 * every other file the menu names is taken as missing. Exit 0 if the menu
 * passes, 1 if not (the line and the reason on stdout), 2 on a usage or
 * read error. Built from user/lib/bootmenu.c as it is. */
#include <stdio.h>
#include <string.h>
#include <bootmenu.h>

struct files {
    int    n;
    char **paths;
};

static bool present(void *ctx, const char *path)
{
    const struct files *f = ctx;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->paths[i], path))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    static unsigned char text[BOOTMENU_MAX + 1];
    if (argc < 2) {
        fprintf(stderr, "usage: menucheck <menu> [<file on the ESP>...]\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(text, 1, sizeof(text), in);   /* one byte more: TOO_BIG */
    fclose(in);
    struct files f = { argc - 2, argv + 2 };
    struct bootmenu_result r;
    if (bootmenu_check(text, len, present, &f, &r)) {
        printf("menucheck: %s passes (%u entries boot)\n", argv[1], r.entries);
        return 0;
    }
    printf("menucheck: %s refused: line %u: %s%s%s\n", argv[1], r.line, bootmenu_why_str(r.why),
           r.path[0] ? ": " : "", r.path);
    return 1;
}
