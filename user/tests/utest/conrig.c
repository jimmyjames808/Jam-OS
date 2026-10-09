/* utest: a connection console and its remote terminal (conrig.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/console.h>
#include <os.h>
#include <termconn.h>
#include "conrig.h"
#include "utest.h"

struct conrig rig;

/* Start the console with argv (argc entries) and the kernel log if klog. */
bool rig_start(const char *const *argv, int argc, bool klog)
{
    handle_t admin_theirs, master_theirs, ctl_theirs, root = startup_handle(SR_RESOURCE);
    memset(&rig, 0, sizeof(rig));
    CHECK_ST(new_job(&rig.job), OK);
    CHECK_ST(jam_channel_create(&rig.admin, &admin_theirs), OK);
    CHECK_ST(jam_channel_create(&rig.master, &master_theirs), OK);
    CHECK_ST(jam_channel_create(&rig.ctl, &ctl_theirs), OK);
    struct spawn_handle x[4] = { { SR_USER + 0, admin_theirs }, { CONN_MASTER_ROLE, master_theirs },
                                 { CONN_CTL_ROLE, ctl_theirs } };
    unsigned nx = 3;
    handle_t klog_root;
    if (klog) {
        CHECK_ST(jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_ROOT_KLOG, &klog_root), OK);
        x[nx++] = (struct spawn_handle){ SR_RESOURCE, klog_root };
    }
    struct spawn_args a = { .path = "bin/console", .name = "console-conn", .argc = argc,
                            .argv = argv, .job = rig.job, .extra = x, .nextra = nx };
    CHECK_ST(spawn(&a, &rig.proc), OK);
    return true;
}

void rig_stop(bool ok)
{
    if (!ok)
        (void)jam_job_kill(rig.job);
    jam_handle_close(rig.proc);
    jam_handle_close(rig.admin);
    jam_handle_close(rig.master);
    jam_handle_close(rig.ctl);
    jam_handle_close(rig.job);
}

/* ---- the remote terminal ----------------------------------------------------------- */

/* One message from the master, appended to out (until deadline). */
bool rig_take(uint64_t until)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(rig.master, SIG_READABLE | SIG_PEER_CLOSED, until, &seen);
    if (st != OK || !(seen & SIG_READABLE))
        return false;
    static uint8_t msg[CONN_OUT_MSG_MAX + 16];
    uint32_t n = 0, nh = 0;
    if (drv_channel_read(rig.master, msg, sizeof(msg), &n, NULL, 0, &nh) != OK)
        return false;
    if (rig.len + n > OUT_CAP) {   /* keep the newest half */
        size_t keep = OUT_CAP / 2, drop = rig.len - keep;
        memmove(rig.out, rig.out + drop, keep);
        rig.len = keep;
        rig.from = rig.from > drop ? rig.from - drop : 0;
    }
    memcpy(rig.out + rig.len, msg, n);
    rig.len += n;
    return true;
}

void rig_dump_tail(void)
{
    size_t from = rig.len > 300 ? rig.len - 300 : 0;
    printf("utest: the master's last bytes: \"");
    for (size_t i = from; i < rig.len; i++) {
        uint8_t c = (uint8_t)rig.out[i];
        if (c >= 0x20 && c < 0x7f)
            printf("%c", c);
        else
            printf("\\x%02x", c);
    }
    printf("\"\n");
}

/* Wait until the bytes needle (n of them) come on the master, after the
 * last match. */
bool rig_expect_n(const char *needle, size_t nl, unsigned ms)
{
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    for (;;) {
        for (size_t i = rig.from; i + nl <= rig.len; i++)
            if (!memcmp(rig.out + i, needle, nl)) {
                rig.from = i + nl;
                return true;
            }
        if (!rig_take(until)) {
            printf("utest: %s: not seen on the master within %u ms: \"%s\"\n", utest_cur, ms,
                   needle);
            rig_dump_tail();
            return false;
        }
    }
}

bool rig_expect(const char *needle, unsigned ms)
{
    return rig_expect_n(needle, strlen(needle), ms);
}

/* Read the master for ms milliseconds, whatever comes. */
void rig_settle(unsigned ms)
{
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    while (rig_take(until))
        ;
}

bool rig_typed(const char *s, size_t n)
{
    CHECK_ST(jam_channel_write(rig.master, s, (uint32_t)n, NULL, 0), OK);
    return true;
}

bool rig_saw(const char *needle)
{
    size_t nl = strlen(needle);
    for (size_t i = rig.from; i + nl <= rig.len; i++)
        if (!memcmp(rig.out + i, needle, nl))
            return true;
    return false;
}

bool rig_size_is(handle_t client, uint16_t cols, uint16_t rows)
{
    uint16_t c = 0, r = 0;
    CHECK_ST(console_size_within(client, CON_WAIT, &c, &r), OK);
    CHECK_EQ(c, cols);
    CHECK_EQ(r, rows);
    return true;
}

bool rig_exit_code_is(handle_t proc, int code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(proc, CON_WAIT, &info), OK);
    CHECK_EQ(info.killed, 0);
    CHECK_EQ(info.exit_code, code);
    return true;
}

