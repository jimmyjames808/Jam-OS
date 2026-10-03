/* fat: the state VMO (fat.h has what struct fat_state holds and why).
 *
 * The VMO is laid out by svcstate (<svcstate.h>): its header page, the two
 * request slots, then the service's own area, which is struct fat_state.
 * It is mapped at SVCSTATE_ADDR, so kept is the same address in every
 * instance and the pointers FatFs keeps inside the state stay valid.
 *
 * A slot's request area holds the biggest request fat serves (fs, file,
 * fsctl) in its first page, then FAT_FILE_BUF bytes of data: the request's
 * bounce buffer (request.c), so a write's bytes are in the state before it
 * runs. Its reply area holds the biggest reply. svcstate commits and maps
 * the header and both slots at the start (a request is read straight into
 * a slot, which must never fault): two slots of 68 KiB plus a page each.
 *
 * Today fat makes the VMO itself, at every start, before anything else:
 * a new VMO is zeros, as fat's tables were when they were its own static
 * memory, so fat starts exactly as it did. Its pages are committed as they
 * are touched; the hold's data, most of the state, is committed in one go
 * at the first hold (hold.c), so that running out of memory then is a
 * refusal (writes go through unheld, as when the hold's malloc failed)
 * and never a fault.
 *
 * Its handle and mapping are this instance's own: fat never closes or
 * unmaps them; they go with the process. */
#include <idl/fsctl.h>
#include <svcstate.h>
#include "fat.h"

#define REQ_CAP_FS (FS_REQ_MAX > FSCTL_REQ_MAX ? FS_REQ_MAX : FSCTL_REQ_MAX)
#define REP_CAP_FS (FS_REP_MAX > FSCTL_REP_MAX ? FS_REP_MAX : FSCTL_REP_MAX)
/* The biggest request fat serves (fs, file, fsctl), and the biggest reply. */
#define FAT_REQ_MSG (REQ_CAP_FS > FILE_REQ_MAX ? REQ_CAP_FS : FILE_REQ_MAX)
#define FAT_REP_CAP (REP_CAP_FS > FILE_REP_MAX ? REP_CAP_FS : FILE_REP_MAX)
/* A request slot: the message, then its data (the bounce buffer). */
#define FAT_REQ_CAP (FAT_SLOT_DATA + FAT_FILE_BUF)

_Static_assert(FAT_REQ_MSG <= FAT_SLOT_DATA, "a request fits before its data");
_Static_assert(FAT_SLOT_DATA % PAGE_SIZE == 0, "a slot's data starts on a page");
_Static_assert(FAT_REQ_CAP <= SVCSTATE_REQ_MAX, "a slot svcstate takes");
_Static_assert(offsetof(struct fat_state, hold.data) % PAGE_SIZE == 0,
               "the hold's data starts on a page (svcstate's own area does)");

struct fat_state *kept;

static handle_t state_vmo;     /* ours: the VMO under kept */
static struct svcstate svc;    /* its mapping */

status_t state_open(void)
{
    const struct svcstate_layout layout = {
        .kind = FAT_STATE_KIND, .layout = FAT_STATE_LAYOUT,
        .req_cap = FAT_REQ_CAP, .rep_cap = FAT_REP_CAP,
        .user_size = sizeof(struct fat_state),
    };
    enum svcstate_start how;
    status_t st = svcstate_create(svcstate_size(&layout), &state_vmo);
    if (st == OK)
        st = svcstate_open(state_vmo, &layout, &svc, &how);
    if (st != OK)
        return st;
    kept = svcstate_user(&svc);   /* a new VMO: zeros, fresh */
    return OK;
}

status_t state_commit(const void *p, size_t len)
{
    uint64_t base = (uint64_t)(uintptr_t)svc.h;
    uint64_t at = (uint64_t)(uintptr_t)p;
    uint64_t first = (at - base) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (at - base + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return jam_vmo_commit(state_vmo, first, end - first);
}

struct svcstate *state_slots(void)
{
    return &svc;
}
