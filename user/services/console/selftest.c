/* console: its selftest (`run console selftest`, console.h): log lines
 * made into notices, or not, the way notices.c says. Only the lines that
 * need no waiting: the mounts settle over seconds and are checked in QEMU
 * (tools/screen-test.sh). Runs without a screen or a kernel log: the
 * lines are fed to notice_take directly, each with the writer the
 * kernel's mark and init's table would have given it. */
#include "console.h"

static unsigned failures;

struct fed {
    enum log_writer w;   /* who wrote it */
    const char     *line;
};

/* After `lines` (ended by a NULL line), the last notice is `want` ("" for
 * none). */
static void expect(const char *what, const struct fed *lines, bool announce, const char *want)
{
    notice_reset();
    for (unsigned i = 0; lines[i].line; i++)
        notice_take(lines[i].line, strlen(lines[i].line), announce, lines[i].w);
    bool ok = !strcmp(notice_last(), want);
    printf("console: selftest: %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        printf("console: selftest:   said \"%s\", want \"%s\"\n", notice_last(), want);
        failures++;
    }
}

#define LINES(...) ((const struct fed[]){ __VA_ARGS__, { W_OTHER, NULL } })
#define K(text) { W_KERNEL, text }
#define I(text) { W_INIT, text }
#define D(text) { W_DEVMGR, text }
#define G(text) { W_LOGD, text }
#define O(text) { W_OTHER, text }

static void services(void)
{
    expect("a service crashed: init starts it again",
           LINES(K("[    5.000000] user: process \"music\" killed: page fault at rip 401000, "
                   "address 0 (thread \"music\")"),
                 I("[    5.000100] [init] init: bin/music was killed, code -1")),
           true, "music crashed (page fault): init is starting it again");
    expect("a service killed on request: no news",
           LINES(K("[    5.000000] user: process \"music\" killed by \"init\""),
                 I("[    5.000100] [init] init: bin/music was killed, code -1")),
           true, "");
    expect("another process crashed than the one init saw end",
           LINES(K("[    5.000000] user: process \"life\" killed: page fault at rip 1, "
                   "address 0 (thread \"life\")"),
                 I("[    5.000100] [init] init: bin/music was killed, code -1")),
           true, "");
    /* init_say's lines are debug_report lines: no "[init] " in front. */
    expect("init gave up on a service",
           LINES(I("[    9.000000] init: bin/mixer ended 11 times in a minute: not restarting "
                   "it")),
           true, "mixer kept stopping: init gave up on it (`log` says more)");
    expect("a driver crashed: devmgr starts it again",
           LINES(D("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in "
                   "100 ms")),
           true, "the hid driver (usb 1:0) crashed: devmgr is starting it again");
    expect("a driver killed on request: no news",
           LINES(D("[    7.000000] [devmgr] devmgr: 00:04.0 drv/edu was killed (KILL): "
                   "restart 1 in 100 ms")),
           true, "");
    expect("devmgr gave up on a driver",
           LINES(D("[    7.000000] [devmgr] devmgr: 00:1d.0 drv/usb-bus crashed after 5 "
                   "restarts in 60 s: giving up")),
           true, "the usb-bus driver (00:1d.0) kept failing: devmgr gave up on it");
    expect("... but not on the crash-test driver",
           LINES(D("[    7.000000] [devmgr] devmgr: 00:05.0 drv/crashtest crashed after 5 "
                   "restarts in 60 s: giving up (the crash-test driver: expected)")),
           true, "");
}

/* Lines that look like the system's but another process wrote: a program
 * can start a process called "init" or "devmgr", or report a line that
 * reads like the kernel's. */
static void impostors(void)
{
    expect("a process called init that isn't init",
           LINES(O("[    5.000000] [init] init: bin/music ended 11 times in a minute: not "
                   "restarting it")),
           true, "");
    expect("a process called devmgr that isn't devmgr",
           LINES(O("[    5.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in "
                   "1 ms")),
           true, "");
    expect("a process called logd that isn't logd",
           LINES(O("[   20.000000] [logd] logd: no /data/logs (ERR_NO_SPACE): the log is not "
                   "being saved; trying again")),
           true, "");
    expect("a kernel-looking line a process wrote, then init's",
           LINES(O("[    5.000000] user: process \"music\" killed: page fault at rip 1, "
                   "address 0 (thread \"music\")"),
                 I("[    5.000100] [init] init: bin/music was killed, code -1")),
           true, "");
    expect("devmgr's line from init's process: not devmgr's news",
           LINES(I("[    7.000000] [init] devmgr: usb 1:0 drv/hid crashed: restart 1 in 1 ms")),
           true, "");
}

static void others(void)
{
    expect("/data full",
           LINES(G("[   20.000000] [logd] logd: no /data/logs (ERR_NO_SPACE): the log is not "
                   "being saved; trying again")),
           true, "/data is full: the boot log is not being saved");
    expect("/data gone is the mounts' news, not logd's",
           LINES(G("[   20.000000] [logd] logd: no /data/logs (ERR_PEER_CLOSED): the log is "
                   "not being saved; trying again")),
           true, "");
    expect("nothing while the log itself is on the screen",
           LINES(D("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in "
                   "100 ms")),
           false, "");
    expect("the same notice again soon after: said once",
           LINES(D("[    7.000000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 1 in "
                   "100 ms"),
                 I("[    7.200000] init: bin/mixer ended 11 times in a minute: not "
                   "restarting it"),
                 D("[    7.300000] [devmgr] devmgr: usb 1:0 drv/hid crashed: restart 2 in "
                   "200 ms")),
           true, "mixer kept stopping: init gave up on it (`log` says more)");
}

int console_selftest(void)
{
    if (!text_init()) {
        printf("console: selftest: out of memory\n");
        return 1;
    }
    services();
    impostors();
    others();
    printf("console: selftest %s (%u failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return (int)failures;
}
