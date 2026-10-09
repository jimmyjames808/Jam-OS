/* posixtest (user/posix/test/): the POSIX layer's own test program, a
 * POSIX program built with tools/jamos-cc like any port. One section per
 * area of the layer (docs/M13-PLAN.md, the tracks' tests); later tracks
 * add theirs. `posixtest` runs every section, `posixtest <section>...`
 * the ones named. It prints a line per section and ends with PASS or
 * FAIL; the exit code is 0 only if every check held.
 *
 * Files: main.c (sections, the child modes), start.c (argv, the
 * environment, the auxiliary vector, the hand-over: children started
 * with libjam's spawn), mem.c (mmap and friends, brk, malloc), thread.c
 * (pthreads, TLS, errno), proc.c (posix_spawn, the waits, the ids,
 * groups, execve, system and popen, the hand-over cut short). start.c
 * and proc.c include libjam (<jamos/libjam.h>); mem.c is the only one
 * with Linux's call numbers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A check: on failure a line naming it, and the run fails; the test goes
 * on (the rest of the section may say more). */
#define CHECK(c)                                    \
    do {                                            \
        if (!(c))                                   \
            pt_fail(__FILE__, __LINE__, #c);        \
    } while (0)

void pt_fail(const char *file, int line, const char *what);
/* Checks failed so far. */
int  pt_failures(void);

/* The sections, given main's argv (each returns when done; failures are
 * counted). */
void pt_start(char **argv);
void pt_mem(char **argv);
void pt_thread(char **argv);
void pt_proc(char **argv);

/* The child modes start.c's checks start this program in (main.c runs
 * them instead of the sections): each checks what it was given and exits
 * 0 if all of it was right. */
int  pt_child_start(int argc, char **argv);
int  pt_child_handover(void);
int  pt_child_proc(int argc, char **argv);

/* Pages charged to our job now (job_get_info; start.c). */
uint64_t pt_job_pages(void);
