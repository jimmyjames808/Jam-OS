/* utest: the namespace over the real fat service.
 *
 * bin/fat over a RAM disk (fattest.h), its `fs` channel mounted in utest's
 * own namespace: libos's file calls against a FAT volume, with names that
 * need long-name entries, and a child that is given the mount. The
 * "fat-shell" mode puts a shell on top of the same thing, for the shell's
 * scripts (tools/shell-tests/files-fat.txt). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define FAT     "/f"
#define DISK_MB 16
#define BIG     150000u   /* bytes: more than two transfer buffers, many clusters */

static struct ramdisk disk;
static uint8_t big[BIG], back[BIG];

/* A big file with a long name, written, read back, added to. */
static bool fat_big_file(void)
{
    struct jfile f;
    size_t done = 0;
    uint64_t size = 0, mtime = 0;
    bool dir = true;
    CHECK_ST(fs_mkdir(FAT "/My Docs"), OK);
    CHECK_ST(fs_mkdir(FAT "/My Docs"), ERR_ALREADY_EXISTS);
    CHECK_ST(file_open(FAT "/My Docs/big file.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    for (unsigned i = 0; i < BIG; i++)
        big[i] = (uint8_t)(i * 7 + (i >> 9));
    CHECK_ST(file_write(&f, 0, big, BIG, &done), OK);
    CHECK_EQ(done, BIG);
    CHECK_ST(file_stat(&f, &size, NULL), OK);
    CHECK_EQ(size, BIG);
    CHECK_ST(file_read(&f, 0, back, BIG + 1, &done), OK);
    CHECK_EQ(done, BIG);
    CHECK(!memcmp(big, back, BIG));
    CHECK_ST(file_sync(&f), OK);
    file_close(&f);
    CHECK_ST(file_open(FAT "/My Docs/big file.bin", FS_WRITE | FS_APPEND, &f), OK);
    CHECK_ST(file_write(&f, 0, "tail", 4, &done), OK);
    file_close(&f);
    CHECK_ST(fs_stat(FAT "/My Docs/big file.bin", &size, &dir, &mtime), OK);
    CHECK(size == BIG + 4 && !dir && mtime > 0);
    CHECK_ST(file_open(FAT "/My Docs/big file.bin", FS_READ, &f), OK);
    CHECK_ST(file_read(&f, BIG - 2, back, 16, &done), OK);
    CHECK(done == 6 && !memcmp(back, big + BIG - 2, 2) && !memcmp(back + 2, "tail", 4));
    CHECK_ST(file_write(&f, 0, "x", 1, &done), ERR_ACCESS_DENIED);   /* opened to read */
    file_close(&f);
    return true;
}

/* Names as typed, renames, what fat refuses, the free space. */
static bool fat_tree(void)
{
    struct fs_entry e;
    struct jfile f;
    uint64_t total = 0, free0 = 0, free1 = 0;
    bool ro = true, dir = false;
    char label[17];
    CHECK_ST(fs_statfs(FAT, &total, &free0, &ro, label), OK);
    CHECK(total > (DISK_MB / 2) << 20 && free0 <= total && !ro && !strcmp(label, "JAMOS-DATA"));
    if (!fat_big_file())
        return false;
    CHECK_ST(fs_readdir(FAT "/My Docs", 0, &e), OK);
    CHECK(!strcmp(e.name, "big file.bin") && !e.is_dir && e.size == BIG + 4);
    CHECK_ST(fs_readdir(FAT "/My Docs", 1, &e), ERR_NOT_FOUND);
    CHECK_ST(fs_readdir(FAT "/My Docs/big file.bin", 0, &e), ERR_WRONG_TYPE);
    CHECK_ST(ns_put(FAT "/notes.txt", "lower case\n"), OK);
    bool notes = false, docs = false;
    for (uint32_t i = 0; fs_readdir(FAT, i, &e) == OK; i++) {
        notes |= !strcmp(e.name, "notes.txt") && !e.is_dir;
        docs |= !strcmp(e.name, "My Docs") && e.is_dir;
    }
    CHECK(notes && docs);
    CHECK_ST(fs_rename(FAT "/notes.txt", FAT "/My Docs/Notes Renamed.txt"), OK);
    CHECK(ns_holds(FAT "/My Docs/Notes Renamed.txt", "lower case\n"));
    CHECK_ST(fs_stat(FAT "/notes.txt", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(fs_rename(FAT "/My Docs/Notes Renamed.txt", "/boot/x"), ERR_NOT_SUPPORTED);
    /* ".." stops at the mount's root here too. */
    CHECK_ST(fs_stat(FAT "/My Docs/../../My Docs", NULL, &dir, NULL), OK);
    CHECK(dir);
    CHECK_ST(file_open(FAT "/My Docs", FS_READ, &f), ERR_WRONG_TYPE);
    CHECK_ST(fs_unlink(FAT "/My Docs"), ERR_BAD_STATE);   /* not empty */
    CHECK_ST(fs_mkdir(FAT "/bad:name"), ERR_INVALID_ARGS);
    CHECK_ST(fs_sync(FAT), OK);
    CHECK_ST(fs_statfs(FAT "/My Docs", NULL, &free1, NULL, NULL), OK);
    CHECK(free1 < free0 && free0 - free1 >= BIG);
    CHECK_ST(fs_unlink(FAT "/My Docs/big file.bin"), OK);
    CHECK_ST(fs_unlink(FAT "/My Docs/Notes Renamed.txt"), OK);
    CHECK_ST(fs_unlink(FAT "/My Docs"), OK);
    CHECK_ST(fs_statfs(FAT, NULL, &free1, NULL, NULL), OK);
    CHECK_EQ(free1, free0);
    return true;
}

/* A child given only the fat mount reads a file there (two processes on
 * one `fs` channel). */
static bool fat_child(void)
{
    handle_t proc;
    static const char *const only[] = { FAT, NULL };
    struct spawn_handle none = { 0, HANDLE_INVALID };
    CHECK_ST(ns_put(FAT "/hello", NS_HELLO), OK);
    CHECK_ST(ns_child_start("ns-only", FAT, only, none, NULL, &proc), OK);
    return ns_child_exits(proc, 0);
}

bool t_ns_fat_mount(void)
{
    struct fatrun r;
    handle_t fs;
    if (!ramdisk_create(&disk, DISK_MB * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &fs), OK);
    CHECK_ST(ns_mount(FAT, fs), OK);
    bool ok = fat_tree() && fat_child();
    CHECK_ST(ns_unmount(FAT), OK);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}

/* "utest fat-shell": bin/shell on our console channel, with our namespace
 * and, as its /data (in place of the stick's), a fresh FAT volume on a RAM
 * disk. Runs until it is killed (the shell that ran us, on Ctrl+C). */
int fat_shell(void)
{
    struct fatrun r;
    handle_t fs, job, proc;
    utest_cur = "fat-shell";
    if (!ramdisk_create(&disk, DISK_MB * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return 1;
    status_t st = jam_handle_duplicate(r.fs, RIGHT_SAME, &fs);
    if (st == OK)
        st = ns_mount("/data", fs);
    if (st == OK)
        st = new_job(&job);
    const char *argv[] = { "bin/shell" };
    struct spawn_handle x = { SR_CONSOLE, startup_handle(SR_CONSOLE) };
    struct spawn_args a = {
        .path = "bin/shell", .argc = 1, .argv = argv, .job = job, .extra = &x,
        .nextra = x.h ? 1 : 0, .ns = NS_ALL,
    };
    if (st == OK)
        st = spawn(&a, &proc);
    if (st != OK) {
        printf("utest: fat-shell: %s\n", status_str(st));
        return 1;
    }
    signals_t seen;
    (void)jam_object_wait_one(proc, SIG_TERMINATED, DEADLINE_NEVER, &seen);   /* or we are killed */
    return 0;
}
