/* utest: init's sealer, /svc/exec (init's exec.c; abi/idl/exec.idl). The
 * boot image's exectest, execwide, pxtest, posixtest and wantsession are copied to
 * /data and approved here as `allow` approves (a line in /data/etc/allow,
 * a page table in /data/etc/allow.d: <dataprog.h>); exec_cleanup puts
 * /data back. Skipped without /svc/exec or /data.
 *
 * t_exec_sealed_runs: an approved program is sealed and runs, with exactly
 * its own approval: exectest (narrow) and execwide (wide) started from
 * utest, which holds far more (the whole of /data, devmgr's and volumes'
 * channels, the root with klog): each gets only what its list says (no
 * devmgr, no volumes, /data read-only for exectest, a root with sysinfo
 * alone and never RIGHT_TRANSFER for execwide). The process comes back
 * without RIGHT_WRITE (no threads in it: process_kill refused), the job
 * with RIGHT_MANAGE (job_kill ends a child that waits for ever).
 *
 * t_exec_wider_than_parent: exectest (narrow: /data read-only, no root)
 * starts execwide through /svc/exec itself: the child has /data writable,
 * /boot and the root with sysinfo, none of which its parent has (the
 * owner's rule: a child gets its own approval, independent of its parent).
 *
 * t_exec_refused: a program without a line; an approved program whose
 * list asks for what no /data program may have (wantsession: `svc
 * session`); a file handle of another file than the path names; a channel
 * that isn't a file and never answers (refused at its deadline); an event
 * for a file; unclean paths and one outside /data; nothing at the path;
 * start's malformed strings, argc 0, an event for a job, a job without
 * RIGHT_WRITE; one start per program (the channel is closed after it).
 *
 * t_exec_changed: a file changed after its approval: one byte of its ELF
 * header's page (refused at open: its list's page doesn't match) and one
 * byte of pxtest's table, which only a touch fetches (open and start go
 * through; the child dies at that page, killed by the kernel). And while a
 * program is sealed, fat won't open its file for writing (ERR_BUSY).
 *
 * t_exec_handover: what a POSIX child keeps of its caller's hand-over
 * (docs/CR-REVIEW-POSIX.md, finding 1 and Q1): posixtest (copied to /data
 * and approved too) given a file said to be /data/etc/allow, the
 * directory /data/etc and duplicates of both, /data/etc as its current
 * directory and a shell as its program: its file is the real one, usable
 * and not given on, the directory and its duplicate gone, its current
 * directory "/", /proc/self/exe the sealed path, AT_SECURE (no TMPDIR, no
 * secure_getenv), the umask and the pipe kept (posixtest's `--child-launch
 * sealed` checks). And with no hand-over of ours, it is still AT_SECURE,
 * 0 to 2 on /dev/null. utest's job holds as many handles after.
 *
 * t_exec_no_leak: 40 programs sealed and dropped and 12 started and
 * waited for on one opener (past PROGRAMS_PER_OPENER and PROGRAMS_MAX:
 * init frees each), with utest's job holding as many handles after as
 * before; then fat lets the files be written again (no sealed VMO left in
 * init or a dead child). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <dataprog.h>
#include <idl/exec.h>
#include <os.h>
#include <posixfds.h>
#include <sha256.h>
#include "disks.h"
#include "exectest.h"
#include "pfdtest.h"
#include "utest.h"

#define NARROW   "/data/ce-narrow"
#define WIDE     "/data/ce-wide"
#define PX       "/data/ce-px"
#define PT       "/data/ce-pt"            /* posixtest: what a POSIX child is handed */
#define SESS     "/data/ce-sess"
#define UNALLOW  "/data/ce-unallowed"
#define PX_CUE   "/data/ce-cue-never"     /* pxtest waits for it: never made */
#define PX_MARK  "PXTEST-TABLE-0123456789"
#define WAIT     (60 * NS_PER_S)
#define RELEASE  (3 * NS_PER_S)           /* fat hears a sealed VMO is gone */

static bool    set_up, skipped;
static bool    data_ours;                 /* /data mounted here (from volumes' watch) */
static struct view data_view;             /* ... that watch */
static char   *allow_before;             /* /data/etc/allow as it was (NULL: none) */
static size_t  allow_before_len;
static char    allow_text[4096];          /* the lines added */
static char    tables[8][DP_HEX_LEN + 1]; /* the tables written */
static unsigned ntables;

/* ---- the rig ---------------------------------------------------------------------- */

static status_t write_file(const char *path, const void *data, size_t size)
{
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    st = size ? file_write(&f, 0, data, size, &done) : OK;
    if (st == OK && done != size)
        st = ERR_IO;
    file_close(&f);
    return st;
}

/* A boot image program's bytes (the stripped copy bootfs holds). */
static bool boot_bytes(const char *name, const uint8_t **data, uint64_t *size)
{
    const struct bootfs_view *fs;
    const void *d;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, name, &d, size) != OK)
        return false;
    *data = d;
    return true;
}

/* path approved for bytes (size of them), as `allow` writes it: its page
 * table now, its line into allow_text (written by setup). */
static bool approve(const char *path, const uint8_t *bytes, uint64_t size)
{
    uint8_t sha[SHA256_BYTES];
    char hex[DP_HEX_LEN + 1];
    sha256(bytes, size, sha);
    sha256_hex(sha, hex);
    uint64_t pages = dp_ptable_pages(size);
    size_t n = sizeof(struct dp_ptable_head) + pages * SHA256_BYTES;
    uint8_t *t = malloc(n);
    CHECK(t);
    dp_ptable_head(size, sha, (struct dp_ptable_head *)t);
    dp_ptable_hash(bytes, size, t + sizeof(struct dp_ptable_head));
    char tp[sizeof(DP_TABLE_DIR) + DP_HEX_LEN + 2];
    snprintf(tp, sizeof(tp), DP_TABLE_DIR "/%s", hex);
    status_t st = write_file(tp, t, n);
    free(t);
    CHECK_ST(st, OK);
    CHECK(ntables < sizeof(tables) / sizeof(tables[0]));
    memcpy(tables[ntables++], hex, sizeof(hex));
    size_t have = strlen(allow_text);
    snprintf(allow_text + have, sizeof(allow_text) - have, "%s\t%s\tutest\n", hex, path);
    return true;
}

/* A boot program copied to path, and approved unless !allow. */
static bool place(const char *name, const char *path, bool allow)
{
    const uint8_t *b;
    uint64_t n;
    CHECK(boot_bytes(name, &b, &n));
    CHECK_ST(write_file(path, b, n), OK);
    return !allow || approve(path, b, n);
}

/* Everything the tests need, once; true with skipped set if they can't run. */
static bool setup(void)
{
    if (set_up)
        return true;
    handle_t ch;
    status_t svc = svc_open(SVC_EXEC, &ch), data = fs_stat("/data", NULL, NULL, NULL);
    if (svc == OK)
        jam_handle_close(ch);
    /* The init run starts us before volumes has mounted /data, and our
     * namespace is init's as it was then: the stick's /data from volumes'
     * watch instead (init's /svc/exec reads its own /data: the same one). */
    handle_t vm = HANDLE_INVALID, fs;
    if (svc == OK && data != OK && svc_open(SVC_VOLUMES, &vm) == OK) {
        uint64_t end = now() + WAIT_MOUNT;
        while (!mounted(&data_view, "/data") && view_next(vm, &data_view, end) == OK)
            ;
        if (mounted(&data_view, "/data") &&
            jam_handle_duplicate(mount_of(&data_view, "/data").fs, RIGHT_SAME, &fs) == OK &&
            ns_mount("/data", fs) == OK) {
            data_ours = true;
            data = fs_stat("/data", NULL, NULL, NULL);
        }
        jam_handle_close(vm);
    }
    if (svc != OK || data != OK) {
        printf("utest: %s: /svc/exec %s, /data %s: skipped\n", utest_cur, status_str(svc),
               status_str(data));
        set_up = skipped = true;
        return true;
    }
    if (fs_stat(DP_ALLOW_FILE, NULL, NULL, NULL) == OK) {
        handle_t v;
        uint64_t got;
        CHECK_ST(file_read_vmo(DP_ALLOW_FILE, 1 << 20, &v, &got), OK);
        allow_before = malloc(got + 1);
        CHECK(allow_before);
        CHECK_ST(jam_vmo_read(v, 0, allow_before, got), OK);
        jam_handle_close(v);
        allow_before[got] = '\0';
        allow_before_len = got;
    }
    status_t st = fs_mkdir(DP_ALLOW_DIR);
    CHECK(st == OK || st == ERR_ALREADY_EXISTS);
    st = fs_mkdir(DP_TABLE_DIR);
    CHECK(st == OK || st == ERR_ALREADY_EXISTS);
    allow_text[0] = '\0';
    set_up = true;   /* from here on exec_cleanup undoes it */
    CHECK(place("bin/exectest", NARROW, true));
    CHECK(place("bin/execwide", WIDE, true));
    CHECK(place("bin/pxtest", PX, true));
    CHECK(place("bin/posixtest", PT, true));
    CHECK(place("bin/wantsession", SESS, true));
    CHECK(place("bin/exectest", UNALLOW, false));
    /* The old approvals, then ours (a path's last line is its approval). */
    size_t n = allow_before_len + strlen(allow_text);
    char *all = malloc(n + 1);
    CHECK(all);
    if (allow_before_len)
        memcpy(all, allow_before, allow_before_len);
    memcpy(all + allow_before_len, allow_text, strlen(allow_text) + 1);
    st = write_file(DP_ALLOW_FILE, all, n);
    free(all);
    CHECK_ST(st, OK);
    return true;
}

/* Start path with argv through /svc/exec and wait: its exit code, or -1
 * if it was killed. */
static bool ex_run(const char *path, int argc, const char *const *argv, int64_t *code)
{
    handle_t proc, job, sig;
    int step;
    status_t st = exectest_start(path, argc, argv, &proc, &job, &sig, &step);
    if (st != OK)
        FAIL("%s: step %d: %s", path, step, status_str(st));
    struct process_info info;
    st = spawn_wait(proc, WAIT, &info);
    if (st != OK)
        jam_job_kill(job);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(sig);
    CHECK_ST(st, OK);
    *code = info.killed ? -1 : info.exit_code;
    return true;
}

/* exec.open of path with file (consumed) and a fresh hand-over end; the
 * program channel into *prog when it is OK. */
static status_t open_with(const char *path, handle_t file, handle_t *prog)
{
    handle_t svc, mine, theirs;
    uint8_t posix;
    char wants[1024];
    uint32_t wl;
    status_t st = svc_open(SVC_EXEC, &svc);
    if (st != OK) {
        jam_handle_close(file);
        return st;
    }
    st = jam_channel_create(&mine, &theirs);
    if (st != OK) {
        jam_handle_close(file);
        jam_handle_close(svc);
        return st;
    }
    st = exec_open_within(svc, WAIT, path, (uint32_t)strlen(path), file, theirs, prog, &posix,
                          wants, &wl);
    jam_handle_close(mine);
    jam_handle_close(svc);
    return st;
}

/* A file channel of path (ours to give). */
static bool file_of(const char *path, handle_t *ch)
{
    struct jfile f;
    handle_t buf;
    CHECK_ST(file_open(path, FS_READ, &f), OK);
    file_give(&f, ch, &buf);
    jam_handle_close(buf);
    return true;
}

static status_t open_path(const char *path, handle_t *prog)
{
    handle_t ch;
    struct jfile f;
    handle_t buf;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    file_give(&f, &ch, &buf);
    jam_handle_close(buf);
    return open_with(path, ch, prog);
}

/* Write a file whose sealed VMO may only just have gone: fat hears of it
 * shortly. */
static status_t write_soon(const char *path, const void *data, size_t size)
{
    uint64_t end = now() + RELEASE;
    status_t st;
    while ((st = write_file(path, data, size)) == ERR_BUSY && now() < end)
        jam_sleep_until(now() + 20 * NS_PER_MS);
    return st;
}

/* ---- the tests -------------------------------------------------------------------- */

bool t_exec_sealed_runs(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    int64_t code;
    const char *probe[] = { "exectest", "probe" };
    CHECK(ex_run(NARROW, 2, probe, &code));
    CHECK_EQ(code, EXT_NARROW);
    CHECK(ex_run(WIDE, 2, probe, &code));
    CHECK_EQ(code, EXT_WIDE);
    /* What the caller holds of a child: no RIGHT_WRITE on the process, the
     * job's kill. pxtest waits for a cue that never comes. */
    const char *wait_cue[] = { "pxtest", "touch", PX_CUE };
    handle_t proc, job, sig;
    int step;
    CHECK_ST(exectest_start(PX, 3, wait_cue, &proc, &job, &sig, &step), OK);
    struct handle_info hi;
    CHECK_ST(jam_handle_info(proc, &hi), OK);
    CHECK(!(hi.rights & RIGHT_WRITE));
    CHECK_ST(jam_process_kill(proc), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_info(job, &hi), OK);
    CHECK((hi.rights & RIGHT_MANAGE) && !(hi.rights & RIGHT_WRITE));
    CHECK_ST(jam_job_kill(job), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, WAIT, &info), OK);
    CHECK(info.killed);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(sig);
    return true;
}

bool t_exec_wider_than_parent(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    int64_t code;
    const char *wide[] = { "exectest", "launch", WIDE, "probe" };
    CHECK(ex_run(NARROW, 4, wide, &code));
    CHECK_EQ(code, EXT_WIDE);
    const char *narrow[] = { "execwide", "launch", NARROW, "probe" };
    CHECK(ex_run(WIDE, 4, narrow, &code));
    CHECK_EQ(code, EXT_NARROW);
    return true;
}

bool t_exec_refused(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    handle_t prog, ch, ev, a, b;
    CHECK_ST(open_path(UNALLOW, &prog), ERR_ACCESS_DENIED);   /* no line for it */
    CHECK_ST(open_path(SESS, &prog), ERR_ACCESS_DENIED);      /* `svc session` */
    CHECK(file_of(WIDE, &ch));
    CHECK_ST(open_with(NARROW, ch, &prog), ERR_ACCESS_DENIED);   /* another file */
    CHECK(file_of(NARROW, &ch));
    CHECK_ST(open_with("/data/ce-none", ch, &prog), ERR_NOT_FOUND);
    CHECK(file_of(NARROW, &ch));
    CHECK_ST(open_with("/data/./ce-narrow", ch, &prog), ERR_INVALID_ARGS);
    CHECK(file_of(NARROW, &ch));
    CHECK_ST(open_with("/data//ce-narrow", ch, &prog), ERR_INVALID_ARGS);
    CHECK(file_of(NARROW, &ch));
    CHECK_ST(open_with("/boot/bin/exectest", ch, &prog), ERR_INVALID_ARGS);
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(open_with(NARROW, ev, &prog), ERR_INVALID_ARGS);   /* not a channel */
    /* A channel nobody answers: init gives up at its deadline. */
    CHECK_ST(jam_channel_create(&a, &b), OK);
    uint64_t t0 = now();
    CHECK_ST(open_with(NARROW, b, &prog), ERR_ACCESS_DENIED);
    CHECK(now() - t0 < 30 * NS_PER_S);
    jam_handle_close(a);

    /* start's refusals, each on a fresh program (one try each). */
    static const uint8_t no_nul[] = { 'x' }, two[] = { 'x', 0, 'y', 0 };
    handle_t job, sig, s2, proc, cjob;
    CHECK_ST(open_path(NARROW, &prog), OK);
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_JOB), RIGHT_SAME, &job), OK);
    CHECK_ST(jam_channel_create(&sig, &s2), OK);
    CHECK_ST(exec_prog_start_within(prog, WAIT, 1, no_nul, 1, job, s2, &proc, &cjob),
             ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_JOB), RIGHT_SAME, &job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    /* The channel was closed after that one try. */
    CHECK_ST(exec_prog_start_within(prog, WAIT, 1, two, 4, job, b, &proc, &cjob), ERR_PEER_CLOSED);
    jam_handle_close(prog);
    jam_handle_close(a);
    jam_handle_close(sig);
    struct { uint16_t argc; bool event_job, no_write; } bad[] = {
        { 0, false, false }, { 3, false, false }, { 1, true, false }, { 1, false, true },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_ST(open_path(NARROW, &prog), OK);
        if (bad[i].event_job)
            CHECK_ST(jam_event_create(&job), OK);
        else
            CHECK_ST(jam_handle_duplicate(startup_handle(SR_JOB), bad[i].no_write ?
                                          RIGHTS_BASIC | RIGHT_READ : RIGHT_SAME, &job), OK);
        CHECK_ST(jam_channel_create(&a, &b), OK);
        CHECK_ST(exec_prog_start_within(prog, WAIT, bad[i].argc, two, sizeof(two), job, b, &proc,
                                        &cjob), ERR_INVALID_ARGS);
        jam_handle_close(prog);
        jam_handle_close(a);
    }
    return true;
}

bool t_exec_changed(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    const uint8_t *orig;
    uint64_t n;
    CHECK(boot_bytes("bin/pxtest", &orig, &n));
    uint8_t *bytes = malloc(n);
    CHECK(bytes);
    /* A byte of the ELF header's page (e_ident's padding): the list's read
     * at open fetches that page, and it isn't the approved one. */
    memcpy(bytes, orig, n);
    bytes[15] ^= 0x5a;
    CHECK_ST(write_soon(PX, bytes, n), OK);
    handle_t prog;
    CHECK_ST(open_path(PX, &prog), ERR_ACCESS_DENIED);
    /* A byte in the middle of pxtest's table: nothing reads it until the
     * program touches it, and then the kernel ends the program. */
    memcpy(bytes, orig, n);
    uint64_t mark = 0;
    while (mark + sizeof(PX_MARK) <= n && memcmp(bytes + mark, PX_MARK, sizeof(PX_MARK) - 1))
        mark++;
    CHECK(mark + 100 * PAGE_SIZE < n);
    bytes[mark + 64 * PAGE_SIZE + 100] ^= 0x5a;
    CHECK_ST(write_soon(PX, bytes, n), OK);
    int64_t code;
    const char *touch[] = { "pxtest", "touch" };
    CHECK(ex_run(PX, 2, touch, &code));
    CHECK_EQ(code, -1);   /* killed: a page doesn't match the approved program */
    /* While sealed, the file can't be opened for writing. */
    CHECK_ST(write_soon(PX, orig, n), OK);
    CHECK_ST(open_path(PX, &prog), OK);
    struct jfile f;
    CHECK_ST(file_open(PX, FS_WRITE, &f), ERR_BUSY);
    jam_handle_close(prog);   /* dropped: init lets it go */
    CHECK_ST(write_soon(PX, orig, n), OK);
    CHECK(ex_run(PX, 2, touch, &code));
    CHECK_EQ(code, 0);   /* the approved bytes again: runs */
    free(bytes);
    return true;
}

bool t_exec_no_leak(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    struct job_info before, after;
    const char *probe[] = { "exectest", "probe" };
    int64_t code;
    handle_t prog;
    CHECK(ex_run(NARROW, 2, probe, &code));   /* libos's lazy handles made once */
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &before), OK);
    for (unsigned i = 0; i < 40; i++) {
        CHECK_ST(open_path(NARROW, &prog), OK);
        jam_handle_close(prog);
    }
    for (unsigned i = 0; i < 12; i++) {
        CHECK(ex_run(i & 1 ? WIDE : NARROW, 2, probe, &code));
        CHECK_EQ(code, i & 1 ? EXT_WIDE : EXT_NARROW);
    }
    CHECK_ST(jam_job_get_info(startup_handle(SR_JOB), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    /* No sealed VMO of either file is left anywhere: fat writes them. */
    const uint8_t *b;
    uint64_t n;
    CHECK(boot_bytes("bin/exectest", &b, &n));
    CHECK_ST(write_soon(NARROW, b, n), OK);
    CHECK(boot_bytes("bin/execwide", &b, &n));
    CHECK_ST(write_soon(WIDE, b, n), OK);
    return true;
}
/* posixtest (on /data as PT) started through /svc/exec as
 * `posixtest --child-launch <mode> PT` with HOME and TMPDIR in its
 * environment, the hand-over ho written on our end first (NULL: none):
 * its exit code into *code (-1: killed). */
static bool pt_run(const char *mode, const struct posixfds_out *ho, int64_t *code)
{
    const char *const strs[] = { "posixtest", "--child-launch", mode, PT,
                                 "HOME=/data", "TMPDIR=/data/etc" };
    enum { ARGC = 4 };
    uint8_t strings[256];
    uint32_t len = 0;
    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        size_t l = strlen(strs[i]) + 1;
        memcpy(strings + len, strs[i], l);
        len += (uint32_t)l;
    }
    handle_t svc, file, mine, theirs, prog, job, sig, sig_theirs, proc, cjob;
    uint8_t posix = 0;
    char wants[1024];
    uint32_t wl;
    CHECK(file_of(PT, &file));
    CHECK_ST(svc_open(SVC_EXEC, &svc), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    status_t st = exec_open_within(svc, WAIT, PT, (uint32_t)strlen(PT), file, theirs, &prog,
                                   &posix, wants, &wl);
    jam_handle_close(svc);
    CHECK_ST(st, OK);
    CHECK(posix);
    if (ho)
        CHECK_ST(posixfds_write(mine, ho), OK);
    jam_handle_close(mine);   /* written: init reads it all at the start */
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_JOB), RIGHT_SAME, &job), OK);
    CHECK_ST(jam_channel_create(&sig, &sig_theirs), OK);
    st = exec_prog_start_within(prog, WAIT, ARGC, strings, len, job, sig_theirs, &proc, &cjob);
    jam_handle_close(prog);
    if (st != OK)
        jam_handle_close(sig);
    CHECK_ST(st, OK);
    struct process_info info;
    st = spawn_wait(proc, WAIT, &info);
    if (st != OK)
        jam_job_kill(cjob);
    jam_handle_close(proc);
    jam_handle_close(cjob);
    jam_handle_close(sig);
    CHECK_ST(st, OK);
    *code = info.killed ? -1 : info.exit_code;
    return true;
}

bool t_exec_handover(void)
{
    CHECK(setup());
    if (skipped)
        return true;
    uint64_t before = pfd_handles();
    /* Names the child's approval reaches and its caller's may not: a file
     * (really NARROW's, an ELF) said to be /data/etc/allow, the directory
     * /data/etc, duplicates of both, the directory as the current one, a
     * shell as the program; and the caller's pipe at 1. */
    static const char lie[] = DP_ALLOW_FILE, dir[] = "/data/etc";
    const uint32_t four = 4, three = 3;
    handle_t pipe_mine, pipe_h[1], file_h[2];
    struct jfile f;
    CHECK_ST(jam_channel_create(&pipe_mine, &pipe_h[0]), OK);
    CHECK_ST(file_open(NARROW, FS_READ, &f), OK);
    file_give(&f, &file_h[0], &file_h[1]);
    const struct posixfds_desc d[] = {
        { .fd = 1, .kind = POSIXFDS_PIPE, .status = 1 /* O_WRONLY */, .handles = pipe_h },
        { .fd = 3, .kind = POSIXFDS_FILE, .data = lie, .data_len = sizeof(lie),
          .handles = file_h },
        { .fd = 4, .kind = POSIXFDS_DIR, .data = dir, .data_len = sizeof(dir) },
        { .fd = 5, .kind = POSIXFDS_DUP, .data = &four, .data_len = sizeof(four) },
        { .fd = 6, .kind = POSIXFDS_DUP, .data = &three, .data_len = sizeof(three) },
    };
    const struct posixfds_out ho = { .state = { .umask = 027 }, .cwd = dir, .fds = d,
                                     .nfds = sizeof(d) / sizeof(d[0]), .exe = "/boot/bin/sh" };
    int64_t code = -2;
    CHECK(pt_run("sealed", &ho, &code));
    jam_handle_close(pipe_mine);
    CHECK_EQ(code, 0);   /* else launch.c's child_sealed says which check */
    /* No hand-over of ours: init writes one for the mark all the same. */
    CHECK(pt_run("sealed-bare", NULL, &code));
    CHECK_EQ(code, 0);
    CHECK_EQ(pfd_handles(), before);
    return true;
}

/* /data as utest found it: ours unmounted, volumes' watch closed. */
static void data_back(void)
{
    if (data_ours)
        (void)ns_unmount("/data");
    data_ours = false;
    view_close(&data_view);
}

bool t_exec_cleanup(void)
{
    if (!set_up || skipped) {
        data_back();
        return true;
    }
    const char *files[] = { NARROW, WIDE, PX, PT, SESS, UNALLOW, EXT_PROBE_FILE };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        (void)fs_unlink(files[i]);
    for (unsigned i = 0; i < ntables; i++) {
        char tp[sizeof(DP_TABLE_DIR) + DP_HEX_LEN + 2];
        snprintf(tp, sizeof(tp), DP_TABLE_DIR "/%s", tables[i]);
        if (!allow_before || !strstr(allow_before, tables[i]))
            (void)fs_unlink(tp);
    }
    status_t st = allow_before ? write_file(DP_ALLOW_FILE, allow_before, allow_before_len)
                               : fs_unlink(DP_ALLOW_FILE);
    free(allow_before);
    allow_before = NULL;
    set_up = false;
    data_back();
    CHECK_ST(st, OK);
    return true;
}
