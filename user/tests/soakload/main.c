/* soakload: user-space load for the shell's `soak` command, running next
 * to the kernel tests until it is told to stop. Four kinds of work, each in
 * a thread of its own, each checking what it did:
 *
 *   files     on every mount of the namespace: a writable one (/data, a
 *             stick made writable with `mount -w`) gets a file written,
 *             synced, read back and compared through the same open file,
 *             read again after a reopen, and deleted; a read-only one
 *             (/boot, /esp, other sticks) has files of its root read twice
 *             and compared
 *   memory    VMOs created, mapped, written page by page, checked,
 *             decommitted (the pages must read zero again), unmapped
 *   calls     channel_call to a second thread that echoes, the reply checked
 *   spawn     bin/utest started in a job of its own in the mode that exits
 *             with 7, its exit code checked
 *
 * A stick may be pulled while this runs. A file call that fails then is
 * not a failure of the system: the cycle is "cut short" and counted as
 * such, with its status in the log. What counts as FAILED is wrong data
 * where every call said OK, and any failure of the other three kinds.
 *
 * Started with a channel (SR_USER): one message on it (or its other end
 * closing) stops the load; the reply is struct soakload_result. With
 * no channel it runs for argv[1] seconds (default 10), for `run soakload`. */
#include <os.h>
#include <soakload.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("mount * rw\n");

#define NTHREADS   5
#define STACK_SIZE (32 * 1024)
#define FILE_MAX   (192 * 1024)   /* the biggest file a cycle writes */
#define RO_MAX     (64 * 1024)    /* bytes of a read-only file read twice */
#define RO_FILES   4              /* files of a read-only root read per pass */
#define SAID_MAX   12             /* lines of each kind before they are only counted */

static bool stop;                 /* set once by main; the workers poll it */
static struct soakload_result res;   /* the workers' atomic adds */

static bool stopped(void)
{
    return __atomic_load_n(&stop, __ATOMIC_ACQUIRE);
}

static void add(uint64_t *counter)
{
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

static uint64_t rnd(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* A check failed: the system did something wrong. */
static void failed(const char *what, const char *where, status_t st)
{
    if (__atomic_add_fetch(&res.failed, 1, __ATOMIC_RELAXED) <= SAID_MAX)
        printf("soakload: FAILED %s (%s, %s)\n", what, where, status_str(st));
}

/* A file call failed: the mount went away under us, or could not answer. */
static void cut_short(const char *what, const char *path, status_t st)
{
    if (__atomic_add_fetch(&res.cut_short, 1, __ATOMIC_RELAXED) <= SAID_MAX)
        printf("soakload: %s of %s cut short: %s\n", what, path, status_str(st));
}

/* ---- files ------------------------------------------------------------------------ */

static uint8_t wbuf[FILE_MAX], rbuf[FILE_MAX];

/* Read n bytes at 0 into rbuf and compare with wbuf. */
static bool read_back(struct jfile *f, const char *path, size_t n, const char *when)
{
    size_t done = 0;
    status_t st = file_read(f, 0, rbuf, n, &done);
    if (st != OK) {
        cut_short(when, path, st);
        return false;
    }
    if (done != n || memcmp(rbuf, wbuf, n)) {
        failed(done != n ? "a file read back short" : "a file read back different", path, OK);
        return false;
    }
    return true;
}

/* One file on a writable mount, start to finish. */
static void write_cycle(const char *mount, uint64_t *seed)
{
    char path[NS_NAME_MAX + 24];
    snprintf(path, sizeof(path), "%s/soak-%u.tmp", mount, (unsigned)(rnd(seed) % 4));
    size_t n = 4096 + (size_t)(rnd(seed) % (FILE_MAX - 4096)), done = 0;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = rnd(seed);
        memcpy(wbuf + i, &v, n - i < 8 ? n - i : 8);
    }
    struct jfile f;
    status_t st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK) {
        cut_short("open", path, st);
        return;
    }
    st = file_write(&f, 0, wbuf, n, &done);
    if (st == OK && done == n)
        st = file_sync(&f);
    bool ok = st == OK && done == n;
    if (!ok)
        cut_short("write", path, st);
    ok = ok && read_back(&f, path, n, "read");
    file_close(&f);
    if (ok && (st = file_open(path, FS_READ, &f)) == OK) {
        ok = read_back(&f, path, n, "second read");
        file_close(&f);
    } else if (ok) {
        cut_short("reopen", path, st);
        ok = false;
    }
    st = fs_unlink(path);
    if (ok && st != OK)
        cut_short("unlink", path, st);
    else if (ok)
        add(&res.file_cycles);
}

/* A read-only mount: the first files of its root, each read twice. */
static void read_pass(const char *mount)
{
    static uint8_t again[RO_MAX];
    unsigned files = 0;
    for (uint32_t i = 0; i < 16 && files < RO_FILES && !stopped(); i++) {
        struct fs_entry e;
        if (fs_readdir(mount, i, &e) != OK)
            break;
        if (e.is_dir || !e.size)
            continue;
        char path[FS_PATH_MAX + NS_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%s", mount, e.name);
        struct jfile f;
        size_t a = 0, b = 0, n = e.size < RO_MAX ? (size_t)e.size : RO_MAX;
        if (file_open(path, FS_READ, &f) != OK)
            continue;   /* logd's own log, open for writing, may refuse */
        status_t s1 = file_read(&f, 0, rbuf, n, &a), s2 = file_read(&f, 0, again, n, &b);
        file_close(&f);
        if (s1 != OK || s2 != OK)
            cut_short("read", path, s1 != OK ? s1 : s2);
        else if (a != b || memcmp(rbuf, again, a))
            failed("two reads of one file differ", path, OK);
        else
            files++;
    }
    if (files)
        add(&res.read_passes);
}

static void files_main(void *arg)
{
    (void)arg;
    uint64_t seed = 0x9e3779b97f4a7c15ull ^ now();
    while (!stopped()) {
        char mount[NS_NAME_MAX];
        bool any = false;
        for (unsigned i = 0; ns_mount_at(i, mount) && !stopped(); i++) {
            bool ro = true;
            if (fs_statfs(mount, NULL, NULL, &ro, NULL) != OK)
                continue;   /* gone: its stick was pulled */
            any = true;
            if (ro)
                read_pass(mount);
            else
                write_cycle(mount, &seed);
        }
        jam_nanosleep(now() + (any ? 20 : 200) * NS_PER_MS);
    }
}

/* ---- memory ----------------------------------------------------------------------- */

static void memory_round(uint64_t *seed)
{
    uint64_t pages = 1 + rnd(seed) % 32, size = pages * PAGE_SIZE, addr = 0;
    uint64_t tag = rnd(seed);
    handle_t vmo, vmar = startup_handle(SR_SELF_VMAR);
    status_t st = jam_vmo_create(size, 0, HANDLE_INVALID, &vmo);
    if (st != OK) {
        failed("vmo_create", "memory", st);
        return;
    }
    st = jam_vmar_map(vmar, vmo, 0, size, VMAR_READ | VMAR_WRITE, &addr);
    if (st != OK) {
        failed("vmar_map", "memory", st);
        jam_handle_close(vmo);
        return;
    }
    volatile uint64_t *p = (volatile uint64_t *)addr;
    bool ok = true;
    for (uint64_t i = 0; i < pages; i++)
        p[i * (PAGE_SIZE / 8)] = tag + i;   /* a page fault each */
    for (uint64_t i = 0; i < pages; i++)
        ok &= p[i * (PAGE_SIZE / 8)] == tag + i;
    if (!ok)
        failed("a page lost what was written to it", "memory", OK);
    st = jam_vmo_decommit(vmo, 0, size);
    for (uint64_t i = 0; st == OK && i < pages; i++)
        if (p[i * (PAGE_SIZE / 8)] != 0) {
            failed("a decommitted page did not read zero", "memory", OK);
            break;
        }
    if (st != OK)
        failed("vmo_decommit", "memory", st);
    if ((st = jam_vmar_unmap(vmar, addr, size)) != OK)
        failed("vmar_unmap", "memory", st);
    jam_handle_close(vmo);
    add(&res.memory_rounds);
}

static void memory_main(void *arg)
{
    (void)arg;
    uint64_t seed = 0x2545f4914f6cdd1dull ^ now();
    while (!stopped()) {
        memory_round(&seed);
        if (rnd(&seed) % 16 == 0)
            jam_nanosleep(now() + NS_PER_MS);
    }
}

/* ---- calls ------------------------------------------------------------------------ */

#define CALL_BYTES 64

static handle_t call_client, call_server;

/* Echo every request until told to stop. */
static void echo_main(void *arg)
{
    (void)arg;
    while (!stopped()) {
        if (jam_object_wait_one(call_server, SIG_READABLE, now() + 100 * NS_PER_MS, NULL) != OK)
            continue;
        uint8_t msg[CALL_BYTES];
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = call_server, .bytes_cap = sizeof(msg), .bytes = (uint64_t)(uintptr_t)msg,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        if (jam_channel_read(&a) == OK)
            jam_channel_write(call_server, msg, n, NULL, 0);
    }
}

static void calls_main(void *arg)
{
    (void)arg;
    uint64_t seed = 0xdeadbeefcafef00dull ^ now();
    while (!stopped()) {
        uint64_t req[CALL_BYTES / 8], rep[CALL_BYTES / 8];
        uint32_t got = 0;
        for (unsigned i = 0; i < CALL_BYTES / 8; i++)
            req[i] = rnd(&seed);
        struct channel_call_args a = {
            .h = call_client, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)req,
            .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)rep,
            .ractual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + 30 * NS_PER_S,
        };
        status_t st = jam_channel_call(&a);
        if (st != OK)
            failed("channel_call", "calls", st);
        else if (got != sizeof(req) || memcmp((uint8_t *)req + 4, (uint8_t *)rep + 4,
                                              sizeof(req) - 4))
            failed("a call's reply is not its request", "calls", OK);   /* past the txid */
        add(&res.calls);
        if (rnd(&seed) % 64 == 0)
            jam_nanosleep(now() + NS_PER_MS);
    }
}

/* ---- spawn ------------------------------------------------------------------------ */

static void spawn_main(void *arg)
{
    (void)arg;
    while (!stopped()) {
        handle_t job, proc;
        status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
        if (st != OK) {
            failed("job_create", "spawn", st);
            return;
        }
        const char *argv[] = { "utest", "exit7" };
        struct spawn_args a = { .path = "bin/utest", .argc = 2, .argv = argv, .job = job };
        struct process_info info;
        st = spawn(&a, &proc);
        if (st != OK) {
            failed("spawn of bin/utest", "spawn", st);
        } else {
            st = spawn_wait(proc, 60 * NS_PER_S, &info);
            if (st != OK || info.killed || info.exit_code != 7)
                failed("bin/utest exit7 did not exit with 7", "spawn", st);
            jam_handle_close(proc);
            add(&res.spawns);
        }
        jam_handle_close(job);
        jam_nanosleep(now() + 25 * NS_PER_MS);
    }
}

/* ---- main ------------------------------------------------------------------------- */

/* Leave no soak-N.tmp behind on any writable mount. */
static void sweep(void)
{
    char mount[NS_NAME_MAX], path[NS_NAME_MAX + 24];
    for (unsigned i = 0; ns_mount_at(i, mount); i++) {
        bool ro = true;
        if (fs_statfs(mount, NULL, NULL, &ro, NULL) != OK || ro)
            continue;
        for (unsigned k = 0; k < 4; k++) {
            snprintf(path, sizeof(path), "%s/soak-%u.tmp", mount, k);
            fs_unlink(path);
        }
    }
}

int main(int argc, char **argv)
{
    static uint8_t stacks[NTHREADS][STACK_SIZE] __attribute__((aligned(64)));
    static void (*const fns[NTHREADS])(void *) = { files_main, memory_main, echo_main,
                                                   calls_main, spawn_main };
    static const char *const names[NTHREADS] = { "files", "memory", "echo", "calls", "spawn" };
    handle_t ctl = startup_handle(SR_USER), th[NTHREADS];
    uint64_t seconds = 10;
    if (argc > 1) {
        seconds = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9' && seconds < 100000; p++)
            seconds = seconds * 10 + (uint64_t)(*p - '0');
    }
    status_t st = jam_channel_create(&call_client, &call_server);
    for (unsigned i = 0; st == OK && i < NTHREADS; i++)
        st = thread_spawn(names[i], fns[i], NULL, stacks[i], STACK_SIZE, &th[i]);
    if (st != OK) {
        printf("soakload: can't start (%s)\n", status_str(st));
        return 2;
    }
    if (ctl)
        jam_object_wait_one(ctl, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, NULL);
    else
        jam_nanosleep(now() + seconds * NS_PER_S);
    __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < NTHREADS; i++)
        if (jam_object_wait_one(th[i], SIG_TERMINATED, now() + 120 * NS_PER_S, NULL) != OK)
            failed("a worker did not stop within 120 s", names[i], ERR_TIMED_OUT);
    sweep();
    printf("soakload: %lu file cycles, %lu read-only passes, %lu cut short; %lu memory rounds, "
           "%lu calls, %lu spawns; %lu FAILED\n", (unsigned long)res.file_cycles,
           (unsigned long)res.read_passes, (unsigned long)res.cut_short,
           (unsigned long)res.memory_rounds, (unsigned long)res.calls,
           (unsigned long)res.spawns, (unsigned long)res.failed);
    if (ctl)
        jam_channel_write(ctl, &res, sizeof(res), NULL, 0);
    return res.failed ? 1 : 0;
}
