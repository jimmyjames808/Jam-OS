/* A POSIX program's start, from crt1's _start_c (docs/M13-PLAN.md
 * section 1, "Startup"): what a Linux kernel would have put on the stack,
 * made from what a Jam OS process starts with.
 *
 *   1. The startup message (libjam's startup_read): argv, the
 *      environment, the handles by role.
 *   2. The POSIX hand-over that may follow it (<posixfds.h>,
 *      posixfds_read): the descriptors and the state exec keeps. Without
 *      one, descriptors 0 to 2 are the defaults (stdfd_default); a
 *      malformed one leaves the program with no descriptors and a line in
 *      the log.
 *   3. Linux's block, on this function's stack (which lives as long as
 *      the program: __libc_start_main never returns): argc, argv, NULL,
 *      the environment (with TZ added from the system's zone if the
 *      starter gave none), NULL, and the auxiliary vector: AT_PAGESZ,
 *      AT_RANDOM (16 bytes from the kernel's generator), AT_HWCAP,
 *      AT_PHDR/AT_PHENT/AT_PHNUM (the program headers, which
 *      user/posix/posix.ld maps, so musl's __init_tls finds PT_TLS),
 *      AT_ENTRY, the user and group ids (the one user's, 1000) and
 *      AT_SECURE 0.
 *   4. musl's __libc_start_main, unmodified: TLS, the thread pointer,
 *      the init functions, main, exit.
 *
 * Steps 1 to 3 run before musl's library is set up: no thread pointer,
 * so no errno, no malloc and no printf here (libjam's startup_read and
 * posixfds_read use none either). */
#include <elf.h>
#include <string.h>
#include "start.h"

#define AUX_MAX   16                    /* auxv pairs, AT_NULL's included */
#define STRS_MAX  128                   /* libjam's limit on argv + environment strings */
#define BLOCK_MAX (1 + STRS_MAX + 1 + 1 + 1 + 2 * AUX_MAX)   /* + TZ */
#define ONE_USER  1000                  /* the one user's uid and gid (M13 Q7) */

int __libc_start_main(int (*)(), int, char **, void (*)(void), void (*)(void), void (*)(void));

/* The ELF header, mapped with the program headers at the start of the
 * first segment (posix.ld); the linker defines the name. */
extern const Elf64_Ehdr __ehdr_start __attribute__((visibility("hidden")));

static handle_t self_process, self_vmar;
static struct posixfds_state ho_state;
static char ho_cwd[POSIXFDS_PATH_MAX];
static bool ho_given;
static uint8_t at_random[16];
static char tz_env[80] = "TZ=";

handle_t lx_self_process(void)
{
    return self_process;
}

handle_t lx_self_vmar(void)
{
    return self_vmar;
}

const struct posixfds_state *lx_handover(const char **cwd)
{
    if (!ho_given)
        return NULL;
    *cwd = ho_cwd;
    return &ho_state;
}

static status_t sink_fd(void *ctx, const struct posixfds_fd *rec, const void *data,
                        const handle_t *hs)
{
    (void)ctx;
    return stdfd_take(rec, data, hs);
}

static void sink_reset(void *ctx)
{
    (void)ctx;
    stdfd_reset();
}

/* "posix: <what> (<status>)\n" in the log, without printf. */
static void say(const char *what, status_t st)
{
    const char *parts[] = { "posix: ", what, " (", status_str(st), ")\n" };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        (void)jam_debug_write(parts[i], strlen(parts[i]));   /* the log: nothing to do if not */
}

/* Step 2: the hand-over, or the default descriptors. */
static void take_handover(void)
{
    const struct posixfds_sink sink = { NULL, sink_fd, sink_reset };
    status_t st = posixfds_read(startup_channel(), &sink, &ho_state, ho_cwd);
    if (st == ERR_NOT_FOUND) {
        stdfd_default();
        return;
    }
    if (st != OK) {
        say("the hand-over message was refused: no descriptors", st);
        return;
    }
    ho_given = true;
}

static bool has_tz(char *const *envp)
{
    for (; *envp; envp++)
        if (!strncmp(*envp, "TZ=", 3))
            return true;
    return false;
}

static uint64_t hwcap(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return d;   /* Linux's AT_HWCAP on x86: CPUID leaf 1's EDX */
}

/* The auxiliary vector into aux (AUX_MAX pairs at most); its length. */
static unsigned fill_aux(long *aux)
{
    const Elf64_Ehdr *eh = &__ehdr_start;
    os_random(at_random, sizeof(at_random));
    const long pairs[][2] = {
        { AT_PAGESZ, 4096 },
        { AT_RANDOM, (long)(uintptr_t)at_random },
        { AT_HWCAP, (long)hwcap() },
        { AT_PHDR, (long)((uintptr_t)eh + eh->e_phoff) },
        { AT_PHENT, eh->e_phentsize },
        { AT_PHNUM, eh->e_phnum },
        { AT_ENTRY, (long)eh->e_entry },
        { AT_UID, ONE_USER }, { AT_EUID, ONE_USER }, { AT_GID, ONE_USER }, { AT_EGID, ONE_USER },
        { AT_SECURE, 0 },
        { AT_NULL, 0 },
    };
    _Static_assert(sizeof(pairs) / sizeof(pairs[0]) <= AUX_MAX, "auxv fits");
    memcpy(aux, pairs, sizeof(pairs));
    return (unsigned)(sizeof(pairs) / sizeof(pairs[0])) * 2;
}

_Noreturn void __jam_start(unsigned ch, int (*main)(), void (*init)(void), void (*fini)(void))
{
    int argc;
    char **argv;
    startup_read(ch, &argc, &argv);
    self_process = startup_handle(SR_SELF_PROCESS);
    self_vmar = startup_handle(SR_SELF_VMAR);
    take_handover();

    long block[BLOCK_MAX];
    unsigned n = 0;
    block[n++] = argc;
    for (int i = 0; i <= argc; i++)   /* argv and its NULL */
        block[n++] = (long)(uintptr_t)argv[i];
    char **envp = argv + argc + 1;
    for (char **e = envp; *e && n < 1 + STRS_MAX + 1; e++)
        block[n++] = (long)(uintptr_t)*e;
    if (!has_tz(envp)) {
        tz_posix(tz_env + 3, sizeof(tz_env) - 3);
        block[n++] = (long)(uintptr_t)tz_env;
    }
    block[n++] = 0;
    n += fill_aux(&block[n]);
    __libc_start_main(main, argc, (char **)&block[1], init, fini, NULL);
    jam_process_exit(127);   /* it never returns */
}
