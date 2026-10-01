/* utest: the children of the namespace tests (ns.c, nsfat.c, svc.c),
 * started as "utest ns-..." with a namespace the test chose, and "utest
 * fscat <path>", "utest fs-hold <path>" and "utest fs-put <path> <text>"
 * for the shell's scripts. Each returns 0, or a number that says which
 * check failed. */
#include <os.h>
#include "utest.h"

#define RAM "/t"   /* where ns.c mounts its RAM filesystem */

/* Wait (up to 10 s) until fs_stat(path) gives `want`. */
static bool wait_stat(const char *path, status_t want)
{
    for (uint64_t end = now() + 10 * NS_PER_S; now() < end;) {
        if (fs_stat(path, NULL, NULL, NULL) == want)
            return true;
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    return false;
}

/* "ns-only <mount>": that mount, with /hello in it, is all we have.
 * "ns-all <mount>": that mount and /boot. */
static int ns_mounts(const char *point, bool all)
{
    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/hello", point);
    if (!wait_stat(path, OK))
        return 40;
    if (!ns_holds(path, NS_HELLO))
        return 41;
    if (fs_stat("/boot/init.cfg", NULL, NULL, NULL) != (all ? OK : ERR_NOT_FOUND))
        return 42;
    struct fs_entry e;
    unsigned n = 0;
    bool saw = false;
    for (; fs_readdir("/", n, &e) == OK; n++)
        saw |= !strcmp(e.name, point + 1);
    if (!saw || (!all && n != 1))
        return 43;
    /* Its SR_NS end can only be read: nothing can be sent back up it. */
    struct ns_msg m = { .kind = NS_MOUNT };
    if (jam_channel_write(startup_handle(SR_NS), &m, NS_MSG_SIZE(0), NULL, 0) != ERR_ACCESS_DENIED)
        return 44;
    return 0;
}

/* "ns-none": no mount at all. */
static int ns_none(void)
{
    struct fs_entry e;
    struct jfile f;
    if (fs_stat("/boot/init.cfg", NULL, NULL, NULL) != ERR_NOT_FOUND)
        return 50;
    if (fs_readdir("/", 0, &e) != ERR_NOT_FOUND)
        return 51;
    if (file_open("/boot/init.cfg", FS_READ, &f) != ERR_NOT_FOUND)
        return 52;
    return 0;
}

/* "ns-late": open /t/hello, tell the parent (SR_USER), then /u arrives and
 * /t goes; the open file still reads. */
static int ns_late(void)
{
    struct jfile f;
    char buf[64];
    size_t got = 0;
    uint32_t ready = 0;
    if (file_open(RAM "/hello", FS_READ, &f) != OK)
        return 60;
    if (jam_channel_write(startup_handle(SR_USER), &ready, sizeof(ready), NULL, 0) != OK)
        return 61;
    if (!wait_stat("/u/hello", OK))
        return 62;
    if (!wait_stat(RAM "/hello", ERR_NOT_FOUND))
        return 63;
    if (file_read(&f, 0, buf, sizeof(buf), &got) != OK || got != strlen(NS_HELLO) ||
        memcmp(buf, NS_HELLO, got))
        return 64;
    file_close(&f);
    return ns_holds("/u/hello", NS_HELLO) ? 0 : 65;
}

/* "ns-sleeper": say we run, then touch nothing until the parent says go
 * (SR_USER), while it changes our namespace hundreds of times
 * (nsnotice.c); then our mounts are what it has now: /boot and /w, not /v. */
static int ns_sleeper(void)
{
    signals_t seen;
    uint32_t ready = 0;
    if (jam_channel_write(startup_handle(SR_USER), &ready, sizeof(ready), NULL, 0) != OK)
        return 69;
    if (jam_object_wait_one(startup_handle(SR_USER), SIG_READABLE, now() + 120 * NS_PER_S,
                            &seen) != OK)
        return 70;
    if (fs_stat("/w/init.cfg", NULL, NULL, NULL) != OK)
        return 71;
    if (fs_stat("/v/init.cfg", NULL, NULL, NULL) != ERR_NOT_FOUND)
        return 72;
    if (fs_stat("/boot/init.cfg", NULL, NULL, NULL) != OK)
        return 73;
    struct fs_entry e;
    unsigned n = 0;
    while (fs_readdir("/", n, &e) == OK)
        n++;
    return n == 2 ? 0 : 74;
}

/* "fscat <path>": the file's text, for the shell's scripts (a program
 * `run` starts sees the shell's mounts). */
static int fscat(const char *path)
{
    struct jfile f;
    char buf[256];
    size_t got = 0;
    status_t st = file_open(path, FS_READ, &f);
    if (st == OK)
        st = file_read(&f, 0, buf, sizeof(buf) - 1, &got);
    if (st != OK) {
        printf("fscat: %s: %s\n", path, status_str(st));
        return 1;
    }
    file_close(&f);
    buf[got] = '\0';
    printf("fscat: %s", buf);
    return 0;
}

/* "fs-put <path> <text>": write the file (created or emptied) and say how
 * it went, for the shell's scripts: a program's /data leaves etc alone. */
static int fs_put(const char *path, const char *text)
{
    status_t st = ns_put(path, text);
    printf("fs-put: %s: %s\n", path, status_str(st));
    return st == OK ? 0 : 1;
}

/* "fs-hold <path>": write the file a piece at a time and never sync or
 * close it, until we are killed: a file that is unsynced whenever the
 * plug is pulled (tools/data-test.sh). */
static int fs_hold(const char *path)
{
    static uint8_t piece[4096];
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    memset(piece, 'x', sizeof(piece));
    for (uint64_t off = 0; st == OK; off += sizeof(piece)) {
        if (off < (8u << 20))   /* then it only stays open */
            st = file_write(&f, off, piece, sizeof(piece), &done);
        if (off == 0 && st == OK)
            printf("fs-hold: writing %s\n", path);
        jam_nanosleep(now() + 5 * NS_PER_MS);
    }
    printf("fs-hold: %s: %s\n", path, status_str(st));
    return 1;
}

int ns_child(int argc, char **argv)
{
    const char *m = argv[1];
    if (!strcmp(m, "ns-only") && argc > 2)
        return ns_mounts(argv[2], false);
    if (!strcmp(m, "ns-all") && argc > 2)
        return ns_mounts(argv[2], true);
    if (!strcmp(m, "ns-none"))
        return ns_none();
    if (!strcmp(m, "ns-late"))
        return ns_late();
    if (!strcmp(m, "ns-sleeper"))
        return ns_sleeper();
    if (!strcmp(m, "ns-svc") && argc > 2)
        return ns_svc_child(argv[2]);
    if (!strcmp(m, "ns-view") && argc > 2)
        return ns_view_child(argv[2]);
    if (!strcmp(m, "fscat") && argc > 2)
        return fscat(argv[2]);
    if (!strcmp(m, "fs-hold") && argc > 2)
        return fs_hold(argv[2]);
    if (!strcmp(m, "fs-put") && argc > 3)
        return fs_put(argv[2], argv[3]);
    if (!strcmp(m, "fat-shell"))
        return fat_shell();
    return 127;
}
