/* The POSIX layer's fork messages: what a forked child gets for its
 * descriptors and its table of mappings (docs/FORK-PLAN.md section 1.6,
 * track FL2). They follow libjam's fork message (<forkmsg.h>) on the
 * child's startup channel, all written by the parent before the child
 * starts (the layer's give, with its locks held from before the clone), so
 * the child reads them without waiting; the last is marked POSIXFORK_LAST
 * (an empty one if there is nothing to pass).
 *
 * The child holds a copy of its parent's memory, so it has every
 * description and every entry of the table already: what it lacks are
 * handles of its own. The messages carry only those, each under a key
 * saying what it is for. Both sides walk the same tables in the same order
 * (the parent's were whole at the clone, and the child's are the copy), so
 * a key needs no more than a part and a place in it:
 *
 *   POSIXFORK_FD     the description first met at descriptor `item` (its
 *                    lowest), or POSIXFORK_CTTY for the process's
 *                    terminal held by no descriptor, or POSIXFORK_CWD
 *                    for the current directory held by neither (its
 *                    channel, for a directory channel); `sub` is the kind's
 *                    own numbering of its handles (a terminal's tty
 *                    channel 0 and data channel 1; a connection's inner
 *                    socketpair end from 8 on)
 *   POSIXFORK_COPY   the copy-on-write child of the `item`-th VMO of the
 *                    parent's table mapped as a copy (in the order of the
 *                    handles' values: vmar_clone's children)
 *   POSIXFORK_SHARE  a duplicate of the `item`-th VMO mapped shared
 *
 * Keys grow along the messages. A key the child doesn't ask for (a
 * description that changed in the parent between the clone and the
 * walk) is closed unused; one it asks for that didn't come (a handle
 * without RIGHT_DUPLICATE or RIGHT_TRANSFER, which is never passed) leaves
 * that description out of the child, or that entry without its handle.
 *
 * The same program writes it and reads it, but the child checks it as it
 * checks libjam's: a malformed message ends the child with
 * FORK_BAD_MESSAGE. It is a hand-written format, not IDL: its id in
 * abi/idl/ids.txt is the top half of POSIXFORK_MAGIC. */
#pragma once

#include <stdint.h>
#include <jam/abi.h>

#define POSIXFORK_MAGIC   0x4b46504au   /* "JPFK" */
#define POSIXFORK_VERSION 1
#define POSIXFORK_LAST    1u            /* posixfork_msg.flags: the last message */
#define POSIXFORK_MAX     CHANNEL_MAX_HANDLES   /* handles in one message */

/* A key: the part in the top 4 bits, then the item and (for FD) the sub. */
#define POSIXFORK_FD      (1u << 28)
#define POSIXFORK_COPY    (2u << 28)
#define POSIXFORK_SHARE   (3u << 28)
#define POSIXFORK_PART    (15u << 28)
#define POSIXFORK_SUBS    16u           /* a description's handles: subs 0 .. 15 */
#define POSIXFORK_CTTY    1024u         /* the terminal's item (descriptors are 0 .. 1023) */
#define POSIXFORK_CWD     1025u         /* the current directory's (after the terminal's) */
#define POSIXFORK_KEY(part, item, sub) ((part) | (uint32_t)(item) * POSIXFORK_SUBS | (sub))

struct posixfork_msg {
    uint32_t txid;                  /* channel convention: first 4 bytes; 0 here */
    uint32_t magic;                 /* POSIXFORK_MAGIC */
    uint32_t version;               /* POSIXFORK_VERSION */
    uint32_t flags;                 /* POSIXFORK_LAST on the last message, else 0 */
    uint32_t nhandles;              /* handles carried, 0 .. POSIXFORK_MAX */
    uint32_t keys[POSIXFORK_MAX];   /* handle i's key: each greater than the one before,
                                     * here and in every earlier message */
};
