/* extfs: its side of the keep channel (<keep.h>), as fat's keeper.c.
 * Every handle a client holds the other end of (an open file's channel
 * and buffer, a view's channel) is put with the keeper before the request
 * that made it commits, so a slot the state knows is always one the keeper
 * was sent; a drop is sent once the state has forgotten the slot. No
 * request forgets a slot it didn't make (a file or view goes when its
 * client closes it, between requests), so a drop never waits for a
 * commit as fat's do. Without SR_KEEP nothing is kept. */
#include <keep.h>
#include "extfs.h"

_Static_assert(XF_KEEP_SLOTS <= KEEP_MAX_SLOTS, "the keeper takes every slot");
_Static_assert(XF_MAX_FILES * 2 + XF_VIEWS <= KEEP_MAX_HANDLES, "the keeper holds them all");

status_t kept_put(uint32_t slot, const handle_t *hs, unsigned n)
{
    return vol.keep ? keep_put(vol.keep, slot, hs, n) : OK;
}

void kept_drop(uint32_t slot)
{
    if (!vol.keep)
        return;
    status_t st = keep_drop(vol.keep, slot);
    if (st != OK && st != ERR_PEER_CLOSED)
        printf("extfs %s: can't tell the keeper slot %u is closed (%s)\n", vol.name, slot,
               status_str(st));
}
