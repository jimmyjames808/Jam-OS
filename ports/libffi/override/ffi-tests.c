/* ffi-tests: libffi's own ffi_call tests as one program for Jam OS.
 *
 * libffi's testsuite (testsuite/libffi.call and the ffi_call tests of
 * libffi.complex) is a directory of small programs run by DejaGnu, which
 * Jam OS has no use for. build.sh compiles each of those test files into
 * this program with its `main` renamed (-Dmain=ffitest_<name>_main) and its
 * exit and abort renamed to the two functions below, and writes the list
 * of names (ffi-tests-list.h). A test passes if it returns 0 or calls
 * exit(0); it fails if it returns anything else, calls exit with another
 * value, or aborts (libffi's CHECK macro prints "Check failed" and aborts).
 *
 *     ffi-tests            run every test, saying only the failures
 *     ffi-tests -v         the same, with each test's own output
 *     ffi-tests <name>...  run only those tests, with their output
 *     ffi-tests -l         list the names
 *
 * The last line is "ffi-tests: N passed, M failed" and the exit status is
 * 0 only when M is 0. The tests are libffi's (MIT); this driver is Jam
 * OS's (BSD-2-Clause), as the rest of this port's files.
 *
 * Closures are not tested: they need memory that can be written and then
 * executed, which a Jam OS program does not have (libwayland only calls
 * through ffi_call). */
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jmp_buf leave;
static int     verdict;

void ffitest_exit(int status) __attribute__((noreturn));
void ffitest_abort(void) __attribute__((noreturn));

void ffitest_exit(int status)
{
    verdict = status;
    longjmp(leave, 1);
}

void ffitest_abort(void)
{
    verdict = 134;   /* what a shell reports for SIGABRT */
    longjmp(leave, 1);
}

/* Each test's renamed main: int main(void) or int main(), so unprototyped. */
#define T(id, name) extern int ffitest_##id##_main();
#include "ffi-tests-list.h"
#undef T

struct test {
    const char *name;
    int (*run)();
};

static const struct test tests[] = {
#define T(id, name) { name, ffitest_##id##_main },
#include "ffi-tests-list.h"
#undef T
};

#define NTESTS (sizeof tests / sizeof tests[0])

/* Run one test; its status. Its output goes to /dev/null unless verbose. */
static int run_one(const struct test *t, int verbose)
{
    int saved = -1;
    fflush(stdout);
    if (!verbose) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            saved = dup(1);
            dup2(nul, 1);
            close(nul);
        }
    }
    verdict = 0;
    if (setjmp(leave) == 0)
        verdict = t->run();
    fflush(stdout);
    if (saved >= 0) {
        dup2(saved, 1);
        close(saved);
    }
    return verdict;
}

int main(int argc, char **argv)
{
    int verbose = 0, named = 0, passed = 0, failed = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            for (size_t k = 0; k < NTESTS; k++)
                puts(tests[k].name);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else
            named++;
    }
    for (size_t k = 0; k < NTESTS; k++) {
        int wanted = named == 0;
        for (int i = 1; i < argc && !wanted; i++)
            wanted = strcmp(argv[i], tests[k].name) == 0;
        if (!wanted)
            continue;
        int status = run_one(&tests[k], verbose || named);
        if (status == 0) {
            passed++;
        } else {
            failed++;
            printf("FAIL %s (status %d)\n", tests[k].name, status);
        }
    }
    if (named && passed + failed < named) {
        printf("ffi-tests: %d of the %d names are not tests (ffi-tests -l)\n",
               named - passed - failed, named);
        return 2;
    }
    printf("ffi-tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
