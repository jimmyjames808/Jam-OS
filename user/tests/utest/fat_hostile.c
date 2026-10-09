/* utest: fat (user/services/fat) against clients that misbehave, over a RAM
 * disk: one that never reads its answers (fat_unread_flood). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

static struct ramdisk disk;

#define FLOOD      300u           /* readdir requests the hostile client never reads */
#define LONG_FILES 40u            /* files with long names: a full readdir batch */
#define FAT_JOB_MSG_BYTES (1u << 20)   /* volumes' limit on a fat's queued messages */
#define UNREAD_MOST 16u           /* answers fat may leave at a client that reads none */

/* /big holds LONG_FILES files with 200-byte names: one readdir answer is a
 * whole batch, about 8 KiB. */
static bool make_long_dir(const struct fatrun *r)
{
    char path[256];
    CHECK_ST(t_mkdir(r, "/big"), OK);
    for (unsigned i = 0; i < LONG_FILES; i++) {
        int n = snprintf(path, sizeof(path), "/big/%02u", i);
        while (n < 5 + 200)
            path[n++] = 'x';
        path[n] = '\0';
        struct tfile f;
        CHECK_ST(t_open(r, path, FS_READ | FS_WRITE | FS_CREATE, &f), OK);
        t_close(&f);
    }
    struct tbatch b;
    CHECK_ST(t_batch(r, "/big", 0, &b), OK);
    CHECK(b.len > FS_READDIR_BYTES - 300);   /* a full batch */
    return true;
}

/* A view whose client writes FLOOD readdir requests and reads none of the
 * answers: each is charged to fat's job until read. fat must keep
 * answering everyone else (its job is volumes' 1 MiB of queued messages),
 * stay alive, and answer the flood once it is read. Before the fix it
 * answered all of them, its job filled, every other answer was dropped
 * and its port bindings failed. */
bool t_fat_unread_flood(void)
{
    struct fatrun r;
    handle_t v;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false) ||
        !make_long_dir(&r))
        return false;
    CHECK_ST(jam_job_set_limit(r.job, JOB_LIMIT_MSG_BYTES, FAT_JOB_MSG_BYTES), OK);
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, 0, &v), OK);
    for (uint32_t i = 0; i < FLOOD; i++)
        CHECK_ST(fs_readdir_send(v, 0x100 + i, "/big", 4, 0), OK);
    jam_sleep_until(now() + 500 * NS_PER_MS);   /* fat does what it will of them */
    struct handle_info hi;
    CHECK_ST(jam_handle_info(v, &hi), OK);
    printf("utest: fat_unread_flood: %u of %u answers sent before any was read, %u requests "
           "waiting at fat\n", hi.queued, FLOOD, hi.peer_queued);
    CHECK(hi.queued <= UNREAD_MOST);
    /* Everyone else is answered meanwhile, again and again. */
    static struct tbatch b;
    for (unsigned k = 0; k < 50; k++) {
        uint64_t size = 0;
        uint8_t dir = 0;
        CHECK_ST(fs_stat_until(r.fs, now() + 2 * NS_PER_S, "/big", 4, &size, &dir, NULL, NULL),
                 OK);
        CHECK(dir);
        CHECK_ST(fs_readdir_until(r.fs, now() + 2 * NS_PER_S, "/big", 4, 0, &b.next, &b.count,
                                  b.entries, &b.len), OK);
        CHECK(b.count > 0);
    }
    /* Reading the answers lets the rest through, each one right. */
    _Alignas(8) static uint8_t rep[FS_REP_MAX];
    uint32_t got = 0;
    uint64_t until = now() + 20 * NS_PER_S;
    while (got < FLOOD) {
        struct idl_msg m;
        status_t st = idl_reply_read(v, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            CHECK(now() < until);
            (void)jam_object_wait_one(v, SIG_READABLE, until, &seen);
            continue;
        }
        CHECK_ST(st, OK);
        CHECK_EQ(m.txid, 0x100 + got);   /* in order */
        CHECK_ST(fs_readdir_result(rep, &m, &b.next, &b.count, b.entries, &b.len), OK);
        CHECK(b.count > 0);
        got++;
    }
    CHECK_ST(jam_handle_close(v), OK);
    return fat_stop(&r);
}
