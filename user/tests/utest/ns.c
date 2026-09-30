/* utest: the file namespace (<os.h> "files"), end to end.
 *
 * /boot is the bootfs server init mounted (read-only); the read-write
 * paths run against bin/ramfs, the tests' RAM filesystem, started here and
 * mounted in utest's own namespace. Covered: the path rules (absolute, at
 * most FS_PATH_MAX - 1 bytes, ".." never leaves a mount), every file call,
 * a server that dies (ERR_PEER_CLOSED, no hang), what a child is given
 * (only the mounts named), mounts that reach a running child while its
 * open files stay valid, malformed ns messages, and spawn from a VMO.
 *
 * The children are this program's "ns-..." modes (nschild.c). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/bootfs.h>
#include <os.h>
#include "utest.h"

#define RAM "/t"
#define BIG 150000u   /* bytes: more than two transfer buffers */

static uint8_t big[BIG], back[BIG];

/* ---- helpers ------------------------------------------------------------------------ */

/* /boot is there (init and the shell give it), or the test is skipped. */
static bool have_boot(void)
{
    bool dir = false;
    if (fs_stat("/boot", NULL, &dir, NULL) == OK && dir)
        return true;
    printf("utest: %s: no /boot in our namespace (not started by init or the shell?): "
           "skipped\n", utest_cur);
    return false;
}

struct ram {
    handle_t job, proc;   /* bin/ramfs */
};

/* Start bin/ramfs and mount it at point. */
static bool ram_start(const char *point, struct ram *r)
{
    handle_t mine, theirs;
    CHECK_ST(new_job(&r->job), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    const char *argv[] = { "ramfs" };
    struct spawn_handle x = { SR_USER + 0, theirs };
    struct spawn_args a = {
        .path = "bin/ramfs", .argc = 1, .argv = argv, .job = r->job, .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&a, &r->proc), OK);
    CHECK_ST(ns_mount(point, mine), OK);
    return true;
}

/* Unmount it: with its last client gone it exits 0 and leaves nothing. */
static bool ram_stop(const char *point, struct ram *r)
{
    struct process_info info;
    CHECK_ST(ns_unmount(point), OK);
    CHECK_ST(spawn_wait(r->proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    struct job_info ji;
    CHECK_ST(info_of(r->job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(r->proc), OK);
    CHECK_ST(jam_handle_close(r->job), OK);
    return true;
}

status_t ns_put(const char *path, const char *text)
{
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    st = file_write(&f, 0, text, strlen(text), &done);
    file_close(&f);
    return st == OK && done != strlen(text) ? ERR_IO : st;
}

bool ns_holds(const char *path, const char *text)
{
    struct jfile f;
    char buf[128];
    size_t got = 0;
    if (file_open(path, FS_READ, &f) != OK)
        return false;
    status_t st = file_read(&f, 0, buf, sizeof(buf), &got);
    file_close(&f);
    return st == OK && got == strlen(text) && !memcmp(buf, text, got);
}

status_t ns_child_start(const char *mode, const char *arg, const char *const *ns,
                               struct spawn_handle extra, handle_t *ns_out, handle_t *proc)
{
    handle_t job;
    status_t st = new_job(&job);
    if (st != OK)
        return st;
    const char *argv[] = { "utest", mode, arg };
    struct spawn_args a = {
        .path = "bin/utest", .name = "utest-ns", .argc = arg ? 3 : 2, .argv = argv, .job = job,
        .extra = extra.h ? &extra : NULL, .nextra = extra.h ? 1 : 0, .ns = ns, .ns_out = ns_out,
    };
    st = spawn(&a, proc);
    jam_handle_close(job);   /* the process keeps its job alive */
    return st;
}

bool ns_child_exits(handle_t proc, int64_t code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 20 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, code);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* ---- /boot: the bootfs server -------------------------------------------------------- */

bool t_ns_boot_mount(void)
{
    if (!have_boot())
        return true;
    /* stat and readdir */
    uint64_t size = 0, mtime = 1;
    bool dir = true;
    CHECK_ST(fs_stat("/boot/bin/utest", &size, &dir, &mtime), OK);
    CHECK(size > 0 && !dir && mtime == 0);
    CHECK_ST(fs_stat("/boot/bin", &size, &dir, NULL), OK);
    CHECK(dir && size == 0);
    CHECK_ST(fs_stat("/boot/nosuch", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(fs_stat("/boot/bin/utest/x", NULL, NULL, NULL), ERR_NOT_FOUND);
    bool saw_bin = false, saw_cfg = false;
    struct fs_entry e;
    uint32_t n = 0;
    for (; fs_readdir("/boot", n, &e) == OK; n++) {
        saw_bin |= !strcmp(e.name, "bin") && e.is_dir;
        saw_cfg |= !strcmp(e.name, "init.cfg") && !e.is_dir && e.size > 0;
        CHECK(strcmp(e.name, "utest") != 0);   /* not the files of bin */
    }
    CHECK(saw_bin && saw_cfg && n >= 2 && n < 16);
    CHECK_ST(fs_readdir("/boot", n, &e), ERR_NOT_FOUND);
    CHECK_ST(fs_readdir("/boot/init.cfg", 0, &e), ERR_WRONG_TYPE);
    /* "/" lists the mount points */
    CHECK_ST(fs_stat("/", &size, &dir, NULL), OK);
    CHECK(dir);
    bool saw_boot = false;
    for (n = 0; fs_readdir("/", n, &e) == OK; n++)
        saw_boot |= !strcmp(e.name, "boot") && e.is_dir;
    CHECK(saw_boot);

    /* A whole file, in pieces bigger than the transfer buffer, is the
     * image's bytes. */
    const struct bootfs_view *img;
    const void *data;
    uint64_t len;
    CHECK_ST(bootfs_default(&img), OK);
    CHECK_ST(bootfs_lookup(img, "bin/utest", &data, &len), OK);
    struct jfile f;
    CHECK_ST(file_open("/boot/bin/utest", FS_READ, &f), OK);
    CHECK_EQ(f.size, len);
    CHECK(len > BIG);
    size_t got = 0;
    CHECK_ST(file_read(&f, 4096, back, BIG, &got), OK);
    CHECK_EQ(got, BIG);
    CHECK(!memcmp(back, (const uint8_t *)data + 4096, BIG));
    CHECK_ST(file_read(&f, len - 10, back, 100, &got), OK);   /* short at the end */
    CHECK_EQ(got, 10);
    CHECK_ST(file_read(&f, len + 5, back, 100, &got), OK);
    CHECK_EQ(got, 0);
    uint64_t fsize = 0;
    CHECK_ST(file_stat(&f, &fsize, &mtime), OK);
    CHECK_EQ(fsize, len);
    file_close(&f);
    CHECK(!f.ch && !f.buf);
    return true;
}

/* /boot is read-only, all the way down. */
bool t_ns_boot_read_only(void)
{
    if (!have_boot())
        return true;
    struct jfile f;
    size_t got = 0;
    CHECK_ST(file_open("/boot/bin/utest", FS_READ, &f), OK);
    CHECK_ST(file_write(&f, 0, "x", 1, &got), ERR_ACCESS_DENIED);
    CHECK_ST(file_truncate(&f, 0), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_write(f.buf_vmo, 0, "x", 1), ERR_ACCESS_DENIED);   /* its buffer too */
    CHECK_ST(file_sync(&f), OK);
    file_close(&f);
    struct jfile g;
    CHECK_ST(file_open("/boot/init.cfg", FS_READ | FS_WRITE, &g), ERR_ACCESS_DENIED);
    CHECK_ST(file_open("/boot/new", FS_WRITE | FS_CREATE, &g), ERR_ACCESS_DENIED);
    CHECK_ST(file_open("/boot/init.cfg", FS_READ | 0x100, &g), ERR_INVALID_ARGS);
    CHECK_ST(file_open("/boot/init.cfg", FS_READ | FS_CREATE, &g), ERR_INVALID_ARGS);
    CHECK_ST(file_open("/boot/init.cfg", 0, &g), ERR_INVALID_ARGS);
    CHECK_ST(file_open("/boot/bin", FS_READ, &g), ERR_WRONG_TYPE);   /* a directory */
    CHECK_ST(file_open("/", FS_READ, &g), ERR_WRONG_TYPE);
    CHECK_ST(fs_mkdir("/boot/d"), ERR_ACCESS_DENIED);
    CHECK_ST(fs_unlink("/boot/init.cfg"), ERR_ACCESS_DENIED);
    CHECK_ST(fs_rename("/boot/init.cfg", "/boot/x"), ERR_ACCESS_DENIED);
    CHECK_ST(fs_sync("/boot"), OK);
    uint64_t total = 0, free_bytes = 1;
    bool ro = false;
    char label[17];
    const struct bootfs_view *img;
    CHECK_ST(bootfs_default(&img), OK);
    CHECK_ST(fs_statfs("/boot/bin", &total, &free_bytes, &ro, label), OK);
    CHECK(total == img->size && free_bytes == 0 && ro && !strcmp(label, "BOOTFS"));
    return true;
}

bool t_ns_path_rules(void)
{
    if (!have_boot())
        return true;
    /* Absolute, and at most FS_PATH_MAX - 1 bytes. */
    CHECK_ST(fs_stat("boot/init.cfg", NULL, NULL, NULL), ERR_INVALID_ARGS);
    CHECK_ST(fs_stat("", NULL, NULL, NULL), ERR_INVALID_ARGS);
    char path[FS_PATH_MAX + 8];
    memset(path, 'a', sizeof(path));
    memcpy(path, "/boot/", 6);
    path[FS_PATH_MAX] = '\0';
    CHECK_ST(fs_stat(path, NULL, NULL, NULL), ERR_INVALID_ARGS);   /* FS_PATH_MAX bytes */
    path[FS_PATH_MAX - 1] = '\0';
    CHECK_ST(fs_stat(path, NULL, NULL, NULL), ERR_NOT_FOUND);      /* one less: fine */
    struct jfile f;
    path[FS_PATH_MAX - 1] = 'a';
    CHECK_ST(file_open(path, FS_READ, &f), ERR_INVALID_ARGS);
    /* Under no mount. */
    CHECK_ST(fs_stat("/nomount/x", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(fs_stat("/boo", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(fs_stat("/boot2/init.cfg", NULL, NULL, NULL), ERR_NOT_FOUND);
    /* "." and ".." inside a mount, and extra slashes. */
    CHECK_ST(fs_stat("//boot//bin/./utest", NULL, NULL, NULL), OK);
    CHECK_ST(fs_stat("/boot/bin/../init.cfg", NULL, NULL, NULL), OK);
    CHECK_ST(fs_stat("/boot/bin/../bin/../drv/../init.cfg/", NULL, NULL, NULL), OK);
    /* ".." never leaves a mount: at its root it stays there ... */
    CHECK_ST(fs_stat("/boot/../init.cfg", NULL, NULL, NULL), OK);
    CHECK_ST(fs_stat("/boot/../../../bin/utest", NULL, NULL, NULL), OK);
    CHECK_ST(fs_stat("/boot/bin/../../..", NULL, NULL, NULL), OK);
    /* ... so this is /boot/boot/init.cfg, not another way to /boot. */
    CHECK_ST(fs_stat("/boot/../boot/init.cfg", NULL, NULL, NULL), ERR_NOT_FOUND);
    /* ".." and "." before the mount's name stay at "/". */
    CHECK_ST(fs_stat("/../boot/./init.cfg", NULL, NULL, NULL), OK);
    bool dir = false;
    CHECK_ST(fs_stat("/..", NULL, &dir, NULL), OK);
    CHECK(dir);
    /* What "/" itself refuses. */
    CHECK_ST(fs_mkdir("/"), ERR_ALREADY_EXISTS);
    CHECK_ST(fs_unlink("/"), ERR_ACCESS_DENIED);
    CHECK_ST(fs_sync("/"), ERR_INVALID_ARGS);
    /* fs_path_clean, as the services use it. */
    uint8_t in[FS_PATH_MAX];
    char out[FS_PATH_MAX];
    memset(in, 0, sizeof(in));
    memcpy(in, "/a/./b//../c/", 13);
    CHECK_ST(fs_path_clean(in, out), OK);
    CHECK(!strcmp(out, "a/c"));
    memcpy(in, "../../x\0", 8);
    CHECK_ST(fs_path_clean(in, out), OK);
    CHECK(!strcmp(out, "x"));
    memcpy(in, "/\0", 2);
    CHECK_ST(fs_path_clean(in, out), OK);
    CHECK(!out[0]);
    memset(in, 'a', sizeof(in));   /* no NUL in the field */
    CHECK_ST(fs_path_clean(in, out), ERR_INVALID_ARGS);
    return true;
}

/* Mount points are "/name". */
bool t_ns_mount_point_names(void)
{
    handle_t a, b;
    static const char *const bad[] = { "t", "/", "/a/b", "/.", "/..", "/0123456789abcdef", "" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_ST(jam_channel_create(&a, &b), OK);
        CHECK_ST(ns_mount(bad[i], a), ERR_INVALID_ARGS);
        CHECK_ST(jam_handle_close(a), ERR_BAD_HANDLE);   /* consumed all the same */
        CHECK_ST(jam_handle_close(b), OK);
    }
    CHECK_ST(ns_unmount("/nosuch"), ERR_NOT_FOUND);
    return true;
}

/* ---- a read-write mount: the RAM filesystem ---------------------------------------- */

/* Directories, create, write, read back, append, truncate. */
static bool ram_files(void)
{
    CHECK_ST(fs_mkdir(RAM "/docs"), OK);
    CHECK_ST(fs_mkdir(RAM "/docs"), ERR_ALREADY_EXISTS);
    CHECK_ST(fs_mkdir(RAM "/no/such/parent"), ERR_NOT_FOUND);
    struct jfile f;
    CHECK_ST(file_open(RAM "/docs/big.bin", FS_READ, &f), ERR_NOT_FOUND);
    CHECK_ST(file_open(RAM "/docs/big.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    CHECK_EQ(f.size, 0);
    for (unsigned i = 0; i < BIG; i++)
        big[i] = (uint8_t)(i * 31 + (i >> 8));
    size_t done = 0;
    CHECK_ST(file_write(&f, 0, big, BIG, &done), OK);   /* three buffers' worth */
    CHECK_EQ(done, BIG);
    uint64_t size = 0;
    CHECK_ST(file_stat(&f, &size, NULL), OK);
    CHECK_EQ(size, BIG);
    memset(back, 0, BIG);
    CHECK_ST(file_read(&f, 0, back, BIG + 100, &done), OK);
    CHECK_EQ(done, BIG);
    CHECK(!memcmp(big, back, BIG));
    CHECK_ST(file_read(&f, 70000, back, 1000, &done), OK);   /* across a buffer boundary */
    CHECK(done == 1000 && !memcmp(big + 70000, back, 1000));
    CHECK_ST(file_truncate(&f, 10), OK);
    CHECK_ST(file_read(&f, 0, back, 100, &done), OK);
    CHECK_EQ(done, 10);
    CHECK_ST(file_sync(&f), OK);
    file_close(&f);
    CHECK_ST(fs_stat(RAM "/docs/big.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, 10);

    CHECK_ST(ns_put(RAM "/docs/a.txt", "one\n"), OK);
    CHECK_ST(file_open(RAM "/docs/a.txt", FS_WRITE | FS_APPEND, &f), OK);
    CHECK_ST(file_write(&f, 0, "two\n", 4, &done), OK);   /* the offset is ignored */
    file_close(&f);
    CHECK(ns_holds(RAM "/docs/a.txt", "one\ntwo\n"));
    CHECK_ST(file_open(RAM "/docs/a.txt", FS_READ, &f), OK);
    CHECK_ST(file_write(&f, 0, "x", 1, &done), ERR_ACCESS_DENIED);   /* not opened FS_WRITE */
    file_close(&f);
    CHECK_ST(ns_put(RAM "/docs/a.txt", "new\n"), OK);        /* FS_TRUNCATE */
    CHECK(ns_holds(RAM "/docs/a.txt", "new\n"));
    CHECK_ST(file_open(RAM "/docs", FS_READ, &f), ERR_WRONG_TYPE);
    return true;
}

/* readdir, rename, unlink, statfs, a full disk, too many open files. */
static bool ram_tree(void)
{
    struct fs_entry e;
    CHECK_ST(fs_readdir(RAM, 0, &e), OK);
    CHECK(!strcmp(e.name, "docs") && e.is_dir);
    CHECK_ST(fs_readdir(RAM, 1, &e), ERR_NOT_FOUND);
    unsigned n = 0;
    while (fs_readdir(RAM "/docs", n, &e) == OK)
        n++;
    CHECK_EQ(n, 2);
    CHECK_ST(fs_rename(RAM "/docs/a.txt", RAM "/b.txt"), OK);
    CHECK_ST(fs_stat(RAM "/docs/a.txt", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK(ns_holds(RAM "/b.txt", "new\n"));
    CHECK_ST(fs_rename(RAM "/b.txt", RAM "/docs/big.bin"), ERR_ALREADY_EXISTS);
    CHECK_ST(fs_rename(RAM "/nosuch", RAM "/x"), ERR_NOT_FOUND);
    CHECK_ST(fs_rename(RAM "/b.txt", "/boot/b.txt"), ERR_NOT_SUPPORTED);   /* two mounts */
    CHECK_ST(fs_unlink(RAM "/docs"), ERR_BAD_STATE);                       /* not empty */
    CHECK_ST(fs_unlink(RAM "/nosuch"), ERR_NOT_FOUND);
    /* ".." can't reach above the mount from inside it either. */
    CHECK_ST(fs_stat(RAM "/docs/../../b.txt", NULL, NULL, NULL), OK);

    uint64_t total = 0, free0 = 0, free1 = 0;
    bool ro = true;
    char label[17];
    CHECK_ST(fs_statfs(RAM, &total, &free0, &ro, label), OK);
    CHECK(total > 0 && free0 < total && !ro && !strcmp(label, "RAMFS"));
    /* Fill it: the write that doesn't fit is ERR_NO_SPACE, nothing hangs. */
    struct jfile f;
    size_t done = 0;
    status_t st = OK;
    CHECK_ST(file_open(RAM "/fill", FS_WRITE | FS_CREATE, &f), OK);
    for (uint64_t off = 0; st == OK && off < 2 * total; off += BIG)
        st = file_write(&f, off, big, BIG, &done);
    CHECK_ST(st, ERR_NO_SPACE);
    file_close(&f);
    CHECK_ST(fs_statfs(RAM, NULL, &free1, NULL, NULL), OK);
    CHECK(free1 < free0);
    CHECK_ST(fs_unlink(RAM "/fill"), OK);
    CHECK_ST(fs_statfs(RAM, NULL, &free1, NULL, NULL), OK);
    CHECK_EQ(free1, free0);

    /* The service's bound on open files; closing gives the slots back. */
    static struct jfile many[80];
    unsigned opened = 0;
    while (opened < 80 && (st = file_open(RAM "/b.txt", FS_READ, &many[opened])) == OK)
        opened++;
    CHECK_ST(st, ERR_NO_RESOURCES);
    CHECK(opened >= 16 && opened < 80);
    CHECK_ST(fs_unlink(RAM "/b.txt"), ERR_BAD_STATE);   /* open */
    for (unsigned i = 0; i < opened; i++)
        file_close(&many[i]);
    CHECK_ST(file_open(RAM "/b.txt", FS_READ, &many[0]), OK);
    file_close(&many[0]);
    CHECK_ST(fs_unlink(RAM "/b.txt"), OK);
    CHECK_ST(fs_unlink(RAM "/docs/big.bin"), OK);
    CHECK_ST(fs_unlink(RAM "/docs"), OK);
    CHECK_ST(fs_readdir(RAM, 0, &e), ERR_NOT_FOUND);
    return true;
}

bool t_ns_read_write(void)
{
    struct ram r;
    if (!have_boot() || !ram_start(RAM, &r))
        return !have_boot();
    bool ok = ram_files() && ram_tree();
    /* A whole file into a VMO (what spawn does with a path). */
    handle_t vmo;
    uint64_t size = 0;
    char buf[8] = "";
    if (ok) {
        CHECK_ST(ns_put(RAM "/v", "vmo!"), OK);
        CHECK_ST(file_read_vmo(RAM "/v", 3, &vmo, &size), ERR_OUT_OF_RANGE);
        CHECK_ST(file_read_vmo(RAM "/v", 4, &vmo, &size), OK);
        CHECK_EQ(size, 4);
        CHECK_ST(jam_vmo_read(vmo, 0, buf, 4), OK);
        CHECK(!memcmp(buf, "vmo!", 4));
        CHECK_ST(jam_handle_close(vmo), OK);
    }
    return ram_stop(RAM, &r) && ok;
}

/* A mount whose service dies answers ERR_PEER_CLOSED, at once. */
bool t_ns_server_dies(void)
{
    struct ram r;
    if (!have_boot() || !ram_start(RAM, &r))
        return !have_boot();
    struct jfile f;
    size_t done = 0;
    char buf[8];
    CHECK_ST(ns_put(RAM "/x", "data"), OK);
    CHECK_ST(file_open(RAM "/x", FS_READ | FS_WRITE, &f), OK);
    CHECK_ST(jam_job_kill(r.job), OK);
    uint64_t t0 = now();
    CHECK_ST(fs_stat(RAM "/x", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(fs_mkdir(RAM "/d"), ERR_PEER_CLOSED);
    CHECK_ST(fs_sync(RAM), ERR_PEER_CLOSED);
    struct fs_entry e;
    CHECK_ST(fs_readdir(RAM, 0, &e), ERR_PEER_CLOSED);
    struct jfile g;
    CHECK_ST(file_open(RAM "/x", FS_READ, &g), ERR_PEER_CLOSED);
    CHECK_ST(file_read(&f, 0, buf, 4, &done), ERR_PEER_CLOSED);
    CHECK_ST(file_write(&f, 0, "y", 1, &done), ERR_PEER_CLOSED);
    CHECK_ST(file_sync(&f), ERR_PEER_CLOSED);
    CHECK(now() - t0 < 2 * NS_PER_S);   /* no call waited for a timeout */
    file_close(&f);
    /* The dead mount is still listed until it is unmounted or replaced. */
    bool dir = false;
    CHECK_ST(fs_stat("/boot", NULL, &dir, NULL), OK);   /* the others are untouched */
    CHECK_ST(ns_unmount(RAM), OK);
    CHECK_ST(fs_stat(RAM "/x", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(jam_handle_close(r.proc), OK);
    CHECK_ST(jam_handle_close(r.job), OK);
    return true;
}

/* ---- what a child is given ------------------------------------------------------- */

bool t_ns_child_sees_only_its_mounts(void)
{
    struct ram r;
    if (!have_boot() || !ram_start(RAM, &r))
        return !have_boot();
    CHECK_ST(ns_put(RAM "/hello", NS_HELLO), OK);
    handle_t proc;
    struct spawn_handle none = { 0, HANDLE_INVALID };
    /* Only /t: no /boot for it, though we have one. */
    static const char *const only[] = { RAM, NULL };
    CHECK_ST(ns_child_start("ns-only", RAM, only, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* Everything we have. */
    CHECK_ST(ns_child_start("ns-all", RAM, NS_ALL, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    /* An empty namespace, and none at all. */
    static const char *const nothing[] = { NULL };
    CHECK_ST(ns_child_start("ns-none", NULL, nothing, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    CHECK_ST(ns_child_start("ns-none", NULL, NULL, none, NULL, &proc), OK);
    CHECK(ns_child_exits(proc, 0));
    return ram_stop(RAM, &r);
}

/* A running child gets a new mount and loses one; the file it has open
 * on the one it lost stays open. */
bool t_ns_mounts_reach_a_running_child(void)
{
    struct ram r;
    if (!have_boot() || !ram_start(RAM, &r))
        return !have_boot();
    CHECK_ST(ns_put(RAM "/hello", NS_HELLO), OK);
    handle_t proc, ns, mine, theirs, fs;
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    static const char *const only[] = { RAM, NULL };
    struct spawn_handle x = { SR_USER, theirs };
    CHECK_ST(ns_child_start("ns-late", NULL, only, x, &ns, &proc), OK);
    /* It has /t/hello open now: it says so. */
    signals_t seen;
    CHECK_ST(jam_object_wait_one(mine, SIG_READABLE, now() + 10 * NS_PER_S, &seen), OK);
    CHECK_ST(ns_channel(RAM, &fs), OK);
    CHECK_ST(ns_send_one(ns, "/u", fs), OK);              /* the same filesystem, as /u */
    CHECK_ST(ns_send_one(ns, RAM, HANDLE_INVALID), OK);   /* and /t goes */
    CHECK(ns_child_exits(proc, 0));
    CHECK_ST(jam_handle_close(ns), OK);
    CHECK_ST(jam_handle_close(mine), OK);
    return ram_stop(RAM, &r);
}

/* Messages that are no ns_msg are dropped (handles closed); a good one
 * after them still mounts. */
bool t_ns_malformed_messages(void)
{
    struct ram r;
    if (!have_boot() || !ram_start(RAM, &r))
        return !have_boot();
    CHECK_ST(ns_put(RAM "/hello", NS_HELLO), OK);
    handle_t proc, to, theirs, fs, junk_a, junk_b;
    CHECK_ST(jam_channel_create(&to, &theirs), OK);
    /* A namespace channel made by hand, its end read-only as spawn's is. */
    CHECK_ST(jam_handle_replace(theirs, RIGHT_READ | RIGHT_WAIT | RIGHT_TRANSFER, &theirs), OK);
    struct spawn_handle x = { SR_NS, theirs };
    CHECK_ST(ns_child_start("ns-only", RAM, NULL, x, NULL, &proc), OK);
    struct ns_msg m;
    memset(&m, 0, sizeof(m));
    CHECK_ST(jam_channel_write(to, &m, 3, NULL, 0), OK);              /* too short */
    m.kind = NS_MOUNT;
    m.count = NS_MAX_MOUNTS + 1;
    CHECK_ST(jam_channel_write(to, &m, sizeof(m), NULL, 0), OK);      /* too many */
    m.count = 1;
    memcpy(m.path[0], "/boot", 6);
    CHECK_ST(jam_channel_write(to, &m, NS_MSG_SIZE(1), NULL, 0), OK); /* no handle for it */
    CHECK_ST(jam_channel_create(&junk_a, &junk_b), OK);
    CHECK_ST(jam_channel_write(to, &m, NS_MSG_SIZE(1) + 1, &junk_a, 1), OK);   /* wrong size */
    m.kind = 77;
    CHECK_ST(jam_channel_write(to, &m, NS_MSG_SIZE(1), NULL, 0), OK); /* no such kind */
    m.kind = NS_MOUNT;
    m.reserved = 1;
    CHECK_ST(ns_channel(RAM, &fs), OK);
    CHECK_ST(jam_channel_write(to, &m, NS_MSG_SIZE(1), &fs, 1), OK);  /* reserved not 0 */
    m.reserved = 0;
    memcpy(m.path[0], "boot", 5);
    CHECK_ST(ns_channel(RAM, &fs), OK);
    CHECK_ST(jam_channel_write(to, &m, NS_MSG_SIZE(1), &fs, 1), OK);  /* not "/name" */
    static uint8_t huge[8192];
    CHECK_ST(jam_channel_write(to, huge, sizeof(huge), NULL, 0), OK); /* bigger than any */
    /* The junk channel's end was closed by the child when it dropped the
     * message: its peer sees that. */
    signals_t seen;
    CHECK_ST(jam_object_wait_one(junk_b, SIG_PEER_CLOSED, now() + 10 * NS_PER_S, &seen), OK);
    CHECK_ST(jam_handle_close(junk_b), OK);
    /* Now the real one: only /t, as ns-only checks. */
    CHECK_ST(ns_channel(RAM, &fs), OK);
    CHECK_ST(ns_send_one(to, RAM, fs), OK);
    CHECK(ns_child_exits(proc, 0));
    CHECK_ST(jam_handle_close(to), OK);
    return ram_stop(RAM, &r);
}

/* ---- spawn from a VMO ---------------------------------------------------------------- */

bool t_spawn_from_vmo(void)
{
    if (!have_boot())
        return true;
    const struct bootfs_view *img;
    const void *data;
    uint64_t size;
    CHECK_ST(bootfs_default(&img), OK);
    CHECK_ST(bootfs_lookup(img, "bin/utest", &data, &size), OK);
    uint64_t off = (uint64_t)((const uint8_t *)data - img->base);
    handle_t job, proc, vmo;
    CHECK_ST(new_job(&job), OK);
    const char *argv[] = { "utest", "exit7" };
    struct spawn_args a = {
        .path = "utest-vmo", .vmo = startup_handle(SR_BOOTFS), .offset = off, .size = size,
        .argc = 2, .argv = argv, .job = job,
    };
    /* A range of the boot image: runs. */
    CHECK_ST(spawn(&a, &proc), OK);
    CHECK(ns_child_exits(proc, 7));
    /* A range that isn't a program, and one not on a page boundary. */
    a.offset = 0;
    a.size = PAGE_SIZE;
    CHECK_ST(spawn(&a, &proc), ERR_INVALID_ARGS);
    a.offset = off + 8;
    a.size = size - 8;
    CHECK_ST(spawn(&a, &proc), ERR_INVALID_ARGS);
    /* The same program copied into a VMO of ours, and read through the
     * namespace by its path: loaded the same way, but the kernel gives no
     * VMO a process makes RIGHT_EXEC, so its code is refused. */
    CHECK_ST(file_read_vmo("/boot/bin/utest", 64 << 20, &vmo, &size), OK);
    a.vmo = vmo;
    a.offset = 0;
    a.size = size;
    CHECK_ST(spawn(&a, &proc), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(vmo), OK);
    a.vmo = HANDLE_INVALID;
    a.path = "/boot/bin/utest";
    CHECK_ST(spawn(&a, &proc), ERR_ACCESS_DENIED);
    a.path = "/boot/bin/nosuch";
    CHECK_ST(spawn(&a, &proc), ERR_NOT_FOUND);
    /* Nothing is left of the failed starts. */
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}
