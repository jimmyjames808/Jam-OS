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
    { "clock", pt_clock },
    { "thread", pt_thread },
    { "pipe", pt_pipe },
    { "fds", pt_fds },
    { "poll", pt_poll },
    { "proc", pt_proc },
    { "tty", pt_tty },
    { "files", pt_files },
    { "signal", pt_signal },
    { "socket", pt_socket },
    { "pty", pt_pty },
    { "procfs", pt_procfs },
    { "procinfo", pt_procinfo },
    { "self-image", pt_self_image },
    { "launch", pt_launch },
    { "session", pt_session },
    { "wayland", pt_wayland },
    { "scm", pt_scm },
    { "unix", pt_unix },
    { "peer", pt_peer },
    { "files-many", pt_files_many },
    { "sockets", pt_sockets },
    { "raw", pt_raw },
    { "mmap", pt_mmap },
    { "jit", pt_jit },
    { "mmap-big", pt_mmap_big },
    { "mmap-coherent", pt_mmap_coherent },
    { "mmap-tmp", pt_mmap_tmp },
    { "spawn-data", pt_spawn_data },
    { "loopback", pt_loopback },
    { "links", pt_links },
    { "execinfo", pt_execinfo },
    { "fault", pt_fault },
    { "fork", pt_fork },
    { "exec", pt_exec },
    { "efault", pt_efault },
    { "inotify", pt_inotify },
};

/* The sections run only when named (long ones). */
static const char *const asked_only[] = { "files-many", "sockets", "session", "mmap-big",
                                          "mmap-coherent", "mmap-tmp", "spawn-data", "loopback",
                                          "self-image", "links" };

static bool by_default(const char *name)
{
    for (unsigned i = 0; i < sizeof(asked_only) / sizeof(asked_only[0]); i++)
        if (!strcmp(name, asked_only[i]))
            return false;
    return true;
}

static void run(unsigned i, char **argv)
{
    int before = fails;
    sections[i].run(argv);
    printf("posixtest: %s: %s\n", sections[i].name, fails == before ? "ok" : "FAILED");
}

int main(int argc, char **argv)
{
    if (argc < 1)
        return PT_NO_ARGS;   /* the launch section's malformed SR_ARGS children */
    if (argc >= 2 && !strcmp(argv[1], "--child-start"))
        return pt_child_start(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-handover"))
        return pt_child_handover();
    if (argc >= 2 && !strcmp(argv[1], "--child-devetc"))
        return pt_child_devetc();
    if (argc >= 2 && !strcmp(argv[1], "--child-poll"))
        return pt_child_poll();
    if (argc >= 2 && !strcmp(argv[1], "--child-proc"))
        return pt_child_proc(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-procinfo"))
        return pt_child_procinfo(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-tty"))
        return pt_child_tty();
    if (argc >= 2 && !strcmp(argv[1], "--child-tty-handover"))
        return pt_child_tty_handover();
    if (argc >= 2 && !strcmp(argv[1], "--child-tty-sig"))
        return pt_child_tty_sig();
    if (argc >= 2 && !strcmp(argv[1], "--child-tty-spin"))
        return pt_child_tty_spin();
    if (argc >= 2 && !strcmp(argv[1], "--child-sig"))
        return pt_child_sig(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-files"))
        return pt_child_files();
    if (argc >= 2 && !strcmp(argv[1], "--child-rofs"))
        return pt_child_rofs();
    if (argc >= 2 && !strcmp(argv[1], "--child-sock"))
        return pt_child_sock();
    if (argc >= 2 && !strcmp(argv[1], "--child-pty"))
        return pt_child_pty();
    if (argc >= 2 && !strcmp(argv[1], "--child-winsize"))
        return pt_child_winsize();
    if (argc >= 2 && !strcmp(argv[1], "--child-hup"))
        return pt_child_hup();
    if (argc >= 2 && !strcmp(argv[1], "--child-raw"))
        return pt_child_raw(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-procfs"))
        return pt_child_procfs(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-launch"))
        return pt_child_launch(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-mmap-pull"))
        return pt_child_mmap_pull(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-wl"))
        return pt_child_wl(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-scm"))
        return pt_child_scm();
    if (argc >= 2 && !strcmp(argv[1], "--child-unix"))
        return pt_child_unix(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-peer"))
        return pt_child_peer(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-links"))
        return pt_child_links();
    if (argc >= 2 && !strcmp(argv[1], "--child-fault"))
        return pt_child_fault(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-fork"))
        return pt_child_fork(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-notice"))
        return pt_child_notice(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-efault"))
        return pt_child_efault(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-exec"))
        return pt_child_exec(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-inotify"))
        return pt_child_inotify(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* each line out as it is made */
    unsigned n = sizeof(sections) / sizeof(sections[0]);
    for (unsigned i = 0; i < n; i++) {
        bool asked = argc < 2 && by_default(sections[i].name);
        for (int a = 1; a < argc && !asked; a++)
            asked = !strcmp(argv[a], sections[i].name);
        if (asked)
            run(i, argv);
    }
    printf("posixtest: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
