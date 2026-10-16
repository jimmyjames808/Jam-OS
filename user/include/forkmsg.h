/* The fork message: libjam's first message on a forked child's startup
 * channel (docs/FORK-PLAN.md section 1.6). A forked child doesn't start at
 * _start and reads no startup message: it holds a copy of its parent's
 * memory, whose handle values name nothing in its own table, and resumes
 * where jam_fork (<os.h>) was called. Its first thread starts on fork's
 * trampoline (user/lib/fork_entry.S) with this channel in rdi, and before
 * fork returns 0 there, libjam reads this one message and makes its
 * handles the child's startup roles in place of the parent's (start.c's
 * table, as if they had come in a startup message); the C library's own
 * messages (the POSIX layer's descriptors, its table of mappings) follow it
 * on the same channel, written before the child starts.
 *
 * Which roles a child gets, and as what (duplicates only: never the root,
 * never a service's role), is fork.c's list. It is a hand-written format,
 * not IDL: its id in abi/idl/ids.txt is the top half of FORK_MAGIC.
 *
 * The parent is the same program, so the child trusts it as a program
 * trusts its starter, but checks the message all the same: a malformed one
 * (or none) ends the child with FORK_BAD_MESSAGE, its handles closed, as a
 * child whose every handle value is stale can't run. */
#pragma once

#include <stdint.h>
#include <jam/startup.h>

#define FORK_MAGIC        0x4b524f46u   /* "FORK" */
#define FORK_VERSION      1
#define FORK_MAX_HANDLES  STARTUP_MAX_HANDLES
#define FORK_BAD_MESSAGE  3             /* the exit code of a child given a bad one */

struct fork_msg {
    uint32_t txid;                      /* channel convention: first 4 bytes; 0 here */
    uint32_t magic;                     /* FORK_MAGIC */
    uint32_t version;                   /* FORK_VERSION */
    uint32_t nhandles;                  /* handles carried, in order */
    uint32_t roles[FORK_MAX_HANDLES];   /* the startup role of handle i: each at most once */
};
