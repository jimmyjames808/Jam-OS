/* <blockcall.h>: a filesystem service's calls on its partition's `block`
 * channel (abi/idl/block.idl), one at a time, each through the start of
 * the channel's shared buffer: extfs's and fat's disk.c
 * (user/lib/blockcall.c).
 *
 * Whose the buffer is. The driver moves a call's data through the buffer
 * until it answers: drv/nvme's device reads and writes it straight (DMA),
 * usb-storage copies to and from it. An answer, whatever it says, means
 * the driver is done with it (drv/nvme answers once the command completed,
 * or the controller is reset or its bus mastering off: drivers/nvme/
 * block.c). A call whose own wait runs out first leaves the request with
 * the driver, which may still read the buffer (a write) or fill it (a
 * read) later: after the next call has put its blocks there, that is the
 * wrong data on the disk, or in the cache.
 *
 * So each wait is longer than the driver holds a request it can't get
 * done, and a call whose wait still runs out ends the service:
 *   - a channel that answers in order (queue depth 1: usb-storage) gets
 *     BLOCKCALL_WAIT a try: its longest commands, a WRITE(10) and a
 *     SYNCHRONIZE CACHE, are given up on after 25 s (drivers/usb-storage/
 *     scsi.c has the sums), the device reset, the call answered
 *     ERR_TIMED_OUT;
 *   - one that doesn't (drv/nvme) gets BLOCKCALL_WAIT_DEEP: a command gets
 *     30 s, an Abort 5 s more, then the controller is reset; a write is
 *     answered then, a read sent once more and given as long again: 70 s
 *     and two resets.
 *   - A read or a write whose last try's own wait ran out never returns:
 *     the process ends (exit 1, "a restart may help"), so nothing of it
 *     touches the buffer again. volumes starts the next instance on a new
 *     channel with a buffer of its own, and the driver keeps the old
 *     request from landing after the new one's (usb-storage's fence,
 *     drv/nvme's drain rule: block.idl). A flush moves no data: it just
 *     fails.
 *
 * A call that times out is tried again. usb-storage gives a SCSI command's
 * phase 5 to 20 s; a stick that takes longer (a big SYNCHRONIZE CACHE, or
 * QEMU's stick behind a busy host: docs/CR-PLAN.md's "The disk can fail a
 * shard") is reset and the call answered ERR_TIMED_OUT, the device ready
 * for the next command; our own wait running out leaves the request
 * queued at the driver. Either way the next try may well go through, where
 * the journal used to turn the volume read-only for the rest of the boot.
 *   - A flush is tried again on any channel: it moves no data, and a
 *     second one covers what the first did.
 *   - A read or a write only on a channel that answers in order: the try
 *     moves the same blocks through the same buffer, untouched in between,
 *     so what landed of the first is written over with the same bytes, and
 *     the retry's answer means every earlier try is done: the buffer is
 *     free when blockcall() returns. drv/nvme answers in completion order,
 *     so there a retry's answer says nothing of the first command: a call
 *     it answered ERR_TIMED_OUT (an Abort ended it) fails as before (extfs
 *     writes to it with FUA and never flushes).
 * Nothing else is sent meanwhile (one call at a time), so the journal's
 * order holds: what must follow a flush waits for a try that went through.
 * Bounds: BLOCKCALL_TRIES tries within BLOCKCALL_BUDGET of the first one's
 * start, with pauses that double. A flush the stick doesn't finish in
 * usb-storage's 20 s gets two whole tries (20 + 0.5 + 20 s) and what is
 * left for a third; a write whose data the stick doesn't take in 10 s,
 * three (31.5 s). usb-storage gives up on a device after three commands
 * in a row went unanswered (MAX_SILENT: its channels close, the disk is
 * gone, devmgr starts a fresh driver). A client waits 60 s for an answer
 * (FS_CALL_TIMEOUT, the pager's page requests too), so an episode leaves
 * the rest of its request 15 s. The count bounds a driver that answers
 * ERR_TIMED_OUT at once (7.5 s of pauses).
 * Spent, the call fails as before, so a dead disk never holds writers
 * longer. One line per episode that went through; the caller logs a
 * failure (blockcall_tried says how many tries it had). */
#pragma once

#include <os.h>

#define BLOCKCALL_WAIT      (30 * NS_PER_S)    /* one try, on a channel that answers in order */
#define BLOCKCALL_WAIT_DEEP (90 * NS_PER_S)    /* one call, on one that doesn't */
#define BLOCKCALL_TRIES     5                  /* tries of a call that times out, the first too */
#define BLOCKCALL_BUDGET    (45 * NS_PER_S)    /* ... all of them, from the first one's start */
#define BLOCKCALL_PAUSE     (500 * NS_PER_MS)  /* before the second try, doubled before each after */
#define BLOCKCALL_QUICK     (3 * NS_PER_S)     /* a test's every wait, and half its budget */

enum blockcall_op { BLOCKCALL_READ, BLOCKCALL_WRITE, BLOCKCALL_SYNC };

/* A service's channel, as its disk.c opened it. */
struct blockcall {
    handle_t    ch;           /* the `block` channel */
    const char *svc;          /* the log lines' "extfs" ... */
    const char *name;         /* ... and "/data" */
    bool        in_order;     /* block.info's queue_depth is 1: answers in order */
    bool        quick_pause;  /* a test's: pauses from 1 ms, not BLOCKCALL_PAUSE */
    bool        quick_wait;   /* a test's: waits of BLOCKCALL_QUICK, a budget of two */
};

/* A call's tries, for its log line. */
struct blockcall_tries {
    unsigned n;
    uint64_t ns;   /* from the first one's start to the last one's end */
};

/* One call, `count` blocks at `lba` through the buffer's start (a flush:
 * neither), tried again while it times out, as above. A read or a write
 * that got no answer at all doesn't return: the process ends (exit 1). */
status_t blockcall(const struct blockcall *b, enum blockcall_op op, uint64_t lba, uint32_t count,
                   struct blockcall_tries *t);

/* " (5 tries in 7512 ms)" for a call that was tried again, else "". */
const char *blockcall_tried(const struct blockcall_tries *t, char *buf, size_t n);

/* "read", "write" or "flush". */
const char *blockcall_name(enum blockcall_op op);
