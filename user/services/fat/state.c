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
 * The VMO is devmgr's (SR_STATE), made once per mount and handed to each
 * instance, or, without SR_STATE (a test's RAM disk), one fat makes itself:
 * a new VMO is zeros. The state is bound to the instance's name and the
 * partition's size (the binding svcstate checks), so a state can't be
 * taken for another mount's. A state that isn't adopted (new, refused by
 * svcstate, or given up on by adopt.c) is set up empty (state_reset)
 * before anything else uses it: a refused one holds whatever it held.
 * Its pages are committed as they are touched, but for two areas committed
 * ahead, so that running out of memory is a refusal and never a fault: the
 * undo copy's (at the start: every operation writes it) and the hold's
 * data (hold.c, a chunk at a time as it grows).
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

/* What the state belongs to: the instance's name ("/data") and the
 * partition's size in sectors. */
static void binding(uint8_t out[SVCSTATE_BINDING])
{
    memset(out, 0, SVCSTATE_BINDING);
    size_t n = strnlen(vol.name, SVCSTATE_BINDING - 9);
    memcpy(out, vol.name, n);
    memcpy(out + SVCSTATE_BINDING - 8, &vol.blocks, 8);
}

status_t state_open(bool *adopted)
{
    struct svcstate_layout layout = {
        .kind = FAT_STATE_KIND, .layout = FAT_STATE_LAYOUT,
        .req_cap = FAT_REQ_CAP, .rep_cap = FAT_REP_CAP,
        .user_size = sizeof(struct fat_state),
    };
    binding(layout.binding);
    enum svcstate_start how = SVCSTATE_FRESH;
    state_vmo = startup_handle(SR_STATE);
    status_t st = state_vmo ? OK : svcstate_create(svcstate_size(&layout), &state_vmo);
    if (st == OK)
        st = svcstate_open(state_vmo, &layout, &svc, &how);
    if (st != OK)
        return st;
    kept = svcstate_user(&svc);
    *adopted = how == SVCSTATE_ADOPTED;
    st = state_commit(&kept->undo, sizeof(kept->undo));
    if (st == OK && !*adopted)
        state_reset(false);
    return st;
}

void state_reset(bool keep_views)
{
    static struct fat_view views[FAT_VIEWS];
    if (keep_views)
        memcpy(views, kept->views, sizeof(views));
    /* Everything but the hold's data, which nothing reads past `held`. */
    memset(kept, 0, offsetof(struct fat_state, hold.data));
    if (keep_views)
        memcpy(kept->views, views, sizeof(views));
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
