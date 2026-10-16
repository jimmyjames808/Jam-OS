/* init's /tmp (docs/M13.6-PLAN.md's U7b; decided after CB2: Chromium,
 * like most ported software, takes a writable /tmp for granted). In shell
 * mode every program init starts, and every shell it follows, has one,
 * emptied at boot, from the first of these that works:
 *   1. a view of /jam/tmp (fs.subview), when the Jam OS drive's ext volume
 *      is mounted read-write at /jam (its other views, /usr, /var and
 *      /home, are jammounts.c's);
 *   2. a view of /data/tmp, when /data is read-write and can give one: an
 *      ext /data (a boot from the Jam OS drive, whose data partition is
 *      /data and no /jam). The stick's FAT /data can't: fat has no views
 *      of directories;
 *   3. an ext volume in memory: bin/extfs on a VMO of init's
 *      (EXTFS_SR_RAM), formatted at its start, so it is empty at every
 *      boot. Its size is an eighth of the memory, between RAM_MIN and
 *      RAM_MAX; a VMO's pages are taken only as they are written, so an
 *      empty /tmp costs a few pages and the service's own memory.
 * A /tmp on a volume is emptied the first time it is used this boot (what
 * the last boot, or a cut, left there goes); one that comes back later
 * (/jam after the installer let go of it) keeps what it has.
 *
 * The choice is made again, on a thread of its own (a /tmp full of files
 * takes a while to empty; init's loop must not wait), whenever /jam or
 * /data is mounted or goes (tmp_mount_changed, from mounts.c) and when the
 * memory volume's service ends (KEY_TMP): /jam and /data are looked at in
 * init's own namespace (fs.statfs: there and read-write), and /tmp is
 * mounted from the best source when that isn't what it is now (ns_mount
 * replaces the old mount). A memory /tmp that a better one replaces is
 * ended (files open in it see their channel close); one that ends by
 * itself is made again, empty, at most RAM_STARTS times a boot. The loop is
 * told (KEY_MOUNTS: tmp_changed) to update the followers, as for
 * jammounts.c's views. The thread is the only one that touches the
 * service's handles. */
#include <extfssvc.h>
#include "init.h"

#define STACK        (96u << 10)   /* fs calls carry 4 KiB path buffers; the tree walk is flat */
#define THREAD_END   (2 * NS_PER_S)    /* a finished thread ends within this */
#define TMP_LIMIT    (30 * NS_PER_S)   /* /tmp left as it is after this much deleting */
#define DEPTH_MAX    16            /* directories deep in /tmp that are emptied */
#define PATH_MAX_LEN 1024          /* a path in /tmp that is emptied */
#define RAM_MIN      (64ull << 20)     /* the memory /tmp's size, at least */
#define RAM_MAX      (1ull << 30)      /* ... at most */
#define RAM_STARTS   8             /* memory /tmps made a boot, at most (a service that dies
                                    * at once doesn't loop) */
#define EXTFS        "bin/extfs"
#define TMP_MOUNT    "/tmp"

enum src { SRC_NONE, SRC_JAM, SRC_DATA, SRC_RAM, SRC_COUNT };

static const struct {
    const char *mount;   /* the volume it is a view of */
    const char *dir;     /* ... and the directory */
} srcs[SRC_COUNT] = {
    [SRC_JAM] = { JAM_MOUNT, JAM_MOUNT "/tmp" },
    [SRC_DATA] = { DATA_MOUNT, DATA_MOUNT "/tmp" },
};

static struct {
    bool     on;             /* tmp_start was called: shell mode */
    handle_t port;           /* the loop's: KEY_MOUNTS wakes it, KEY_TMP the service's end */
    uint64_t key;
    uint32_t gen;            /* bumped by every change the thread must look at (atomic) */
    uint32_t src_gen[SRC_COUNT];   /* bumped when /jam or /data is mounted again (atomic) */
    bool     running;        /* the thread is at work (atomic) */
    handle_t thread;
    bool     changed;        /* /tmp was mounted: the loop tells the followers (atomic) */
    bool     said;           /* the log line below is for the loop to say (atomic) */
    char     line[160];      /* ... written before `said` is set */
    /* The thread's alone. */
    enum src cur;            /* where /tmp is from now (SRC_NONE: no /tmp) */
    uint32_t used_gen;       /* ... the src_gen it was made at */
    bool     emptied[SRC_COUNT];   /* /jam/tmp, /data/tmp emptied this boot */
    handle_t ram_job, ram_proc;    /* the memory /tmp's service (0: none) */
    unsigned ram_starts;
    uint8_t  stack[STACK] __attribute__((aligned(16)));
} tm;

static bool current(uint32_t gen)
{
    return __atomic_load_n(&tm.gen, __ATOMIC_ACQUIRE) == gen;
}

static void wake(void)
{
    struct port_packet pkt = { .key = tm.key, .type = PORT_PACKET_USER };
    for (int i = 0; i < 100 && jam_port_queue(tm.port, &pkt) != OK; i++)
        jam_sleep_until(now() + 10 * NS_PER_MS);   /* a full port: the loop is far behind */
}

/* A line for the loop to say (a thread's printf isn't safe against init's own). */
static void say_later(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_later(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tm.line, sizeof(tm.line), fmt, ap);
    va_end(ap);
    __atomic_store_n(&tm.said, true, __ATOMIC_RELEASE);
}

/* ---- emptying a /tmp on a volume ------------------------------------------------------- */

/* The path of a directory entry: path + "/" + name into out. False if it
 * doesn't fit. */
static bool join(char *out, const char *path, const char *name)
{
    int n = snprintf(out, PATH_MAX_LEN, "%s/%s", path, name);
    return n > 0 && n < PATH_MAX_LEN;
}

/* Everything below dir removed, as far as it can be and until the deadline:
 * a tree walk with the directory path itself as the stack (no recursion,
 * DEPTH_MAX deep). An entry that can't be removed stays (skip counts them
 * in each directory, so the walk goes on past them). Returns how many
 * entries went; *left: some stayed (or the time ran out). */
static unsigned empty_dir(const char *dir, uint64_t deadline, uint32_t gen, bool *left)
{
    static char path[PATH_MAX_LEN], child[PATH_MAX_LEN];   /* the thread's alone */
    unsigned skip[DEPTH_MAX + 1] = { 0 };
    unsigned depth = 0, removed = 0;
    snprintf(path, sizeof(path), "%s", dir);
    *left = false;
    for (;;) {
        if (now() >= deadline || !current(gen)) {
            *left = true;
            return removed;
        }
        struct fs_entry e;
        if (skip[depth])
            (void)fs_readdir(path, 0, &e);   /* libos goes on from a batch it kept: ask afresh */
        status_t st = fs_readdir(path, skip[depth], &e);
        if (st != OK) {   /* the end of this directory (or it can't be read) */
            if (depth == 0)
                return removed;
            bool gone = st == ERR_NOT_FOUND && fs_unlink(path) == OK;
            *strrchr(path, '/') = '\0';   /* up to the parent */
            depth--;
            if (gone) {
                removed++;
            } else {
                *left = true;   /* the subdirectory stays: not empty, or it can't be removed */
                skip[depth]++;
            }
            continue;
        }
        if (!join(child, path, e.name)) {
            *left = true;
            skip[depth]++;   /* a name too long to follow */
            continue;
        }
        if (e.is_dir && depth < DEPTH_MAX) {
            snprintf(path, sizeof(path), "%s", child);
            skip[++depth] = 0;
        } else if (!e.is_dir && fs_unlink(child) == OK) {
            removed++;
        } else {
            *left = true;
            skip[depth]++;
        }
    }
}

/* ---- the sources ------------------------------------------------------------------------ */

/* A view of s's tmp directory into *out: made if it isn't there, emptied
 * the first time this boot. ERR_NOT_SUPPORTED: the volume has no views
 * (FAT: asked first, so nothing is made on it). */
static status_t view_of(enum src s, uint32_t gen, handle_t *out, char *note, size_t cap)
{
    status_t st = fs_subview(srcs[s].dir, 0, out);
    if (st == ERR_NOT_FOUND) {
        st = fs_mkdir_mode(srcs[s].dir, FS_MODE_DIR_DEFAULT);
        if (st == OK || st == ERR_ALREADY_EXISTS)
            st = fs_subview(srcs[s].dir, 0, out);
    }
    if (st != OK)
        return st;
    note[0] = '\0';
    if (!tm.emptied[s]) {
        bool left = false;
        unsigned removed = empty_dir(srcs[s].dir, now() + TMP_LIMIT, gen, &left);
        tm.emptied[s] = !left || current(gen);
        snprintf(note, cap, " (emptied: %u removed%s)", removed, left ? ", some left" : "");
    }
    return OK;
}

/* Is the memory /tmp's service running? */
static bool ram_alive(void)
{
    signals_t seen;
    return tm.ram_proc && jam_object_wait_one(tm.ram_proc, SIG_TERMINATED, 0, &seen) != OK;
}

/* The memory /tmp's service ended (killed if it still runs), its handles closed. */
static void ram_stop(void)
{
    if (tm.ram_proc) {
        (void)jam_port_unbind(tm.port, tm.ram_proc, KEY_TMP);
        if (tm.ram_job)
            jam_job_kill(tm.ram_job);
        jam_handle_close(tm.ram_proc);
    }
    if (tm.ram_job)
        jam_handle_close(tm.ram_job);
    tm.ram_proc = tm.ram_job = HANDLE_INVALID;
}

/* bin/extfs on a new VMO, formatting it: *client its `fs` channel. */
static status_t ram_start(handle_t *client, uint64_t *size)
{
    if (tm.ram_starts >= RAM_STARTS)
        return ERR_NO_RESOURCES;
    ram_stop();   /* a dead one's handles */
    struct sys_public_info si;
    *size = RAM_MAX;
    if (jam_sys_public_info(&si) == OK && si.mem_total_pages * PAGE_SIZE / 8 < RAM_MAX)
        *size = si.mem_total_pages * PAGE_SIZE / 8 < RAM_MIN ? RAM_MIN
                                                            : si.mem_total_pages * PAGE_SIZE / 8;
    *size -= *size % (1u << 20);
    handle_t vmo = HANDLE_INVALID, mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = jam_vmo_create(*size, 0, &vmo);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    if (st == OK)
        st = jam_channel_set_flags(theirs, CHANNEL_NO_HANDLES);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &tm.ram_job);
    if (st != OK) {
        if (vmo)
            jam_handle_close(vmo);
        if (mine)
            jam_handle_close(mine);
        if (theirs)
            jam_handle_close(theirs);
        tm.ram_job = HANDLE_INVALID;
        return st;
    }
    const char *argv[] = { EXTFS, TMP_MOUNT, EXTFS_ARG_FORMAT };
    struct spawn_handle x[2] = { { EXTFS_SR_RAM, vmo }, { EXTFS_SR_SERVE, theirs } };
    struct spawn_args a = {
        .path = EXTFS, .name = "extfs-tmp", .argc = 3, .argv = argv, .job = tm.ram_job,
        .extra = x, .nextra = 2,
    };
    tm.ram_starts++;
    st = spawn(&a, &tm.ram_proc);   /* the extras go, whatever happens */
    if (st == OK)
        st = jam_port_bind(tm.port, tm.ram_proc, KEY_TMP, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st != OK) {
        ram_stop();
        jam_handle_close(mine);
        return st;
    }
    *client = mine;   /* the namespace's: when its last holder closes it, extfs ends */
    return OK;
}

/* ---- the thread --------------------------------------------------------------------- */

/* Where /tmp should be from now: the first source that is there and
 * read-write (a view of it may still be refused: then memory). */
static enum src wanted(void)
{
    bool ro = true;
    if (fs_statfs(JAM_MOUNT, NULL, NULL, &ro, NULL) == OK && !ro)
        return SRC_JAM;
    if (fs_statfs(DATA_MOUNT, NULL, NULL, &ro, NULL) == OK && !ro)
        return SRC_DATA;
    return SRC_RAM;
}

/* One pass for generation gen: /tmp from the best source, if it isn't. */
static void pass(uint32_t gen)
{
    enum src want = wanted();
    uint32_t sgen = __atomic_load_n(&tm.src_gen[want], __ATOMIC_ACQUIRE);
    if (want == tm.cur && sgen == tm.used_gen && (want != SRC_RAM || ram_alive()))
        return;   /* as it should be */
    handle_t h = HANDLE_INVALID;
    char note[64] = "";
    status_t st = ERR_NOT_FOUND;
    if (want != SRC_RAM) {
        st = view_of(want, gen, &h, note, sizeof(note));
        if (st != OK && st != ERR_NOT_SUPPORTED)
            say_later("init: /tmp can't be a view of %s (%s): it is in memory", srcs[want].dir,
                      status_str(st));
        if (st != OK) {
            want = SRC_RAM;
            sgen = __atomic_load_n(&tm.src_gen[want], __ATOMIC_ACQUIRE);
            if (tm.cur == SRC_RAM && ram_alive())
                return;   /* in memory already */
        }
    }
    uint64_t size = 0;
    if (want == SRC_RAM) {
        bool again = tm.cur == SRC_RAM;   /* its service ended by itself */
        st = ram_start(&h, &size);
        if (st != OK) {
            say_later("init: no /tmp in memory (%s)%s", status_str(st),
                      tm.cur != SRC_NONE ? ": the last one stays" : ": no /tmp");
            return;
        }
        snprintf(note, sizeof(note), " (%lu MiB at most%s)", (unsigned long)(size >> 20),
                 again ? ", made again: its service ended" : "");
    }
    st = ns_mount(TMP_MOUNT, h);   /* replaces the old /tmp */
    if (st != OK) {
        say_later("init: /tmp can't be mounted (%s)", status_str(st));
        if (want == SRC_RAM)
            ram_stop();
        return;
    }
    if (want != SRC_RAM && tm.ram_proc)
        ram_stop();   /* the memory one is replaced: it goes */
    tm.cur = want;
    tm.used_gen = sgen;
    if (want == SRC_RAM)
        say_later("init: /tmp: in memory%s", note);
    else
        say_later("init: /tmp: a view of %s%s", srcs[want].dir, note);
    __atomic_store_n(&tm.changed, true, __ATOMIC_RELEASE);
    wake();
}

static void thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t gen = __atomic_load_n(&tm.gen, __ATOMIC_ACQUIRE);
        pass(gen);
        __atomic_store_n(&tm.running, false, __ATOMIC_RELEASE);
        bool again = false;
        if (!current(gen)) {
            bool was = false;
            again = __atomic_compare_exchange_n(&tm.running, &was, true, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        if (!again)
            return;
    }
}

/* ---- init's side ---------------------------------------------------------------------- */

/* Something changed: the thread looks again (started, or it sees the new
 * generation and goes round once more). */
static void kick(void)
{
    if (!tm.on)
        return;
    __atomic_add_fetch(&tm.gen, 1, __ATOMIC_ACQ_REL);
    bool was = false;
    if (!__atomic_compare_exchange_n(&tm.running, &was, true, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    status_t st = OK;
    if (tm.thread) {   /* the last one has finished (running was false): its stack is free once it ends */
        signals_t seen;
        st = jam_object_wait_one(tm.thread, SIG_TERMINATED, now() + THREAD_END, &seen);
        jam_handle_close(tm.thread);
        tm.thread = HANDLE_INVALID;
    }
    if (st == OK)
        st = thread_spawn("tmp", thread_main, NULL, tm.stack, sizeof(tm.stack), &tm.thread);
    if (st != OK) {
        __atomic_store_n(&tm.running, false, __ATOMIC_RELEASE);
        tm.thread = HANDLE_INVALID;
        init_say("init: no thread for /tmp (%s)", status_str(st));
    }
}

void tmp_start(handle_t port, uint64_t key)
{
    if (tm.on)
        return;
    tm.on = true;
    tm.port = port;
    tm.key = key;
    kick();   /* the first /tmp: in memory until a volume gives one */
}

void tmp_mount_changed(const char *path, bool came)
{
    enum src s = !strcmp(path, JAM_MOUNT) ? SRC_JAM : !strcmp(path, DATA_MOUNT) ? SRC_DATA
                                                                                : SRC_NONE;
    if (s == SRC_NONE)
        return;
    if (came)
        __atomic_add_fetch(&tm.src_gen[s], 1, __ATOMIC_ACQ_REL);   /* a new channel: a new view */
    kick();
}

void tmp_event(void)
{
    kick();   /* the memory /tmp's service ended: the thread sees it isn't running */
}

bool tmp_changed(void)
{
    if (__atomic_exchange_n(&tm.said, false, __ATOMIC_ACQ_REL))
        init_say("%s", tm.line);
    return __atomic_exchange_n(&tm.changed, false, __ATOMIC_ACQ_REL);
}
