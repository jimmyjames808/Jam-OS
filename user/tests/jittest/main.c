/* jittest: JIT memory from a POSIX program (docs/CR-PLAN.md section 12,
 * tracks CJ2 and CR-e): libc's mmap with MAP_JIT, the one way a program
 * writes machine code and runs it. Its list (`wants`) says `right jit`, so
 * the shell (and `allow`, for a copy on /data) gives it the JIT power
 * alone, SR_JIT (its root, SR_RESOURCE, never has RIGHT_ROOT_JIT).
 *
 *   jittest           with the power: plain memory still refuses execute;
 *                     a MAP_JIT page written, run, patched and run again,
 *                     switched between writable and executable with
 *                     mprotect; a MAP_JIT reservation (V8's pattern:
 *                     PROT_NONE, then pages made writable and executable),
 *                     a MAP_FIXED inside it, with MAP_JIT or without,
 *                     decommitting in place (JIT memory still: Linux's
 *                     decommit, V8's WebAssembly code space), a shared
 *                     MAP_FIXED making a page plain memory; ps's mark
 *                     (PROCESS_FLAG_JIT); a fork, whose child isn't marked
 *                     and faults at the JIT page; the power handed on
 *                     (docs/FORK-PLAN.md Q4 (A)): a forked child keeps it,
 *                     and so does this program exec'd again from there
 *                     (/proc/self/exe, as Chromium starts its children);
 *                     another program exec'd in place, or this one started
 *                     by posix_spawn, gets none; PR_SET_MDWE in a child
 *                     gives it up for good, there and in what it starts
 *   jittest without   without the power (a child posixtest starts, which
 *                     gets no root): MAP_JIT is EPERM, execute EACCES
 *   jittest hold      the first page made and run, "jittest: holding JIT
 *                     memory", then waits for a signal (for `ps`)
 *   jittest power     exits 0 if it holds the power and MAP_JIT memory
 *                     runs, 1 if it holds none (MAP_JIT EPERM, PR_GET_MDWE
 *                     says refused), 2 for anything in between
 *
 * Ends "jittest: PASS" (exit 0) or with a FAILED line for each check that
 * didn't hold (exit 1). The kernel's half is kernel/test/test_vmo_jit.c,
 * the raw calls' utest's jit.c. */
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_MDWE   /* Linux 6.3's; musl 1.2.6's <sys/prctl.h> has none */
#define PR_SET_MDWE 65
#define PR_GET_MDWE 66
#define PR_MDWE_REFUSE_EXEC_GAIN 1
#endif

#define PG      4096ul
#define RESERVE (64ul << 20)
#define RW      (PROT_READ | PROT_WRITE)
#define RX      (PROT_READ | PROT_EXEC)
#define RWX     (PROT_READ | PROT_WRITE | PROT_EXEC)
#define ANON    (MAP_PRIVATE | MAP_ANONYMOUS)

static int fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("jittest: FAILED line %d: %s\n", __LINE__, #c);       \
        }                                                                \
    } while (0)

typedef int (*ret_fn)(void);

/* "mov eax, imm32; ret" at code: a function returning n. */
static void emit(void *code, int32_t n)
{
    uint8_t *c = code;
    c[0] = 0xb8;
    memcpy(c + 1, &n, 4);
    c[5] = 0xc3;
}

static int run(void *code)
{
    return ((ret_fn)code)();
}

/* mmap's result, or NULL with errno kept. */
static void *map(void *at, size_t len, int prot, int flags)
{
    void *p = mmap(at, len, prot, flags, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static bool refused(void *p, int err)
{
    return !p && errno == err;
}

extern char **environ;

static bool holds_power(void)
{
    handle_t power = startup_handle(SR_JIT);
    struct handle_info hi;
    return power && jam_handle_info(power, &hi) == OK && (hi.rights & RIGHT_ROOT_JIT);
}

/* The root never holds the JIT power: it comes alone, as SR_JIT. */
static bool root_without_power(void)
{
    handle_t root = startup_handle(SR_RESOURCE);
    struct handle_info hi;
    return !root || (jam_handle_info(root, &hi) == OK && !(hi.rights & RIGHT_ROOT_JIT));
}

static bool marked(void)
{
    struct process_info pi;
    return jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) == OK &&
           (pi.flags & PROCESS_FLAG_JIT);
}

/* What holds with or without the power: plain memory never executes, and
 * MAP_JIT is only for private anonymous memory. */
static void plain(void)
{
    CHECK(refused(map(NULL, PG, RWX, ANON), EACCES));
    CHECK(refused(map(NULL, PG, RX, ANON), EACCES));
    char *p = map(NULL, PG, RW, ANON);
    CHECK(p);
    if (p) {
        CHECK(mprotect(p, PG, RX) == -1 && errno == EACCES);
        CHECK(mprotect(p, PG, RWX) == -1 && errno == EACCES);
        CHECK(munmap(p, PG) == 0);
    }
    CHECK(refused(map(NULL, PG, RWX, MAP_SHARED | MAP_ANONYMOUS | MAP_JIT), EINVAL));
    int fd = memfd_create("jittest", 0);   /* a file's pages are never JIT memory */
    CHECK(fd >= 0 && ftruncate(fd, PG) == 0);
    if (fd >= 0) {
        CHECK(mmap(NULL, PG, RW, MAP_PRIVATE | MAP_JIT, fd, 0) == MAP_FAILED && errno == EINVAL);
        close(fd);
    }
}

static void without(void)
{
    CHECK(!holds_power());
    CHECK(root_without_power());
    CHECK(prctl(PR_GET_MDWE, 0, 0, 0, 0) == PR_MDWE_REFUSE_EXEC_GAIN);
    plain();
    CHECK(refused(map(NULL, PG, RWX, ANON | MAP_JIT), EPERM));
    CHECK(refused(map(NULL, PG, RW, ANON | MAP_JIT), EPERM));
    CHECK(refused(map(NULL, RESERVE, PROT_NONE, ANON | MAP_NORESERVE | MAP_JIT), EPERM));
    CHECK(!marked());
    if (!fails)
        printf("jittest: no JIT power: MAP_JIT refused (EPERM)\n");
}

/* One MAP_JIT page: written, run, patched, run; then switched between
 * writable and executable (a stricter JIT's way) and back to both. */
static char *first_page(void)
{
    char *p = map(NULL, PG, RWX, ANON | MAP_JIT);
    CHECK(p);
    if (!p)
        return NULL;
    emit(p, 42);
    CHECK(run(p) == 42);
    emit(p, 43);
    CHECK(run(p) == 43);
    CHECK(marked());
    return p;
}

/* A fork with p, a writable JIT page, mapped: the child isn't marked and
 * has nothing at p (a writable JIT mapping never reaches a fork's child:
 * docs/FORK-PLAN.md section 1.10), so running it there is a fault that
 * ends it; ours still runs. The child can't print (its descriptors are
 * the parent's until FL2): its exit code says what failed. */
static void forked(char *p)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (marked())
            _exit(3);
        (void)run(p);
        _exit(4);
    }
    CHECK(pid > 0);
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st));   /* not exit 3 (marked) nor 4 (it ran) */
    if (!WIFSIGNALED(st))
        printf("jittest: the forked child's wait status %#x\n", st);
    CHECK(marked());
    CHECK(run(p) == 43);
}

static void with(void)
{
    CHECK(holds_power());
    CHECK(root_without_power());
    CHECK(!marked());
    plain();
    char *p = first_page();
    if (p) {
        forked(p);
        CHECK(mprotect(p, PG, RX) == 0);
        CHECK(run(p) == 43);
        CHECK(mprotect(p, PG, RW) == 0);
        emit(p, 44);
        CHECK(mprotect(p, PG, RX) == 0);
        CHECK(run(p) == 44);
        CHECK(mprotect(p, PG, RWX) == 0);
        emit(p, 45);
        CHECK(run(p) == 45);
        CHECK(munmap(p, PG) == 0);
    }

    /* A reservation, as V8's page allocator makes one for code. */
    char *r = map(NULL, RESERVE, PROT_NONE, ANON | MAP_NORESERVE | MAP_JIT);
    CHECK(r);
    if (!r)
        return;
    char *q = r + RESERVE / 2;
    CHECK(mprotect(q, 3 * PG, RWX) == 0);
    emit(q, 46);
    CHECK(run(q) == 46);
    /* MAP_FIXED | MAP_JIT inside it: decommitted in place, JIT memory still. */
    CHECK(map(q, PG, RW, ANON | MAP_FIXED | MAP_JIT) == q);
    CHECK(q[0] == 0);
    emit(q, 47);
    CHECK(mprotect(q, PG, RX) == 0);
    CHECK(run(q) == 47);
    /* MAP_FIXED without MAP_JIT (PartitionAlloc's decommit under V8's
     * WebAssembly code space: PROT_NONE over it, then execute again):
     * decommitted in place, JIT memory still. */
    CHECK(map(q, PG, PROT_NONE, ANON | MAP_FIXED) == q);
    CHECK(mprotect(q, PG, RW) == 0);
    CHECK(q[0] == 0);
    emit(q, 48);
    CHECK(mprotect(q, PG, RX) == 0);
    CHECK(run(q) == 48);
    /* Shared memory over it: plain memory, which never executes. */
    CHECK(map(q, PG, RW, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED) == q);
    CHECK(q[0] == 0);
    CHECK(mprotect(q, PG, RX) == -1 && errno == EACCES);
    CHECK(mprotect(q, 2 * PG, RWX) == -1 && errno == EACCES);   /* half plain */
    /* Over the two (one plain, one JIT): not all JIT memory, so plain. */
    CHECK(map(q, 2 * PG, RW, ANON | MAP_FIXED) == q);
    CHECK(mprotect(q + PG, PG, RX) == -1 && errno == EACCES);
    emit(q + 2 * PG, 50);   /* the next page is JIT memory still */
    CHECK(run(q + 2 * PG) == 50);
    CHECK(madvise(q + 2 * PG, PG, MADV_DONTNEED) == 0);
    CHECK(q[2 * PG] == 0);
    CHECK(munmap(r, RESERVE) == 0);
}

/* "jittest power": 0 if we hold the power and MAP_JIT memory runs, 1 if
 * we hold none (MAP_JIT EPERM, PR_GET_MDWE in force), 2 for anything in
 * between. */
static int power_check(void)
{
    long mdwe = prctl(PR_GET_MDWE, 0, 0, 0, 0);
    char *p = map(NULL, PG, RWX, ANON | MAP_JIT);
    if (!p)
        return errno == EPERM && mdwe == PR_MDWE_REFUSE_EXEC_GAIN && !holds_power() ? 1 : 2;
    emit(p, 49);
    bool ok = run(p) == 49 && mdwe == 0 && holds_power() && root_without_power();
    munmap(p, PG);
    return ok ? 0 : 2;
}

/* A child forked to do how; its exit code (or 99 + its signal, or -1). */
enum { KEEPS, EXEC_SELF, EXEC_OTHER, GIVES_UP };

static int forked_child(int how)
{
    char *av[] = { "jittest", "power", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        if (how == KEEPS)
            _exit(power_check());
        if (how == EXEC_SELF)
            execv("/proc/self/exe", av);
        if (how == EXEC_OTHER)
            execv("/bin/jittest", av);   /* this file by its name: another program to exec */
        if (how == GIVES_UP) {
            /* Given up for good: here, in a child forked after and in
             * this program exec'd again; never undone. */
            if (prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0) != 0 ||
                prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0) != 0)
                _exit(10);
            if (prctl(PR_SET_MDWE, 0, 0, 0, 0) != -1 || errno != EPERM)
                _exit(11);
            if (power_check() != 1)
                _exit(12);
            pid_t g = fork();
            if (g == 0)
                _exit(power_check());
            int st = 0;
            if (g < 0 || waitpid(g, &st, 0) != g || !WIFEXITED(st) || WEXITSTATUS(st) != 1)
                _exit(13);
            execv("/proc/self/exe", av);   /* its status is the exec'd one's: 1 */
        }
        _exit(20);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99 + WTERMSIG(st);
}

/* The power handed on (docs/FORK-PLAN.md section 1.10, Q4 (A)). */
static void handed_on(void)
{
    int r;
    CHECK(prctl(PR_GET_MDWE, 0, 0, 0, 0) == 0);
    CHECK(prctl(PR_SET_MDWE, 2, 0, 0, 0) == -1 && errno == EINVAL);   /* NO_INHERIT alone */
    CHECK(prctl(PR_SET_MDWE, 3, 0, 0, 0) == -1 && errno == EINVAL);   /* no child free of it */
    CHECK(prctl(PR_SET_MDWE, 0, 0, 0, 0) == 0);                      /* not in force: nothing */
    CHECK(prctl(PR_GET_MDWE, 1, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK((r = forked_child(KEEPS)) == 0);
    if (r != 0)
        printf("jittest: a forked child: %d\n", r);
    CHECK((r = forked_child(EXEC_SELF)) == 0);
    if (r != 0)
        printf("jittest: /proc/self/exe exec'd in a forked child: %d\n", r);
    CHECK((r = forked_child(EXEC_OTHER)) == 1);
    if (r != 1)
        printf("jittest: /bin/jittest exec'd in a forked child: %d\n", r);
    CHECK((r = forked_child(GIVES_UP)) == 1);
    if (r != 1)
        printf("jittest: a child giving it up: %d\n", r);
    pid_t pid = -1;
    char *av[] = { "jittest", "power", NULL };
    int st = 0;
    CHECK(posix_spawn(&pid, "/proc/self/exe", NULL, NULL, av, environ) == 0);
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 1);
    CHECK(holds_power() && prctl(PR_GET_MDWE, 0, 0, 0, 0) == 0);   /* ours untouched */
    if (!fails)
        printf("jittest: the power handed on: a forked child and /proc/self/exe keep it, "
               "another program and posix_spawn get none, PR_SET_MDWE gives it up\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "without")) {
        without();
    } else if (argc == 2 && !strcmp(argv[1], "hold")) {
        if (first_page() && !fails) {
            printf("jittest: holding JIT memory\n");
            fflush(stdout);
            for (;;)
                pause();   /* until a signal ends it */
        }
    } else if (argc == 2 && !strcmp(argv[1], "power")) {
        return power_check();
    } else if (argc == 1) {
        if (!holds_power())
            printf("jittest: no JIT power (its list's `right jit` wasn't given)\n");
        with();
        handed_on();
    } else {
        fprintf(stderr, "usage: jittest [without | hold | power]\n");
        return 2;
    }
    printf("jittest: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
