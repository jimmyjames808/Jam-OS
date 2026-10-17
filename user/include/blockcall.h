/* <blockcall.h>: a filesystem service's calls on its partition's `block`
 * channel (abi/idl/block.idl), one at a time, each through the start of
 * the channel's shared buffer: extfs's disk.c (user/lib/blockcall.c).
 *
 * A call that times out is tried again. usb-storage gives a SCSI command
 * 10 s; a stick that takes longer (a big SYNCHRONIZE CACHE, or QEMU's
 * stick behind a busy host: docs/CR-PLAN.md's "The disk can fail a shard")
 * is reset and the call answered ERR_TIMED_OUT, the device ready for the
 * next command; our own wait running out leaves the request queued at the
 * driver. Either way the next try may well go through, where the journal
 * used to turn the volume read-only for the rest of the boot.
 *   - A flush is tried again on any channel: it moves no data, and a
 *     second one covers what the first did.
 *   - A read or a write only on a channel that answers in order (queue
 *     depth 1: usb-storage): the try moves the same blocks through the
 *     same buffer, untouched in between, so what landed of the first is
 *     written over with the same bytes, and the retry's answer means every
 *     earlier try is done: the buffer is free when blockcall() returns.
 *     drv/nvme answers in completion order, so there a retry's answer says
 *     nothing of the first command, which could still read the buffer
 *     after the next write has filled it: its timeouts fail the call as
 *     before (extfs writes to it with FUA and never flushes).
 * Nothing else is sent meanwhile (one call at a time), so the journal's
 * order holds: what must follow a flush waits for a try that went through.
 * Bounds: BLOCKCALL_TRIES tries within BLOCKCALL_BUDGET of the first one's
 * start, with pauses that double. usb-storage gives up on a device after
 * three commands in a row went unanswered (MAX_SILENT: its channels close,
 * the disk is gone, devmgr starts a fresh driver), and three of its
 * timeouts with their pauses take 31.5 s: the budget covers them. A client
 * waits 60 s for an answer (FS_CALL_TIMEOUT, the pager's page requests
 * too), so an episode leaves the rest of its request 20 s. The count
 * bounds a driver that answers ERR_TIMED_OUT at once (7.5 s of pauses).
 * Spent, the call fails as before, so a dead disk never holds writers
 * longer. One line per episode that went through; the caller logs a
 * failure (blockcall_tried says how many tries it had). */
#pragma once

#include <os.h>

#define BLOCKCALL_WAIT   (30 * NS_PER_S)    /* one try */
#define BLOCKCALL_TRIES  5                  /* tries of a call that times out, the first too */
#define BLOCKCALL_BUDGET (40 * NS_PER_S)    /* ... all of them, from the first one's start */
#define BLOCKCALL_PAUSE  (500 * NS_PER_MS)  /* before the second try, doubled before each after */

enum blockcall_op { BLOCKCALL_READ, BLOCKCALL_WRITE, BLOCKCALL_SYNC };

/* A service's channel, as its disk.c opened it. */
struct blockcall {
    handle_t    ch;           /* the `block` channel */
    const char *svc;          /* the log lines' "extfs" ... */
    const char *name;         /* ... and "/data" */
    bool        in_order;     /* block.info's queue_depth is 1: answers in order */
    bool        quick_pause;  /* a test's: pauses from 1 ms, not BLOCKCALL_PAUSE */
};

/* A call's tries, for its log line. */
struct blockcall_tries {
    unsigned n;
    uint64_t ns;   /* from the first one's start to the last one's end */
};

/* One call, `count` blocks at `lba` through the buffer's start (a flush:
 * neither), tried again while it times out, as above. */
status_t blockcall(const struct blockcall *b, enum blockcall_op op, uint64_t lba, uint32_t count,
                   struct blockcall_tries *t);

/* " (5 tries in 7512 ms)" for a call that was tried again, else "". */
const char *blockcall_tried(const struct blockcall_tries *t, char *buf, size_t n);

/* "read", "write" or "flush". */
const char *blockcall_name(enum blockcall_op op);
