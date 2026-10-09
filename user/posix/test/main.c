/* posixtest's main: the child modes, else the sections asked for (pt.h). */
#include <stdio.h>
#include <string.h>
#include "pt.h"

static int fails;

void pt_fail(const char *file, int line, const char *what)
{
    fails++;
    printf("posixtest: FAILED %s:%d: %s\n", file, line, what);
}

int pt_failures(void)
{
    return fails;
}

static const struct {
    const char *name;
    void (*run)(char **argv);
} sections[] = {
    { "start", pt_start },
    { "mem", pt_mem },
    { "thread", pt_thread },
    { "pipe", pt_pipe },
    { "fds", pt_fds },
    { "poll", pt_poll },
};

static void run(unsigned i, char **argv)
{
    int before = fails;
    sections[i].run(argv);
    printf("posixtest: %s: %s\n", sections[i].name, fails == before ? "ok" : "FAILED");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--child-start"))
        return pt_child_start(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-handover"))
        return pt_child_handover();
    if (argc >= 2 && !strcmp(argv[1], "--child-devetc"))
        return pt_child_devetc();
    if (argc >= 2 && !strcmp(argv[1], "--child-poll"))
        return pt_child_poll();
    setvbuf(stdout, NULL, _IOLBF, 0);   /* each line out as it is made */
    unsigned n = sizeof(sections) / sizeof(sections[0]);
    for (unsigned i = 0; i < n; i++) {
        bool asked = argc < 2;
        for (int a = 1; a < argc && !asked; a++)
            asked = !strcmp(argv[a], sections[i].name);
        if (asked)
            run(i, argv);
    }
    printf("posixtest: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
