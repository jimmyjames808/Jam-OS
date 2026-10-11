/* tinyssh's feature header for Jam OS, fixed: what its configure probes
 * (third_party/tinyssh's tryfeature.sh over has*.c) would find on musl
 * and the POSIX layer, written once instead of probed at every build
 * (cross-compiled, a probe can't run anyway). tinyssh.mk makes each of
 * the headers tinyssh includes (hasopenpty.h, hasutmp.h, ...) a line
 * that includes this file.
 *
 * Found: <limits.h> (musl's). Not found, and why:
 *   lib1305, lib25519, libntruprime, librandombytes   none are ported:
 *       tinyssh's own crypto is built (the *_tinyssh.c files), and its
 *       random bytes come from /dev/urandom (the layer's: the kernel's
 *       generator)
 *   libutil.h, util.h, openpty, login_tty   the session's terminal is
 *       init's (jamos_session_open), not a pseudo-terminal of tinysshd's:
 *       channel.c's override
 *   mlock   the layer has none (pages are never swapped: nothing to lock)
 *   utmp, utmpx   no login records (one user, no `who`): logsys.c is
 *       left out
 *   valgrind   not a target */
#pragma once

#define HASLIMITS 1
