/* fat: its side of the keep channel (<keep.h>; docs/M11.6-PLAN.md, "The
 * keep channel"). Every handle a client holds the other end of (an open
 * file's channel and buffer, a view's channel) is put with devmgr, the
 * keeper, so that it outlives fat and a successor is handed it back
 * (adopt.c). A slot of files[] is keep slot FAT_KEEP_FILE(i), a view's
 * FAT_KEEP_VIEW(i), a mapped file's shared VMO (fat's own handle of it,
 * pager.c) FAT_KEEP_MAP(i).
 *
 * Order makes it exact without a reply. A put is sent before the request
 * that made the slot commits (the open or view is recorded in the state
 * only by that commit), so a slot the state knows is always one the
 * keeper was sent. A drop is sent only once the state has forgotten the
 * slot for good: a slot closed inside an operation (the close of a file
 * whose client has gone, or a failed open) is dropped after that
 * operation's commit (kept_flush, from request.c's send), since an undo
 * would bring it back. A death in between leaves the keeper holding a
 * slot the state doesn't know, which the successor refuses (and so
 * drops).
 *
 * Without SR_KEEP (a test's fat over a RAM disk) nothing is kept: every
 * call here does nothing. */
#include <keep.h>
#include "fat.h"

_Static_assert(FAT_KEEP_SLOTS <= 128, "every slot's drop fits two words");
_Static_assert(FAT_KEEP_SLOTS <= KEEP_MAX_SLOTS, "the keeper takes every slot");
_Static_assert(FAT_MAX_FILES * 2 + FAT_VIEWS + FAT_MAX_MAPS <= KEEP_MAX_HANDLES,
               "the keeper holds them all");

/* Keep slot k is to be dropped after the commit: bit k % 64 of drops[k / 64]. */
static uint64_t drops[2];

status_t kept_put(uint32_t slot, const handle_t *hs, unsigned n)
{
    if (!vol.keep)
        return OK;
    drops[slot / 64] &= ~(1ull << slot % 64);   /* a drop owed would take the new one with it */
    return keep_put(vol.keep, slot, hs, n);
}

void kept_drop(uint32_t slot)
{
    if (!vol.keep)
        return;
    if (op_running()) {
        drops[slot / 64] |= 1ull << slot % 64;
        return;
    }
    status_t st = keep_drop(vol.keep, slot);
    if (st != OK && st != ERR_PEER_CLOSED)
        printf("fat %s: can't tell the keeper slot %u is closed (%s)\n", vol.name, slot,
               status_str(st));
}

void kept_flush(void)
{
    for (uint32_t w = 0; w < 2; w++) {
        while (drops[w]) {
            uint32_t slot = w * 64 + (uint32_t)__builtin_ctzll(drops[w]);
            drops[w] &= drops[w] - 1;
            kept_drop(slot);
        }
    }
}

void kept_cancel(void)
{
    drops[0] = drops[1] = 0;
}
