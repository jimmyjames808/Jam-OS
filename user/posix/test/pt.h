/* posixtest (user/posix/test/): the POSIX layer's own test program, a
 * POSIX program built with tools/jamos-cc like any port. One section per
 * area of the layer (docs/M13-PLAN.md, the tracks' tests); later tracks
 * add theirs. `posixtest` runs every section, `posixtest <section>...`
 * the ones named. It prints a line per section and ends with PASS or
 * FAIL; the exit code is 0 only if every check held.
 *
 * Files: main.c (sections, the child modes), start.c (argv, the
 * environment, the auxiliary vector, the hand-over: children started
 * with libjam's spawn), mem.c (mmap and friends, brk, malloc) and
 * memres.c and mempat.c (reservations; V8's and PartitionAlloc's
 * patterns), clock.c
 * (the clocks from the time page against the calls, and their bench), thread.c
 * (pthreads, TLS, errno), pipe.c (pipes and socketpair, and the pipe
 * bench), fds.c (dup and fcntl, memfd, eventfd, the one user, /dev and
 * /etc), poll.c (poll, select, epoll on pipes; a child started with
 * libjam's spawn), proc.c (posix_spawn, the waits, the ids, groups,
 * execve, system and popen, the hand-over cut short), tty.c (a program on
 * a terminal: a connection console it drives itself), files.c, files2.c
 * and fileskid.c (files and directories on /data and /boot: ptfiles.h),
 * signal.c and sigproc.c (signals in a process, and between processes),
 * pty.c (pseudo-terminals, and init's sessions from a POSIX program),
 * clock.c (the clocks from the time page), socket.c (sockets without the
 * network), sockets.c (against the network peer: asked for by name),
 * loopback.c (127.0.0.0/8 through netstack alone: asked for by name),
 * procfs.c (libc's /proc: the program's file and the descriptors),
 * procinfo.c (/proc's generated files: stat, status, maps, cpuinfo, ...),
 * launch.c (Chromium's launcher's needs: a big argv, addclosefrom_np),
 * execinfo.c (backtrace and its lines: nested calls, a thread, a signal
 * handler, qsort's comparator), fault.c (a thread's own faults as Linux's
 * signals: siginfo, the ucontext, the flags, children that must end),
 * efault.c (a call given a bad pointer: EFAULT, and the process goes on),
 * mmap.c (files mapped on their pages, msync, posix_spawn from /data),
 * jit.c (MAP_JIT refused without the power, and to bin/jittest as our child),
 * wayland.c (the Wayland socket against a fake compositor of its own),
 * scm.c and scmkid.c (socketpairs carrying descriptors: SCM_RIGHTS, Mojo's
 * pattern between two processes, a fuzz loop; ptscm.h), unix.c (named
 * AF_UNIX sockets through /svc/unix: socket files and abstract names),
 * peer.c (socketpairs and connections fed by a hostile peer in another
 * process: scm's fuzz between two processes), fork.c (fork, vfork and
 * clone without CLONE_VM, each case in a job of its own; forkfd.c's and
 * forknote.c's cases too: descriptors, vfork's wait, a child's notices), exec.c (execve
 * in place from a forked child, each case in a job of its own), inotify.c
 * (the inotify descriptor against fake file services of its own) and
 * rawcalls.c, rawfutex.c and rawlog.c (the raw Linux calls Chromium makes, and the
 * ENOSYS log: the log's hooks are the layer's calls.h, a second look inside).
 * start.c, mem.c, memres.c (the job's counts), clock.c, fds.c, poll.c,
 * proc.c, tty.c, fileskid.c, wayland.c, jit.c, procfs.c, unix.c, peer.c,
 * fork.c, fault.c (a VMO shrunk under its mapping) and inotify.c are
 * the ones that include libjam (<jamos/libjam.h>: children started with a hand-over, a mapping libc
 * didn't make, the calls the clocks are checked against; fileskid.c
 * through the layer's fd.h); mem.c, rawcalls.c, rawfutex.c and rawlog.c include Linux's call
 * numbers. */
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
void pt_clock(char **argv);
void pt_thread(char **argv);
void pt_pipe(char **argv);
void pt_fds(char **argv);
void pt_poll(char **argv);
void pt_proc(char **argv);
void pt_tty(char **argv);
void pt_files(char **argv);
void pt_files_many(char **argv);   /* only when asked for by name */
void pt_signal(char **argv);
void pt_socket(char **argv);
void pt_sockets(char **argv);    /* only when asked for by name: the network peer */
void pt_loopback(char **argv);   /* only when asked for by name: needs netstack, no peer */
void pt_pty(char **argv);
void pt_session(char **argv);      /* only when asked for: a shell-mode init's sessions */
void pt_raw(char **argv);          /* raw calls and the ENOSYS log */
/* The raw section's second file (rawlog.c): the ENOSYS log. */
void pt_raw_log(void);
/* And the futex checks (rawfutex.c). */
void pt_raw_futex(void);
void pt_procfs(char **argv);
/* procinfo.c: the generated files of /proc (stat, status, maps, cpuinfo, ...). */
void pt_procinfo(char **argv);
int  pt_child_block(void);   /* its child: says so, waits for stdin to close, exits 5 */
/* Only when asked for (procfs.c): a program's own path and its
 * self-image, from /data (tools/posix-mmap-test.sh, case spawn). */
void pt_self_image(char **argv);
/* launch.c: a big argv and environment, addclosefrom_np. */
void pt_launch(char **argv);
void pt_mmap(char **argv);
/* Only when asked for (mmap.c): tools/posix-mmap-test.sh's cases. */
void pt_mmap_big(char **argv);
void pt_mmap_coherent(char **argv);
void pt_mmap_wshared(char **argv);
/* ... and tools/layout-test.sh's: the mmap section in /tmp, which must have a pager. */
void pt_mmap_tmp(char **argv);
void pt_spawn_data(char **argv);
void pt_wayland(char **argv);
void pt_jit(char **argv);          /* MAP_JIT without the power (jit.c) */
void pt_links(char **argv);        /* only when asked for: an ext volume at /u6 (links.c) */
void pt_execinfo(char **argv);     /* backtrace() and its lines (execinfo.c) */
void pt_fault(char **argv);        /* a thread's own faults as signals (fault.c) */
void pt_efault(char **argv);       /* a bad pointer to a call: EFAULT, on (efault.c) */
void pt_scm(char **argv);          /* socketpairs carrying descriptors (scm.c, scmkid.c) */
void pt_unix(char **argv);         /* named AF_UNIX sockets through /svc/unix (unix.c) */
void pt_peer(char **argv);         /* a hostile peer in another process (peer.c) */
void pt_fork(char **argv);         /* fork, vfork and clone without CLONE_VM (fork.c) */
void pt_exec(char **argv);         /* execve in place (exec.c) */
void pt_inotify(char **argv);      /* inotify against fake services, and the real ones (inotify.c) */
/* The signal section's second half (sigproc.c): between processes. */
void pt_sigproc(void);
/* The mem section's second half (memres.c): big sparse reservations. */
void pt_mem_reserve(void);

/* main's exit code when started with no arguments at all (argc 0: what a
 * malformed SR_ARGS leaves a program, launch.c's check). */
#define PT_NO_ARGS 9

/* The child modes start.c's checks start this program in (main.c runs
 * them instead of the sections): each checks what it was given and exits
 * 0 if all of it was right. */
int  pt_child_start(int argc, char **argv);
int  pt_child_handover(void);
int  pt_child_devetc(void);
int  pt_child_poll(void);
int  pt_child_proc(int argc, char **argv);
int  pt_child_procinfo(int argc, char **argv);   /* the process title, as base's setproctitle does it */
int  pt_child_tty(void);
int  pt_child_tty_handover(void);
int  pt_child_tty_sig(void);
int  pt_child_tty_spin(void);
int  pt_child_sig(int argc, char **argv);
int  pt_child_files(void);
int  pt_child_rofs(void);
int  pt_child_sock(void);
int  pt_child_pty(void);
int  pt_child_winsize(void);
int  pt_child_hup(void);
int  pt_child_raw(int argc, char **argv);
int  pt_child_procfs(int argc, char **argv);
int  pt_child_launch(int argc, char **argv);
int  pt_child_mmap_pull(int argc, char **argv);
int  pt_child_wl(int argc, char **argv);
int  pt_child_scm(void);
int  pt_child_unix(int argc, char **argv);
int  pt_child_peer(int argc, char **argv);
int  pt_child_links(void);
int  pt_child_fault(int argc, char **argv);   /* ends by a fault (fault.c says which) */
int  pt_child_fork(int argc, char **argv);    /* one of fork.c's cases, in a job of its own */
/* forkfd.c: fork.c's cases for a forked child's descriptors and mappings (FL2). */
void pt_fork_fds(void);
void pt_fork_chromium(void);
void pt_fork_ttys(void);
void pt_fork_maps(void);
/* forknote.c: fork.c's cases for vfork's wait and a child's notices (FL4),
 * and the program a junk case's child starts to forge one. */
void pt_fork_vfork(void);
void pt_fork_groups(void);
void pt_fork_junk(void);
int  pt_child_notice(int argc, char **argv);
int  pt_child_efault(int argc, char **argv);  /* exits, or EFAULT then a real fault (efault.c) */
/* exec.c: a case in a job of its own, a new image's checks, or utest's
 * program that execs at once. */
int  pt_child_exec(int argc, char **argv);
/* inotify.c: a spawned child, which must not have the inotify descriptor
 * its parent names. */
int  pt_child_inotify(int argc, char **argv);

/* Pages charged to our job now (job_get_info; start.c). */
uint64_t pt_job_pages(void);
/* Message bytes charged to our job now (JOB_LIMIT_MSG_BYTES; start.c). */
uint64_t pt_job_msg_bytes(void);
