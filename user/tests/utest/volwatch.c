/* utest: volumes' watches (volumes.watch, the `mounts` events) and
 * volumes.release_disk, on the mock disk of diskmock.c (disks.h has what
 * these share with disks.c).
 *
 *   volumes_watch         a watch lists every mount, then `settled`; a
 *                         remount is `unmounted` and `mounted` again at a
 *                         new generation; a watcher that never reads is
 *                         dropped before MOUNTS_UNREAD_MAX events wait for
 *                         it, while one that reads goes on; a new watch
 *                         gets the whole list
 *   volumes_release_disk  the disk's mounts stop in order (the volume
 *                         left clean) and stay gone while the disk is
 *                         still there
 *   volumes_new_channel   a restart that failed after the mount's channel
 *                         went: the watch is told of the new one */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "disks.h"
#include "utest.h"

#define REMOUNTS_MAX 120   /* remounts tried before the silent watcher must be gone */

static bool peer_gone(handle_t h)
{
    signals_t seen = 0;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, 0, &seen) == OK;
}

bool t_volumes_watch(void)
{
    handle_t vm = volumes();
    struct view v = { 0 }, silent = { 0 }, fresh = { 0 };
    uint32_t id = 0;
    bool skip;
    char usbn[16];
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_OTHER, &id, &skip))
        return skip;
    if (!wait_usb(vm, &v, 1))
        return false;
    snprintf(usbn, sizeof(usbn), "%s", usb_path(&v));
    /* a second watch: the whole list, then `settled`, at v's generation */
    CHECK_ST(view_next(vm, &silent, now() + 10 * NS_PER_S), OK);
    CHECK_EQ(silent.generation, v.generation);
    CHECK_EQ(silent.n, v.n);
    CHECK_EQ(silent.events, silent.n + 1);
    CHECK(mounted(&silent, usbn));
    /* a remount: the mount comes again, at a new generation */
    uint32_t gen0 = mount_gen(&v, usbn), events0 = v.events;
    CHECK_ST(remount(vm, usbn, true), OK);
    if (!wait_usb(vm, &v, 0))
        return false;
    CHECK(mount_gen(&v, usbn) != gen0 && v.generation != gen0);
    CHECK(v.events >= events0 + 3);   /* unmounted, mounted, settled at least */
    /* silent never reads: dropped before MOUNTS_UNREAD_MAX wait for it */
    unsigned remounts = 1;
    while (!peer_gone(silent.watch) && remounts < REMOUNTS_MAX) {
        CHECK_ST(remount(vm, usbn, remounts % 2 == 0), OK);
        if (!wait_usb(vm, &v, remounts % 2 == 0 ? 0 : 1))
            return false;
        remounts++;
    }
    struct handle_info info;
    CHECK_ST(jam_handle_info(silent.watch, &info), OK);
    printf("utest: %s: the silent watch dropped after %u remounts, %u events unread\n", utest_cur,
           remounts, info.queued);
    CHECK(peer_gone(silent.watch));
    CHECK(info.queued <= MOUNTS_UNREAD_MAX && info.queued > MOUNTS_UNREAD_MAX / 2);
    view_close(&silent);
    /* the one that reads goes on, and a new one gets the whole list */
    CHECK(!peer_gone(v.watch));
    CHECK_ST(view_next(vm, &fresh, now() + 10 * NS_PER_S), OK);
    CHECK_EQ(fresh.generation, v.generation);
    CHECK(mounted(&fresh, usbn));
    view_close(&fresh);
    if (!detach(vm, &v))
        return false;
    return wait_usb(vm, &v, -1);
}

/* Is the mock's data partition's FAT marked clean (entry 1's clean bit:
 * bit 15 on FAT16, bit 27 on FAT32)? *known false on FAT12 (no bit). */
static bool data_clean(bool *known)
{
    const uint8_t *bs = mock.ram + (uint64_t)DM_DATA_START * DM_BLOCK;
    uint32_t reserved = bs[14] | (uint32_t)bs[15] << 8;
    const uint8_t *fat = bs + (uint64_t)reserved * DM_BLOCK;
    bool fat32 = !memcmp(bs + 82, "FAT32", 5), fat16 = !memcmp(bs + 54, "FAT16", 5);
    *known = fat32 || fat16;
    return fat32 ? (fat[7] & 0x08) != 0 : fat16 ? (fat[3] & 0x80) != 0 : false;
}

/* A mount's restart that fails after its `fs` channel went: killed twice
 * in a row, the second restart finds no warm spare ready yet (the first
 * took it), so it starts a process, whose `block` channel the disk
 * refuses: the start fails and the channel goes with it. The restart
 * after that serves a new channel, and the watch must say so (`unmounted`,
 * `mounted`), or every namespace keeps the dead one. Either way the
 * channel the watch last gave answers. */
bool t_volumes_new_channel(void)
{
    handle_t vm = volumes();
    struct view v = { 0 };
    uint32_t id = 0, state = 0, restarts = 0, backoff = 0, promoted = 0;
    uint8_t spare = 0;
    uint64_t koid = 0;
    bool skip;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    struct fatrun data = mount_of(&v, DATA);
    if (!put_file(&data, "/a.txt", "before the kills"))
        return false;
    CHECK_ST(t_sync(&data), OK);
    uint32_t gen0 = mount_gen(&v, DATA);
    CHECK_ST(volumes_kill_until(vm, now() + 30 * NS_PER_S, DATA, sizeof(DATA) - 1, &koid), OK);
    /* the kill's restart runs right after its answer, before volumes reads
     * the next request: this one sees it done (and the spare used) */
    CHECK_ST(volumes_supervision_until(vm, now() + 10 * NS_PER_S, DATA, sizeof(DATA) - 1, &state,
                                       &restarts, &backoff, &promoted, &spare), OK);
    CHECK_EQ(state, VOLUMES_SUP_RUNNING);
    __atomic_store_n(&mock.refuse_opens, 2, __ATOMIC_RELAXED);
    CHECK_ST(volumes_kill_until(vm, now() + 30 * NS_PER_S, DATA, sizeof(DATA) - 1, &koid), OK);
    uint64_t until = now() + 15 * NS_PER_S;
    do {
        CHECK(now() < until);
        jam_sleep_until(now() + 20 * NS_PER_MS);
        CHECK_ST(volumes_supervision_until(vm, now() + 10 * NS_PER_S, DATA, sizeof(DATA) - 1,
                                           &state, &restarts, &backoff, &promoted, &spare), OK);
    } while (state != VOLUMES_SUP_RUNNING || dm_count(&mock.refuse_opens));
    /* whatever the watch had to say by now, then the channel it gave */
    while (view_next(vm, &v, now() + 500 * NS_PER_MS) == OK)
        ;
    bool moved = mount_gen(&v, DATA) != gen0;
    printf("utest: %s: %u restarts, %u from the spare; the mount's channel %s\n", utest_cur,
           restarts, promoted, moved ? "is a new one" : "stayed");
    data = mount_of(&v, DATA);
    CHECK(data.fs != HANDLE_INVALID);
    CHECK(file_is(&data, "/a.txt", "before the kills"));
    return detach(vm, &v);
}

bool t_volumes_release_disk(void)
{
    handle_t vm = volumes();
    struct view v = { 0 };
    uint32_t id = 0;
    bool skip, known = false;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    struct fatrun data = mount_of(&v, DATA);
    if (!put_file(&data, "/released.txt", "written before the release"))
        return false;
    uint32_t closed0 = dm_count(&mock.closed[0]), closed1 = dm_count(&mock.closed[1]);
    CHECK_ST(volumes_release_disk_until(vm, now() + 30 * NS_PER_S, id ^ 1u), ERR_NOT_FOUND);
    CHECK_ST(volumes_release_disk_until(vm, now() + 30 * NS_PER_S, id), OK);
    /* both services stopped: their `block` channels closed, the volume clean */
    CHECK_EQ(dm_count(&mock.closed[0]), closed0 + 1);
    CHECK_EQ(dm_count(&mock.closed[1]), closed1 + 1);
    bool clean = data_clean(&known);
    if (known)
        CHECK(clean);
    else
        printf("utest: %s: a FAT12 volume: no clean bit to look at\n", utest_cur);
    if (!wait_mounts(vm, &v, false, false))
        return false;
    /* and gone they stay: nothing reopens the disk while it is there */
    uint32_t opened = dm_count(&mock.opened[0]) + dm_count(&mock.opened[1]);
    if (!view_quiet(&v))
        return false;
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA));
    CHECK_EQ(dm_count(&mock.opened[0]) + dm_count(&mock.opened[1]), opened);
    return detach(vm, &v);
}
