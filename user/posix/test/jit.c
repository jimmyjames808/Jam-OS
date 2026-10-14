/* posixtest's jit section: MAP_JIT (docs/CR-PLAN.md section 12, track
 * CJ2) from a program without the power. posixtest's list has no `right
 * jit`, so neither the shell nor init's run gives it RIGHT_ROOT_JIT:
 *
 *   - MAP_JIT is EPERM (writable and executable, writable, a PROT_NONE
 *     reservation), and execute on any other memory EACCES as ever;
 *   - MAP_JIT with MAP_SHARED, or over a descriptor, is EINVAL;
 *   - bin/jittest started by posix_spawn gets no root from us (a child
 *     gets nothing of its parent's powers), so its `without` mode finds
 *     MAP_JIT refused too, whatever its list says.
 *
 * With the power (a root that has it) the refusals are skipped, saying
 * so, and one MAP_JIT page is written and run instead. jittest's own run
 * with the power is the shell's (tools/shell-tests/jit.txt). */
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define PG   4096ul
#define RW   (PROT_READ | PROT_WRITE)
#define RWX  (PROT_READ | PROT_WRITE | PROT_EXEC)
#define ANON (MAP_PRIVATE | MAP_ANONYMOUS)

extern char **environ;

static bool refused(void *p, int err)
{
    return p == MAP_FAILED && errno == err;
}

static bool holds_power(void)
{
    handle_t root = startup_handle(SR_RESOURCE);
    struct handle_info hi;
    return root && jam_handle_info(root, &hi) == OK && (hi.rights & RIGHT_ROOT_JIT);
}

void pt_jit(char **argv)
{
    (void)argv;
    CHECK(refused(mmap(NULL, PG, RWX, ANON, -1, 0), EACCES));
    CHECK(refused(mmap(NULL, PG, RWX, MAP_SHARED | MAP_ANONYMOUS | MAP_JIT, -1, 0), EINVAL));
    int fd = memfd_create("pt-jit", 0);
    CHECK(fd >= 0 && ftruncate(fd, PG) == 0);
    if (fd >= 0) {
        CHECK(refused(mmap(NULL, PG, RW, MAP_PRIVATE | MAP_JIT, fd, 0), EINVAL));
        close(fd);
    }
    if (!holds_power()) {
        CHECK(refused(mmap(NULL, PG, RWX, ANON | MAP_JIT, -1, 0), EPERM));
        CHECK(refused(mmap(NULL, PG, RW, ANON | MAP_JIT, -1, 0), EPERM));
        CHECK(refused(mmap(NULL, 1ul << 30, PROT_NONE, ANON | MAP_NORESERVE | MAP_JIT, -1, 0),
                      EPERM));
    } else {
        printf("posixtest: jit: our root holds RIGHT_ROOT_JIT: the refusals skipped\n");
        unsigned char *p = mmap(NULL, PG, RWX, ANON | MAP_JIT, -1, 0);
        CHECK(p != MAP_FAILED);
        if (p != MAP_FAILED) {
            static const unsigned char ret7[] = { 0xb8, 7, 0, 0, 0, 0xc3 };   /* return 7 */
            memcpy(p, ret7, sizeof(ret7));
            CHECK(((int (*)(void))(void *)p)() == 7);
            CHECK(munmap(p, PG) == 0);
        }
    }
    /* A child started by posix_spawn: none of our powers, whatever its list. */
    pid_t pid = -1;
    char *av[] = { "jittest", "without", NULL };
    int status = 0;
    CHECK(posix_spawn(&pid, "/bin/jittest", NULL, NULL, av, environ) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
