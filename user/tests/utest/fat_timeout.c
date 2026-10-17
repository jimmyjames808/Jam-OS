/* utest: fat over a RAM disk whose block calls time out (<blockcall.h>,
 * shared with extfs): tried again while they do (fat_timeout), failed as
 * before past their tries (fat_timeout_spent), and a read the disk holds
 * past fat's wait, which ends fat (fat_late). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define DISK_SECTORS (16 * MIB_SECTORS)

static struct ramdisk disk;
static const char *const quick[] = { "quick-retry", "quick-wait" };

/* fat ended by itself with `code`, the session's channels closed. */
static bool fat_ended(struct fatrun *r, int code)
{
    CHECK(fat_wait(r, code));
    CHECK_ST(jam_handle_close(r->fs), OK);
    return ramdisk_join(r->rd);
}

/* usb-storage answers ERR_TIMED_OUT when the stick's command ran out of
 * its time, and fat tries the call again as extfs does: two flushes that
 * time out; a write that times out having landed half its sectors; four
 * of either in one request (its 5th try goes through). Then a fresh fat,
 * whose first reads time out too, finds it all and writes on. */
bool t_fat_timeout(void)
{
    struct fatrun r;
    CHECK(ramdisk_create(&disk, DISK_SECTORS));
    CHECK(fat_start_words(&r, &disk, quick, 1));
    CHECK_ST(t_mkdir(&r, "/t"), OK);
    CHECK_ST(t_sync(&r), OK);
    ramdisk_time_out(&disk, RAMDISK_SYNC, 2);
    CHECK(put_file(&r, "/t/a", "two flushes timed out"));
    CHECK_ST(t_sync(&r), OK);
    CHECK_EQ(ramdisk_timeouts(&disk), 2);
    ramdisk_time_out(&disk, RAMDISK_WRITE, 1);
    CHECK(put_file(&r, "/t/b", "a write timed out, half of it landed"));
    CHECK_ST(t_sync(&r), OK);
    CHECK_EQ(ramdisk_timeouts(&disk), 3);
    ramdisk_time_out(&disk, RAMDISK_WRITE | RAMDISK_SYNC, 4);   /* one call's 4: its 5th try */
    CHECK(put_file(&r, "/t/c", "four timeouts in a row"));
    CHECK_ST(t_sync(&r), OK);
    CHECK_EQ(ramdisk_timeouts(&disk), 7);
    CHECK(fat_stop(&r));
    ramdisk_time_out(&disk, RAMDISK_READ, 3);
    CHECK(fat_start_words(&r, &disk, quick, 1));
    CHECK(file_is(&r, "/t/a", "two flushes timed out"));
    CHECK_EQ(ramdisk_timeouts(&disk), 10);
    CHECK(file_is(&r, "/t/b", "a write timed out, half of it landed"));
    CHECK(file_is(&r, "/t/c", "four timeouts in a row"));
    CHECK_ST(t_mkdir(&r, "/t/after"), OK);
    CHECK(fat_stop(&r));
    return ramdisk_destroy(&disk);
}

/* Past its tries a flush that times out fails as it always has: fs.sync
 * says so, each flush call having had exactly 5 tries ("a flush of the
 * disk's cache failed: ERR_TIMED_OUT (5 tries in"). The next sync, the
 * disk well again, flushes what was left, and a fresh fat finds the
 * volume as it was and writes on. */
bool t_fat_timeout_spent(void)
{
    struct fatrun r;
    CHECK(ramdisk_create(&disk, DISK_SECTORS));
    CHECK(fat_start_words(&r, &disk, quick, 1));
    CHECK(put_file(&r, "/a", "before"));
    CHECK_ST(t_sync(&r), OK);
    ramdisk_time_out(&disk, RAMDISK_SYNC, 1000);
    (void)t_mkdir(&r, "/b");   /* its own flush may fail too */
    CHECK(t_sync(&r) != OK);
    uint32_t n = ramdisk_timeouts(&disk);
    CHECK(n >= 5 && n % 5 == 0);   /* BLOCKCALL_TRIES a call, then no more */
    ramdisk_time_out(&disk, 0, 0);
    uint32_t syncs = ramdisk_syncs(&disk);
    CHECK_ST(t_sync(&r), OK);
    CHECK(ramdisk_syncs(&disk) > syncs);
    CHECK(fat_stop(&r));
    CHECK(fat_start(&r, &disk, false));
    CHECK(file_is(&r, "/a", "before"));
    CHECK_ST(t_mkdir(&r, "/c"), OK);
    CHECK(fat_stop(&r));
    return ramdisk_destroy(&disk);
}

/* A read the disk holds past fat's wait (quick-wait: 3 s) at queue depth
 * 32 (drv/nvme's: answers in completion order, so nothing tells fat when
 * the read is over): it lands late in the buffer, when the next request
 * comes or fat leaves. fat must end at once (exit 1, for a restart on a
 * new channel) with nothing written after, so no sector holds the late
 * bytes; the next fat finds the files and writes on. */
bool t_fat_late(void)
{
    struct fatrun r;
    CHECK(ramdisk_create(&disk, DISK_SECTORS));
    ramdisk_depth(&disk, 32);
    CHECK(fat_start_words(&r, &disk, quick, 2));
    CHECK_ST(t_mkdir(&r, "/t"), OK);
    CHECK(put_file(&r, "/t/a", "before the late read"));
    CHECK(fat_stop(&r));
    CHECK(fat_start_words(&r, &disk, quick, 2));   /* nothing of /t cached */
    uint32_t writes = ramdisk_writes(&disk);
    ramdisk_hold_read(&disk, false);
    CHECK(t_stat(&r, "/t/a", NULL, NULL, NULL) != OK);
    CHECK(t_mkdir(&r, "/t/after") != OK);
    CHECK(fat_ended(&r, 1));
    CHECK_EQ(ramdisk_late(&disk), 1);
    CHECK_EQ(ramdisk_writes(&disk), writes);
    CHECK(!ramdisk_has_late(&disk));
    CHECK(fat_start(&r, &disk, false));
    CHECK(file_is(&r, "/t/a", "before the late read"));
    CHECK(put_file(&r, "/t/b", "after it"));
    CHECK(fat_stop(&r));
    CHECK(!ramdisk_has_late(&disk));
    return ramdisk_destroy(&disk);
}
