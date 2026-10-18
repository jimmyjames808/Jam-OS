/* extfs: the state VMO (extfs.h has what struct xf_state holds and why).
 *
 * Laid out by svcstate (<svcstate.h>): its header page, the two request
 * slots, then the service's own area, struct xf_state. A slot's request
 * area holds the biggest request extfs serves (fs, file, fsctl) in its
 * first XF_SLOT_DATA bytes, then XF_FILE_BUF bytes of data, the request's
 * bounce buffer (request.c); its reply area the biggest reply. svcstate
 * commits and maps the header and both slots at the start.
 *
 * The VMO is the spawner's (SR_STATE), or, without one (a test's RAM
 * disk), one extfs makes itself. It is bound to the instance's name and
 * the partition's size, as fat's is; adopt.c also checks the volume's
 * UUID against the one the state was made for. */
#include "extfs.h"

#define REQ_CAP_FS (FS_REQ_MAX > FSCTL_REQ_MAX ? FS_REQ_MAX : FSCTL_REQ_MAX)
#define REP_CAP_FS (FS_REP_MAX > FSCTL_REP_MAX ? FS_REP_MAX : FSCTL_REP_MAX)
#define XF_REQ_MSG (REQ_CAP_FS > FILE_REQ_MAX ? REQ_CAP_FS : FILE_REQ_MAX)
#define XF_REP_CAP (REP_CAP_FS > FILE_REP_MAX ? REP_CAP_FS : FILE_REP_MAX)
#define XF_REQ_CAP (XF_SLOT_DATA + XF_FILE_BUF)

_Static_assert(XF_REQ_MSG <= XF_SLOT_DATA, "a request fits before its data");
_Static_assert(XF_SLOT_DATA % PAGE_SIZE == 0, "a slot's data starts on a page");
_Static_assert(XF_REQ_CAP <= SVCSTATE_REQ_MAX, "a slot svcstate takes");

struct xf_state *kept;

static handle_t state_vmo;
static struct svcstate svc;

static void binding(uint8_t out[SVCSTATE_BINDING])
{
    memset(out, 0, SVCSTATE_BINDING);
    size_t n = strnlen(vol.name, SVCSTATE_BINDING - 9);
    memcpy(out, vol.name, n);
    memcpy(out + SVCSTATE_BINDING - 8, &vol.sectors, 8);
}

status_t state_open(bool *adopted)
{
    struct svcstate_layout layout = {
        .kind = XF_STATE_KIND, .layout = XF_STATE_LAYOUT,
        .req_cap = XF_REQ_CAP, .rep_cap = XF_REP_CAP,
        .user_size = sizeof(struct xf_state),
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
    /* All of it now: it is small, and a fault in it later would be a
     * death in the middle of a request. */
    if (st == OK)
        st = jam_vmo_commit(state_vmo, 0, svcstate_size(&layout));
    *adopted = how == SVCSTATE_ADOPTED;
    if (st == OK && !*adopted)
        state_reset(false);
    else if (st == OK)
        watch_adopt_table();   /* before the mount, whose orphans may be watched */
    return st;
}

_Static_assert(offsetof(struct xf_state, watch) + sizeof(struct xf_watches) ==
               sizeof(struct xf_state), "watch.c's part is the state's last");

void state_reset(bool keep_views)
{
    size_t views = offsetof(struct xf_state, views), after = views + sizeof(kept->views);
    size_t watch = offsetof(struct xf_state, watch);
    uint8_t *p = (uint8_t *)kept;
    memset(p, 0, views);
    memset(p + after, 0, watch - after);
    if (!keep_views)
        memset(kept->views, 0, sizeof(kept->views));
    locks_reset();   /* the lock owners' tokens and the waits this instance holds */
    watch_reset();   /* its own part: the channels it still holds ended first */
}

struct svcstate *state_slots(void)
{
    return &svc;
}
