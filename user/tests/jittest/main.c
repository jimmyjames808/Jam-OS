/* jittest: JIT memory from a POSIX program (docs/CR-PLAN.md section 12,
 * track CJ2): libc's mmap with MAP_JIT, the one way a program writes
 * machine code and runs it. Its list (`wants`) says `right jit`, so the
 * shell (and `allow`, for a copy on /data) gives it RIGHT_ROOT_JIT.
 *
 *   jittest           with the power: plain memory still refuses execute;
 *                     a MAP_JIT page written, run, patched and run again,
 *                     switched between writable and executable with
 *                     mprotect; a MAP_JIT reservation (V8's pattern:
 *                     PROT_NONE, then pages made writable and executable),
 *                     a MAP_FIXED | MAP_JIT inside it decommitting in
 *                     place, a MAP_FIXED without MAP_JIT making a page plain
 *                     memory again; ps's mark (PROCESS_FLAG_JIT)
 *   jittest without   without the power (a child posixtest starts, which
 *                     gets no root): MAP_JIT is EPERM, execute EACCES
 *   jittest hold      the first page made and run, "jittest: holding JIT
 *                     memory", then waits for a signal (for `ps`)
 *
 * Ends "jittest: PASS" (exit 0) or with a FAILED line for each check that
 * didn't hold (exit 1). The kernel's half is kernel/test/test_vmo_jit.c,
 * the raw calls' utest's jit.c. */
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

static bool holds_power(void)
{
    handle_t root = startup_handle(SR_RESOURCE);
    struct handle_info hi;
    return root && jam_handle_info(root, &hi) == OK && (hi.rights & RIGHT_ROOT_JIT);
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

static void with(void)
{
    CHECK(holds_power());
    CHECK(!marked());
    plain();
    char *p = first_page();
    if (p) {
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
    CHECK(mprotect(q, 2 * PG, RWX) == 0);
    emit(q, 46);
    CHECK(run(q) == 46);
    /* MAP_FIXED | MAP_JIT inside it: decommitted in place, JIT memory still. */
    CHECK(map(q, PG, RW, ANON | MAP_FIXED | MAP_JIT) == q);
    CHECK(q[0] == 0);
    emit(q, 47);
    CHECK(mprotect(q, PG, RX) == 0);
    CHECK(run(q) == 47);
    /* MAP_FIXED without MAP_JIT: plain memory, which never executes. */
    CHECK(map(q, PG, RW, ANON | MAP_FIXED) == q);
    CHECK(q[0] == 0);
    CHECK(mprotect(q, PG, RX) == -1 && errno == EACCES);
    CHECK(mprotect(q, 2 * PG, RWX) == -1 && errno == EACCES);   /* half plain */
    emit(q + PG, 48);   /* the next page is JIT memory still */
    CHECK(run(q + PG) == 48);
    CHECK(madvise(q + PG, PG, MADV_DONTNEED) == 0);
    CHECK(q[PG] == 0);
    CHECK(munmap(r, RESERVE) == 0);
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
    } else if (argc == 1) {
        if (!holds_power())
            printf("jittest: no JIT power (its list's `right jit` wasn't given)\n");
        with();
    } else {
        fprintf(stderr, "usage: jittest [without | hold]\n");
        return 2;
    }
    printf("jittest: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
