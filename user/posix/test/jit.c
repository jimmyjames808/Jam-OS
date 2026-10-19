/* posixtest's jit section: MAP_JIT (docs/CR-PLAN.md section 12, track
 * CJ2) from a program without the power. posixtest's list has no `right
 * jit`, so neither the shell nor init's run gives it the JIT power
 * (SR_JIT), and its root never has RIGHT_ROOT_JIT:
 *
 *   - MAP_JIT is EPERM (writable and executable, writable, a PROT_NONE
 *     reservation), and execute on any other memory EACCES as ever;
 *   - MAP_JIT with MAP_SHARED, or over a descriptor, is EINVAL;
 *   - prctl's PR_GET_MDWE says memory-deny-write-execute is in force
 *     (PR_MDWE_REFUSE_EXEC_GAIN), PR_SET_MDWE with it is 0 and without it
 *     EPERM (never undone), other flags or arguments EINVAL;
 *   - bin/jittest started by posix_spawn gets no root from us (a child
 *     gets nothing of its parent's powers), so its `without` mode finds
 *     MAP_JIT refused too, whatever its list says.
 *
 * With the power the refusals are skipped, saying so, and one MAP_JIT page
 * is written and run instead. jittest's own run with the power is the
 * shell's (tools/shell-tests/jit.txt). */
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define PG   4096ul
#define RW   (PROT_READ | PROT_WRITE)
#define RWX  (PROT_READ | PROT_WRITE | PROT_EXEC)
#define ANON (MAP_PRIVATE | MAP_ANONYMOUS)

#ifndef PR_SET_MDWE   /* Linux 6.3's; musl 1.2.6's <sys/prctl.h> has none */
#define PR_SET_MDWE 65
#define PR_GET_MDWE 66
#define PR_MDWE_REFUSE_EXEC_GAIN 1
#endif

extern char **environ;

static bool refused(void *p, int err)
{
    return p == MAP_FAILED && errno == err;
}

static bool holds_power(void)
{
    handle_t power = startup_handle(SR_JIT);
    struct handle_info hi;
    return power && jam_handle_info(power, &hi) == OK && (hi.rights & RIGHT_ROOT_JIT);
}

static rights_t rights_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.rights : 0;
}

/* A page that holds nothing but this: read-only data, never run. */
static const unsigned char ro_page[PG] __attribute__((aligned(PG))) = { 0xc3 };

#define VR  (VMAR_READ)
#define VRW (VMAR_READ | VMAR_WRITE)
#define VRX (VMAR_READ | VMAR_EXEC)
#define VWX (VMAR_READ | VMAR_WRITE | VMAR_EXEC)

/* Every way to writable and executable memory a program without the power
 * has (docs/CR-REVIEW-JIT.md), by the raw calls under libc: its own code,
 * read-only data, stack and heap; a handle widened by a duplicate, a
 * replace or a send; the JIT call itself; the old way to code (sealed
 * first, when init's run gives us RIGHT_ROOT_VMEX). Every one refused, and
 * we are never marked. */
static void hostile(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), root = startup_handle(SR_RESOURCE);
    uint64_t code = (uint64_t)(uintptr_t)&hostile & ~(PG - 1);
    uint64_t ro = (uint64_t)(uintptr_t)ro_page;
    volatile unsigned char on_stack = 0;
    uint64_t stack = (uint64_t)(uintptr_t)&on_stack & ~(PG - 1);
    /* Our image: W^X, and no write right on it. */
    CHECK(jam_vmar_protect(vmar, code, PG, VWX) == ERR_INVALID_ARGS);
    CHECK(mprotect((void *)(uintptr_t)code, PG, RWX) == -1 && errno == EACCES);
    CHECK(jam_vmar_protect(vmar, ro, PG, VRW) == ERR_ACCESS_DENIED);
    CHECK(jam_vmar_protect(vmar, ro, PG, VWX) == ERR_INVALID_ARGS);
    CHECK(ro_page[0] == 0xc3);
    /* The stack and the heap: W^X, and no execute right on them. */
    CHECK(jam_vmar_protect(vmar, stack, PG, VWX) == ERR_INVALID_ARGS);
    CHECK(jam_vmar_protect(vmar, stack, PG, VRX) == ERR_ACCESS_DENIED);
    unsigned char *heap = mmap(NULL, PG, RW, ANON, -1, 0);
    CHECK(heap != MAP_FAILED);
    if (heap != MAP_FAILED) {
        heap[0] = 0xc3;
        CHECK(mprotect(heap, PG, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
        CHECK(jam_vmar_protect(vmar, (uint64_t)(uintptr_t)heap, PG, VWX) == ERR_INVALID_ARGS);
        CHECK(jam_vmar_protect(vmar, (uint64_t)(uintptr_t)heap, PG, VRX) == ERR_ACCESS_DENIED);
        /* Mapped over in place: refused before anything changed. */
        handle_t fresh;
        uint64_t at = (uint64_t)(uintptr_t)heap;
        CHECK(jam_vmo_create(PG, 0, &fresh) == OK);
        CHECK(jam_vmar_map(vmar, fresh, 0, PG, VWX | VMAR_FIXED | VMAR_REPLACE, &at) ==
              ERR_INVALID_ARGS);
        CHECK(jam_vmar_map(vmar, fresh, 0, PG, VRX | VMAR_FIXED | VMAR_REPLACE, &at) ==
              ERR_ACCESS_DENIED);
        CHECK(jam_handle_close(fresh) == OK);
        CHECK(refused(mmap(heap, PG, RWX, ANON | MAP_FIXED, -1, 0), EACCES));
        CHECK(heap[0] == 0xc3);
        CHECK(munmap(heap, PG) == 0);
    }
    /* No handle gains execute: a duplicate, a replace, a send. */
    handle_t v, d, c0, c1, x;
    CHECK(jam_vmo_create(PG, 0, &v) == OK);
    rights_t vr = rights_of(v);
    CHECK(!(vr & RIGHT_EXEC));
    CHECK(jam_handle_duplicate(v, vr | RIGHT_EXEC, &d) == ERR_INVALID_ARGS);
    CHECK(jam_handle_duplicate(v, RIGHT_SAME, &d) == OK);
    CHECK(jam_handle_replace(d, vr | RIGHT_EXEC, &x) == ERR_INVALID_ARGS);   /* d is gone */
    CHECK(jam_handle_close(d) == ERR_BAD_HANDLE);
    CHECK(jam_channel_create(&c0, &c1) == OK);
    CHECK(jam_handle_duplicate(v, RIGHT_SAME, &d) == OK);
    rights_t wider = vr | RIGHT_EXEC;
    CHECK(jam_channel_write_rights(c0, "x", 1, &d, &wider, 1) == ERR_INVALID_ARGS);
    CHECK(jam_handle_close(c0) == OK);
    CHECK(jam_handle_close(c1) == OK);
    uint64_t a = 0;
    CHECK(jam_vmar_map(vmar, v, 0, PG, VWX, &a) == ERR_INVALID_ARGS);
    CHECK(jam_vmar_map(vmar, v, 0, PG, VRX, &a) == ERR_ACCESS_DENIED);
    /* A copy-on-write child: a fresh VMO's rights, so no execute either. */
    handle_t kid;
    CHECK(jam_vmo_write(v, 0, "\xc3", 1) == OK);
    CHECK(jam_vmo_create_child(v, VMO_CHILD_SNAPSHOT, 0, PG, &kid) == OK);
    CHECK(!(rights_of(kid) & RIGHT_EXEC));
    CHECK(jam_vmar_map(vmar, kid, 0, PG, VRX, &a) == ERR_ACCESS_DENIED);
    CHECK(jam_handle_close(kid) == OK);
    /* The JIT call: no power on our root (if we have one), and a VMO is no
     * root. Each takes the handle. */
    CHECK(jam_handle_duplicate(v, RIGHT_SAME, &d) == OK);
    CHECK(jam_vmo_make_jit(d, d, &x) == ERR_WRONG_TYPE);
    if (root) {
        handle_t wide;
        CHECK(jam_handle_duplicate(root, rights_of(root) | RIGHT_ROOT_JIT, &wide) ==
              ERR_INVALID_ARGS);
        CHECK(jam_vmo_make_jit(v, root, &x) == ERR_ACCESS_DENIED);
    } else {
        CHECK(jam_vmo_make_jit(v, HANDLE_INVALID, &x) == ERR_BAD_HANDLE);
    }
    CHECK(jam_handle_close(v) == ERR_BAD_HANDLE);
    /* The old way, if our root can (init's run): sealed first, so code we
     * write runs only once nothing can write it again. */
    if (root && (rights_of(root) & RIGHT_ROOT_VMEX)) {
        CHECK(jam_vmo_create(PG, 0, &v) == OK);
        CHECK(jam_vmo_write(v, 0, "\xc3", 1) == OK);
        CHECK(jam_vmo_make_exec(v, root, &x) == OK);
        CHECK((rights_of(x) & (RIGHT_WRITE | RIGHT_EXEC)) == RIGHT_EXEC);
        CHECK(jam_vmar_map(vmar, x, 0, PG, VRW, &a) == ERR_ACCESS_DENIED);
        CHECK(jam_vmar_map(vmar, x, 0, PG, VWX, &a) == ERR_INVALID_ARGS);
        CHECK(jam_vmar_map(vmar, x, 0, PG, VRX, &a) == OK);
        CHECK(jam_vmar_protect(vmar, a, PG, VWX) == ERR_INVALID_ARGS);
        CHECK(jam_vmar_protect(vmar, a, PG, VRW) == ERR_ACCESS_DENIED);
        CHECK(jam_vmo_write(x, 0, "\x90", 1) == ERR_ACCESS_DENIED);
        CHECK(jam_vmo_make_jit(x, root, &d) == ERR_ACCESS_DENIED);   /* x is gone */
        CHECK(jam_vmar_unmap(vmar, a, PG) == OK);
    }
    struct process_info pi;
    CHECK(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) == OK &&
          !(pi.flags & PROCESS_FLAG_JIT));
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
        hostile();
        CHECK(refused(mmap(NULL, PG, RWX, ANON | MAP_JIT, -1, 0), EPERM));
        CHECK(refused(mmap(NULL, PG, RW, ANON | MAP_JIT, -1, 0), EPERM));
        CHECK(refused(mmap(NULL, 1ul << 30, PROT_NONE, ANON | MAP_NORESERVE | MAP_JIT, -1, 0),
                      EPERM));
        /* Memory-deny-write-execute: in force for every process without
         * the power, and never undone. */
        CHECK(prctl(PR_GET_MDWE, 0L, 0L, 0L, 0L) == PR_MDWE_REFUSE_EXEC_GAIN);
        CHECK(prctl(PR_SET_MDWE, (long)PR_MDWE_REFUSE_EXEC_GAIN, 0L, 0L, 0L) == 0);
        CHECK(prctl(PR_SET_MDWE, 0L, 0L, 0L, 0L) == -1 && errno == EPERM);
        CHECK(prctl(PR_SET_MDWE, 4L, 0L, 0L, 0L) == -1 && errno == EINVAL);
        CHECK(prctl(PR_SET_MDWE, (long)PR_MDWE_REFUSE_EXEC_GAIN, 1L, 0L, 0L) == -1 &&
              errno == EINVAL);
        CHECK(prctl(PR_GET_MDWE, 1L, 0L, 0L, 0L) == -1 && errno == EINVAL);
    } else {
        printf("posixtest: jit: we hold the JIT power: the refusals skipped\n");
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
