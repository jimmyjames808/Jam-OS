/* console: its selftest (`run console selftest`, console.h): log lines
 * made into notices, or not, the way notices.c says. Only the lines that
 * need no waiting: the mounts settle over seconds and are checked in QEMU
 * (tools/screen-test.sh). Runs without a screen or a kernel log: the
 * lines are fed to notice_take directly. */
#include "console.h"

static unsigned failures;

static void feed(const char *line, bool announce)
{
    notice_take(line, strlen(line), announce);
}

/* After `lines` (NULL-ended), the last notice is `want` ("" for none). */
static void expect(const char *what, const char *const *lines, bool announce, const char *want)
{
    notice_reset();
    for (unsigned i = 0; lines[i]; i++)
        feed(lines[i], announce);
    bool ok = !strcmp(notice_last(), want);
    printf("console: selftest: %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        printf("console: selftest:   said \"%s\", want \"%s\"\n", notice_last(), want);
        failures++;
    }
}

#define LINES(...) ((const char *const[]){ __VA_ARGS__, NULL })

static void services(void)
{
    expect("a service crashed: init starts it again",
           LINES("[    5.000000] user: process \"music\" killed: page fault at rip 401000, "
                 "address 0 (thread \"music\")",
                 "[    5.000100] [init] init: bin/music was killed, code -1"),
           true, "music crashed (page fault): init is starting it again");
    expect("a service killed on request: no news",
           LINES("[    5.000000] user: process \"music\" killed by \"init\"",
                 "[    5.000100] [init] init: bin/music was killed, code -1"),
           true, "");
    expect("another process crashed than the one init saw end",
           LINES("[    5.000000] user: process \"life\" killed: page fault at rip 1, address 0 "
                 "(thread \"life\")",
                 "[    5.000100] [init] init: bin/music was killed, code -1"),
           true, "");
    expect("init gave up on a service",
           LINES("[    9.000000] [init] init: bin/mixer ended 11 times in a minute: not "
                 "restarting it"),
           true, "mixer kept stopping: init gave up on it (`log` says more)");
    expect("a driver crashed: devmgr starts it again",
           LINES("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in 100 ms"),
           true, "the hid driver (usb 1:0) crashed: devmgr is starting it again");
    expect("a driver killed on request: no news",
           LINES("[    7.000000] [devmgr] devmgr: 00:04.0 drv/edu was killed (KILL): restart 1 "
                 "in 100 ms"),
           true, "");
    expect("devmgr gave up on a driver",
           LINES("[    7.000000] [devmgr] devmgr: 00:1d.0 drv/usb-bus crashed after 5 restarts "
                 "in 60 s: giving up"),
           true, "the usb-bus driver (00:1d.0) kept failing: devmgr gave up on it");
    expect("... but not on the crash-test driver",
           LINES("[    7.000000] [devmgr] devmgr: 00:05.0 drv/crashtest crashed after 5 "
                 "restarts in 60 s: giving up (the crash-test driver: expected)"),
           true, "");
}

static void others(void)
{
    expect("/data full",
           LINES("[   20.000000] [logd] logd: no /data/logs (ERR_NO_SPACE): the log is not "
                 "being saved; trying again"),
           true, "/data is full: the boot log is not being saved");
    expect("/data gone is the mounts' news, not logd's",
           LINES("[   20.000000] [logd] logd: no /data/logs (ERR_PEER_CLOSED): the log is not "
                 "being saved; trying again"),
           true, "");
    expect("a line from a process with another name",
           LINES("[    5.000000] [initx] init: bin/music ended 11 times in a minute: not "
                 "restarting it",
                 "[    5.000000] [jamjar] devmgr: usb 1:0 drv/hid crashed: restart 1 in 1 ms"),
           true, "");
    expect("nothing while the log itself is on the screen",
           LINES("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in 100 ms"),
           false, "");
    expect("the same notice again soon after: said once",
           LINES("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in 100 ms",
                 "[    7.200000] [init] init: bin/mixer ended 11 times in a minute: not "
                 "restarting it",
                 "[    7.300000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 2 in 200 ms"),
           true, "mixer kept stopping: init gave up on it (`log` says more)");
}

int console_selftest(void)
{
    if (!text_init()) {
        printf("console: selftest: out of memory\n");
        return 1;
    }
    services();
    others();
    printf("console: selftest %s (%u failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return (int)failures;
}
