/* ls: list directories (in columns on the screen, one a line in a pipe);
 * -l with each name's mode, links, owner, size and, for a symbolic link,
 * where it points ("lrwxrwxrwx  1 jam  jam  1 B  b -> a"). A volume that
 * keeps no modes (the stick's FAT) shows the ones it makes up. -a and -1
 * are accepted and change nothing. */
#include "sh.h"

/* Columns, as wide as the widest name. */
static void columns(const struct sh_dirent *e, int n)
{
    size_t w = 1;
    for (int i = 0; i < n; i++)
        if (strlen(e[i].name) + 1 > w)
            w = strlen(e[i].name) + 1;
    unsigned per = (unsigned)(96 / (w + 2));
    if (!per)
        per = 1;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(e[i].name) + e[i].dir;
        if (e[i].dir)
            sh_say("\033[94m%s/\033[0m", e[i].name);
        else
            sh_say("%s", e[i].name);
        bool last = (unsigned)(i + 1) % per == 0 || i + 1 == n;
        if (last)
            sh_say("\n");
        else
            sh_say("%*s", (int)(w + 2 - l), "");
    }
}

/* One long line: `name` is what is shown, abs where the file is. */
static void long_line(const char *abs, const char *name, bool tty)
{
    static char target[4097], arrow[4097 + 8];   /* one command at a time */
    struct fs_attr a;
    char mode[11], hs[24], o[12], g[12];
    arrow[0] = '\0';
    if (sh_getattr(abs, FS_NOFOLLOW, &a) != OK) {
        sh_say("??????????  ?  ?  ?  ?  %s\n", name);
        return;
    }
    sh_mode_str(a.mode, mode);
    bool dir = (a.mode & SH_MODE_FMT) == SH_MODE_DIR;
    if ((a.mode & SH_MODE_FMT) == SH_MODE_LNK && fs_readlink(abs, target, sizeof(target)) == OK)
        snprintf(arrow, sizeof(arrow), " -> %s", target);
    sh_say("%s %2u %-4s %-4s %10s  ", mode, (unsigned)a.nlink, sh_owner(a.uid, o, sizeof(o)),
           sh_owner(a.gid, g, sizeof(g)), sh_human(a.size, hs, sizeof(hs)));
    if (dir && tty)
        sh_say("\033[94m%s/\033[0m\n", name);
    else
        sh_say("%s%s%s\n", name, dir ? "/" : "", arrow);
}

static void entries(const char *dirabs, struct sh_dirent *e, int n, bool longf)
{
    bool tty = !sh_piped();
    if (!longf && tty) {
        columns(e, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        char abs[SH_PATH_MAX];
        if (longf && sh_join(dirabs, e[i].name, abs, sizeof(abs)))
            long_line(abs, e[i].name, tty);
        else if (e[i].dir && tty)
            sh_say("\033[94m%s/\033[0m\n", e[i].name);
        else
            sh_say("%s%s\n", e[i].name, e[i].dir ? "/" : "");
    }
}

/* One argument: a file as itself (a symbolic link too, with -l), a
 * directory's entries (with a heading when there are several arguments). 1
 * for a directory, 0 for a file, -1 if it doesn't exist (said). */
static int list_one(const char *arg, struct sh_dirent *e, bool longf, bool heading)
{
    char abs[SH_PATH_MAX];
    bool dir;
    uint64_t size;
    if (!sh_resolve(arg, abs, sizeof(abs))) {
        sh_tty("ls: %s: no such file or directory\n", arg);
        return -1;
    }
    struct fs_attr a;
    if (longf && sh_getattr(abs, FS_NOFOLLOW, &a) == OK && (a.mode & SH_MODE_FMT) == SH_MODE_LNK) {
        long_line(abs, arg, !sh_piped());
        return 0;
    }
    if (sh_stat(abs, &dir, &size) != OK) {
        sh_tty("ls: %s: no such file or directory\n", arg);
        return -1;
    }
    if (!dir) {
        if (longf)
            long_line(abs, arg, false);
        else
            sh_say("%s\n", arg);
        return 0;
    }
    if (heading)
        sh_say("%s:\n", arg);
    int n = sh_readdir(abs, e, SH_DIR_MAX);
    if (n > 0)
        entries(abs, e, n, longf);
    sh_readdir_cut("ls", abs, n, SH_DIR_MAX);
    return 1;
}

SH_CMD(ls)
{
    bool longf = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l')
                longf = true;
            else if (*p != 'a' && *p != '1') {
                sh_tty("usage: ls [-l] [path...]\n");
                return 2;
            }
        }
        first++;
    }
    struct sh_dirent *e = calloc(SH_DIR_MAX, sizeof(*e));
    if (!e)
        return 1;
    int st = 0;
    int nargs = argc - first;
    for (int i = first; i < argc || (i == first && nargs == 0); i++) {
        int r = list_one(nargs ? argv[i] : ".", e, longf, nargs > 1);
        if (r < 0)
            st = 1;
        else if (r > 0 && nargs > 1 && i + 1 < argc)
            sh_say("\n");
    }
    free(e);
    return st;
}
