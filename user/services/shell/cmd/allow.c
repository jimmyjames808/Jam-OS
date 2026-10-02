/* allow: the owner marks a program on /data runnable and grants it what it
 * asks for (sh_allow.c; docs/history/M8.6-SVC.md):
 *   allow <file>      show the file's list and ask y/n; y writes the approval
 *   allow -l          the approvals
 *   allow -r <name>   take away those whose path or file name is <name>
 * Only the shell init starts can: it alone holds a /data whose etc may
 * change and a root that can make code executable. */
#include <wants.h>
#include "sh.h"

#define ANSWER_WAIT (120 * NS_PER_S)   /* the owner's y or n */

/* The owner's answer: true for y, false for n, Ctrl+C or no answer. */
static bool ask(void)
{
    for (;;) {
        int k = sh_poll_key(now() + ANSWER_WAIT);
        if (k == 'y' || k == 'Y') {
            sh_say("y\n");
            return true;
        }
        if (k == 'n' || k == 'N' || k < 0 || sh_interrupted()) {
            sh_say("n\n");
            return false;
        }
    }
}

static void list_one(const char *hash, const char *path, const char *text, void *ctx)
{
    (void)ctx;
    sh_say("%s: %s (sha-256 %.16s...)\n", path, text, hash);
}

static int list(void)
{
    int n = sh_allow_each(list_one, NULL);
    if (n < 0) {
        sh_tty("allow: can't read /data/etc/allow (%s)\n", sh_why(n));
        return 1;
    }
    if (!n)
        sh_say("allow: no program on /data is allowed to run\n");
    return 0;
}

static int remove_named(const char *name)
{
    unsigned n = 0;
    status_t st = sh_allow_remove(name, &n);
    if (st != OK) {
        sh_tty("allow: can't change /data/etc/allow (%s)\n", sh_why(st));
        return 1;
    }
    if (!n) {
        sh_tty("allow: %s: not allowed (allow -l lists them)\n", name);
        return 1;
    }
    sh_say("allow: %s may no longer run\n", name);
    return 0;
}

static int allow_file(const char *arg)
{
    static struct wants w;   /* one command at a time */
    char abs[SH_PATH_MAX], hex[65];
    handle_t vmo;
    uint64_t size;
    if (!sh_resolve(arg, abs, sizeof(abs)) || !sh_on_data(abs)) {
        sh_tty("allow: %s: only a program on /data can be allowed (copy it to /data first)\n",
               arg);
        return 1;
    }
    status_t st = sh_program_file(abs, &vmo, &size, hex, &w);
    if (st != OK) {
        sh_tty("allow: %s: %s\n", arg, st == ERR_ACCESS_DENIED ?
               "only the shell init starts can allow programs" :
               st == ERR_INVALID_ARGS ? "not a program Jam OS runs, or its list is broken" :
               sh_why(st));
        return 1;
    }
    jam_handle_close(vmo);
    const char *bad = sh_wants_refused(&w);
    if (bad) {
        sh_tty("allow: %s: asks for %s, which no program from /data may have\n", arg, bad);
        return 1;
    }
    sh_say("allow %s:%s? y/n ", sh_basename(abs), w.text[0] ? w.text : "nothing but its terminal");
    sh_flush();
    if (!ask())
        return 1;
    st = sh_allow_add(abs, hex, w.text);
    if (st != OK) {
        sh_tty("allow: can't write /data/etc/allow (%s)%s\n", sh_why(st),
               st == ERR_ACCESS_DENIED ? ": only the shell init starts can allow programs" : "");
        return 1;
    }
    sh_say("allow: %s may run\n", abs);
    return 0;
}

SH_CMD(allow)
{
    if (argc == 2 && !strcmp(argv[1], "-l"))
        return list();
    if (argc == 3 && !strcmp(argv[1], "-r"))
        return remove_named(argv[2]);
    if (argc == 2 && argv[1][0] != '-')
        return allow_file(argv[1]);
    sh_tty("usage: allow <file> | allow -l | allow -r <name>\n");
    return 2;
}
