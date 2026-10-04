/* devmgr: DEVMGR_MOUNTS (<devmgr.h>): the list of mounts, its generation,
 * and the calls waiting for it to change.
 *
 * The mounts themselves are disk.c's (the boot disk's running filesystem
 * services). This file keeps the list as it was last handed out: whenever
 * something may have changed it (a service started or ended, a disk came
 * or went) mounts_update compares, and only a real difference moves the
 * generation on. A mount with a new `fs` channel (a remount, a service
 * started again after it was given up on) differs by its channel's
 * generation, so it is a new generation even though the path is the same;
 * a filesystem service restarted on the channel devmgr kept is not (fat
 * keeps its views and files across its deaths: namespaces need nothing
 * new). Generations count up from a number taken from the clock
 * (first_generation says why).
 *
 * A call whose caller already has the current generation is kept in
 * waiters[] (its channel and transaction id) and answered by the next
 * change, or with ERR_TIMED_OUT after DEVMGR_MOUNTS_WAIT: devmgr never
 * holds a call longer than that, so a caller that gave up leaves at most
 * one small reply behind, and the table can't fill with dead calls. */
#include "internal.h"

#define MAX_WAITERS 8

struct waiter {
    handle_t ch;         /* the channel the call came on; 0: a free slot */
    uint32_t txid;       /* its transaction id */
    uint64_t deadline;   /* answered ERR_TIMED_OUT then */
};

static struct mount  cur[DEVMGR_MAX_MOUNTS];   /* the list of generation `generation` */
static unsigned      ncur;
static uint32_t      generation;               /* 0 only until first_generation() */
static struct waiter waiters[MAX_WAITERS];

/* The first generation comes from the clock (uptime in ms, made odd: never
 * 0, which in a request matches nothing). A devmgr that init started again
 * after the first one died would otherwise count 1, 2, ... like the first,
 * and a caller still holding the old one's generation 2 would wait for a
 * change instead of getting the new one's channels. */
static void first_generation(void)
{
    if (!generation)
        generation = (uint32_t)(now() / NS_PER_MS) | 1;
}

/* Answer request txid on ch: the list with a channel per mount when st is
 * OK, else the bare status. */
static void answer(handle_t ch, uint32_t txid, status_t st)
{
    struct devmgr_mounts_rep r;
    handle_t hs[DEVMGR_MAX_MOUNTS];
    uint32_t nh = 0;
    memset(&r, 0, sizeof(r));
    r.txid = txid;
    r.generation = generation;
    while (st == OK && nh < ncur) {
        st = jam_handle_duplicate(devs[cur[nh].bind].client, RIGHT_SAME, &hs[nh]);
        if (st == OK) {
            memcpy(r.mounts[nh].path, cur[nh].path, sizeof(r.mounts[nh].path));
            nh++;
        }
    }
    if (st != OK)   /* all or nothing */
        while (nh > 0)
            jam_handle_close(hs[--nh]);
    r.status = st;
    r.count = nh;
    uint32_t n = st == OK ? sizeof(r) : DEVMGR_REP_HDR;
    if (jam_channel_write(ch, &r, n, hs, nh) != OK)
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);   /* the caller is gone */
}

void mounts_update(void)
{
    struct mount fresh[DEVMGR_MAX_MOUNTS] = { 0 };
    unsigned n = disk_mounts(fresh);
    bool same = n == ncur;
    first_generation();
    for (unsigned i = 0; same && i < n; i++)
        same = fresh[i].bind == cur[i].bind && fresh[i].gen == cur[i].gen;
    if (same)
        return;
    memcpy(cur, fresh, sizeof(cur));
    ncur = n;
    if (++generation == 0)
        generation = 1;
    char list[DEVMGR_MAX_MOUNTS * 17 + 8] = " (none)";
    for (unsigned i = 0, at = 0; i < n; i++)
        at += (unsigned)snprintf(list + at, sizeof(list) - at, " %s", cur[i].path);
    say(false, "devmgr: mounts, generation %u:%s", generation, list);
    for (unsigned i = 0; i < MAX_WAITERS; i++) {
        if (!waiters[i].ch)
            continue;
        answer(waiters[i].ch, waiters[i].txid, OK);
        waiters[i].ch = HANDLE_INVALID;
    }
}

void mounts_request(handle_t ch, uint32_t txid, uint32_t known)
{
    first_generation();
    if (known != generation) {
        answer(ch, txid, OK);
        return;
    }
    for (unsigned i = 0; i < MAX_WAITERS; i++) {
        if (waiters[i].ch)
            continue;
        waiters[i] = (struct waiter){ ch, txid, now() + DEVMGR_MOUNTS_WAIT };
        return;
    }
    answer(ch, txid, ERR_NO_RESOURCES);
}

void mounts_run_due(void)
{
    uint64_t t = now();
    for (unsigned i = 0; i < MAX_WAITERS; i++) {
        if (!waiters[i].ch || waiters[i].deadline > t)
            continue;
        answer(waiters[i].ch, waiters[i].txid, ERR_TIMED_OUT);
        waiters[i].ch = HANDLE_INVALID;
    }
}

uint64_t mounts_next_deadline(void)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < MAX_WAITERS; i++)
        if (waiters[i].ch && waiters[i].deadline < next)
            next = waiters[i].deadline;
    return next;
}
