/* utest: fat's control channel (fsctl, abi/idl/fsctl.idl) answers every
 * request queued on it, not one per wake-up. Its port binding is
 * PERSISTENT, which fires when a request comes to an empty queue: requests
 * queued behind the first must be served in the same turn, or they wait
 * for a request that may never come. Three are queued before fat has even
 * mounted (so before it first looks), each answered by its txid. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/fsctl.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define QUEUED    3u
#define CTL_WAIT  (5 * NS_PER_S)   /* each answer, at most */

static struct ramdisk disk;

static bool all_answered(const struct fatrun *r)
{
    for (uint32_t t = 1; t <= QUEUED; t++)
        CHECK_ST(fsctl_stats_send(r->ctl, t), OK);
    for (uint32_t t = 1; t <= QUEUED; t++) {
        signals_t seen = 0;
        status_t st = jam_object_wait_one(r->ctl, SIG_READABLE, now() + CTL_WAIT, &seen);
        if (st != OK)
            FAIL("fsctl.stats %u of %u queued together: no answer (%s)", t, QUEUED,
                 status_str(st));
        _Alignas(8) uint8_t rep[FSCTL_REP_MAX];
        struct idl_msg m;
        CHECK_ST(idl_reply_read(r->ctl, rep, sizeof(rep), &m), OK);
        CHECK_EQ(m.txid, t);
        CHECK_ST(fsctl_stats_result(rep, &m, NULL, NULL, NULL, NULL, NULL), OK);
    }
    return true;
}

bool t_fat_ctl_queued(void)
{
    struct fatrun r;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    bool ok = all_answered(&r);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
