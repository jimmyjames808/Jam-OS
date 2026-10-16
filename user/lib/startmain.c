/* A native program's start: crt0 (user/lib/crt0.S) calls libos_start with
 * the startup channel, which reads the startup message (startup_read,
 * start.c), points environ at its environment, runs main and exits with
 * what main returns. It is the C library's part of startup: libjam.a
 * leaves it out, and a POSIX program's crt does the same with musl's
 * environ and its __libc_start_main. Here too libos's C library sets its
 * fork hooks (<os.h> jam_fork_set_libc): its heap's lock and printf's
 * channel. */
#include <os.h>
#include "fork.h"

char **environ;

static void libos_fork_prepare(void *ctx)
{
    (void)ctx;
    heap_fork_prepare();
}

static void libos_fork_parent(void *ctx, status_t st)
{
    (void)ctx;
    (void)st;
    heap_fork_parent();
}

static void libos_fork_child(void *ctx, handle_t from)
{
    (void)ctx;
    (void)from;
    heap_fork_child();
    stdio_forked();
}

static const struct jam_fork_libc libos_fork = {
    .prepare = libos_fork_prepare, .parent = libos_fork_parent, .child = libos_fork_child,
};

_Noreturn void libos_start(handle_t ch)
{
    int argc;
    char **argv;
    jam_fork_set_libc(&libos_fork);
    startup_read(ch, &argc, &argv);
    environ = argv + argc + 1;
    jam_process_exit(main(argc, argv));
}
