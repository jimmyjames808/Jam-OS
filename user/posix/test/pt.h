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
 * (pthreads, TLS, errno), pipe.c (pipes and socketpair, and the pipe
 * bench), fds.c (dup and fcntl, memfd, eventfd, the one user, /dev and
 * /etc), poll.c (poll, select, epoll on pipes; a child started with
 * libjam's spawn), files.c, files2.c and fileskid.c (files and
 * directories on /data and /boot: ptfiles.h). start.c, fds.c, poll.c and
 * fileskid.c are the ones that include libjam (<jamos/libjam.h>: children
 * started with a hand-over; fileskid.c through the layer's fd.h); mem.c
 * the only one with Linux's call numbers. */
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
void pt_pipe(char **argv);
void pt_fds(char **argv);
void pt_poll(char **argv);
void pt_files(char **argv);
void pt_files_many(char **argv);   /* only when asked for by name */

/* The child modes start.c's checks start this program in (main.c runs
 * them instead of the sections): each checks what it was given and exits
 * 0 if all of it was right. */
int  pt_child_start(int argc, char **argv);
int  pt_child_handover(void);
int  pt_child_devetc(void);
int  pt_child_poll(void);
int  pt_child_files(void);
int  pt_child_rofs(void);

/* Pages charged to our job now (job_get_info; start.c). */
uint64_t pt_job_pages(void);
