/* A native program's start: crt0 (user/lib/crt0.S) calls libos_start with
 * the startup channel, which reads the startup message (startup_read,
 * start.c), points environ at its environment, runs main and exits with
 * what main returns. It is the C library's part of startup: libjam.a
 * leaves it out, and a POSIX program's crt does the same with musl's
 * environ and its __libc_start_main. */
#include <os.h>

char **environ;

_Noreturn void libos_start(handle_t ch)
{
    int argc;
    char **argv;
    startup_read(ch, &argc, &argv);
    environ = argv + argc + 1;
    jam_process_exit(main(argc, argv));
}
