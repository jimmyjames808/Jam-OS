/* jamcover: what jamjar and its cover helper (bin/jamcover,
 * user/apps/jamcover) agree on, next to the protocol (abi/idl/jamcover.idl).
 *
 * jamjar starts the helper in a job of its own with three handles and
 * nothing else (no namespace, no console, no services):
 *   SR_USER + JAMCOVER_ROLE_CH   the jamcover channel it serves
 *   SR_USER + JAMCOVER_ROLE_IN   the picture's bytes, JAMCOVER_IN_BYTES:
 *                                RIGHT_READ | RIGHT_MAP only
 *   SR_USER + JAMCOVER_ROLE_OUT  the pixels, JAMCOVER_OUT_BYTES: the two
 *                                sizes of the cover, at JAMCOVER_SMALL_AT
 *                                and JAMCOVER_LARGE_AT
 * jamjar reads the pixels back with vmo_read and never maps that VMO, so
 * nothing the helper writes there is ever more than bytes to it. A decode
 * that doesn't answer in JAMCOVER_TIMEOUT kills the helper. */
#pragma once

#include <stdint.h>

#define JAMCOVER_PATH     "bin/jamcover"
#define JAMCOVER_ROLE_CH  0u
#define JAMCOVER_ROLE_IN  1u
#define JAMCOVER_ROLE_OUT 2u

#define JAMCOVER_SMALL     256u                /* every album's cover is kept this big ... */
#define JAMCOVER_LARGE     512u                /* ... and the few drawn bigger at this too */
#define JAMCOVER_MAX_SIDE  2048u               /* a picture wider or taller is refused */
#define JAMCOVER_MAX_PIXELS (2048u * 1600u)
#define JAMCOVER_IN_BYTES  (6ull << 20)        /* the biggest tag jamjar reads */
#define JAMCOVER_SMALL_AT  0ull
#define JAMCOVER_LARGE_AT  ((uint64_t)JAMCOVER_SMALL * JAMCOVER_SMALL * 4)
#define JAMCOVER_OUT_BYTES (JAMCOVER_LARGE_AT + (uint64_t)JAMCOVER_LARGE * JAMCOVER_LARGE * 4)
#define JAMCOVER_TIMEOUT   (5ull * 1000 * 1000 * 1000)   /* ns: one decode at most */
