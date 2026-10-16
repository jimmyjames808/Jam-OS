/* init's /tmp and /pkg (docs/M13.6-PLAN.md's U7b and DB1; decided after
 * CB2: Chromium, like most ported software, takes a writable /tmp for
 * granted). In shell mode every program init starts, and every shell it
 * follows, has a /tmp, emptied at boot, from the first of these that
 * works:
 *   1. a view of /jam/tmp (fs.subview), when the Jam OS drive's ext volume
 *      is mounted read-write at /jam (its other views, /usr, /var and
 *      /home, are jammounts.c's). Booted from that drive, /jam is its data
 *      partition, /data under a second name (volumes, DB1);
 *   2. a view of /data/tmp, when /data is read-write and can give one: an
 *      ext /data (a stick made with an ext /data and no Jam OS drive in).
 *      A FAT /data can't: fat has no views of directories;
 *   3. an ext volume in memory: bin/extfs on a VMO of init's
 *      (EXTFS_SR_RAM), formatted at its start, so it is empty at every
 *      boot. Its size is an eighth of the memory, between RAM_MIN and
 *      RAM_MAX; a VMO's pages are taken only as they are written, so an
 *      empty /tmp costs a few pages and the service's own memory. It has a
 *      pager of its own, made here as volumes makes each mount's, so its
 *      files can be mapped (file.map, a POSIX mmap) as on any ext volume.
 * A /tmp on a volume is emptied the first time it is used this boot (what
 * the last boot, or a cut, left there goes); one that comes back later
 * (/jam after the installer let go of it) keeps what it has.
 *
 * /pkg is the package area as a read-only view, from the same place
 * bin/pkg installs to (user/services/pkg/main.c's pick_prefix): /jam/pkg
 * while /jam is read-write, else /data/pkg, else (a FAT /data, no volume
 * at all) none. The directory is made if it isn't there. Nothing writes
 * through /pkg: bin/pkg changes the area by its own grant, under the name
 * it chose, and the view follows /jam as /tmp does. Ports find their files
 * under /pkg whichever volume holds them (M13.7's PO14).
 *
 * The choice is made again, on a thread of its own (a /tmp full of files
 * takes a while to empty; init's loop must not wait), whenever /jam or
 * /data is mounted or goes (tmp_mount_changed, from mounts.c) and when the
 * memory volume's service ends (KEY_TMP): /jam and /data are looked at in
 * init's own namespace (fs.statfs: there and read-write), and /tmp and
 * /pkg are mounted from the best source when that isn't what they are now
 * (ns_mount replaces the old mount). A memory /tmp that a better one
 * replaces is ended (files open in it see their channel close, its
 * mappings their pager go); one that ends by itself is made again, empty,
 * at most RAM_STARTS times a boot. The loop is told (KEY_MOUNTS:
 * tmp_changed) to update the followers, as for jammounts.c's views. The
 * thread is the only one that touches the service's handles. */
#include <extfssvc.h>
#include <fsview.h>
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
#define PAGER_PAGES  4096          /* the memory /tmp's pager budget when the memory's size is
                                    * unknown (else a sixteenth of it, as volumes' mounts) */
#define EXTFS        "bin/extfs"
#define TMP_MOUNT    "/tmp"
#define PKG_MOUNT    "/pkg"

enum src { SRC_NONE, SRC_JAM, SRC_DATA, SRC_RAM, SRC_COUNT };
enum line { LINE_TMP, LINE_PKG, LINE_COUNT };   /* the log lines the loop says */

static const char *const src_mount[SRC_COUNT] = { [SRC_JAM] = JAM_MOUNT,
                                                  [SRC_DATA] = DATA_MOUNT };

static struct {
    bool     on;             /* tmp_start was called: shell mode */
    handle_t port;           /* the loop's: KEY_MOUNTS wakes it, KEY_TMP the service's end */
    uint64_t key;
    uint32_t gen;            /* bumped by every change the thread must look at (atomic) */
    uint32_t src_gen[SRC_COUNT];   /* bumped when /jam or /data is mounted again (atomic) */
    bool     running;        /* the thread is at work (atomic) */
    handle_t thread;
    bool     changed;        /* /tmp or /pkg was mounted: the loop tells the followers (atomic) */
    bool     said[LINE_COUNT];        /* the lines below are for the loop to say (atomic) */
    char     line[LINE_COUNT][160];   /* ... each written before its `said` is set */
    /* The thread's alone. */
    enum src cur;            /* where /tmp is from now (SRC_NONE: no /tmp) */
    uint32_t used_gen;       /* ... the src_gen it was made at */
    bool     emptied[SRC_COUNT];   /* /jam/tmp, /data/tmp emptied this boot */
    handle_t ram_job, ram_proc;    /* the memory /tmp's service (0: none) */
    unsigned ram_starts;
    enum src pkg_from;       /* the source /pkg was last looked at for ... */
    uint32_t pkg_gen;        /* ... at this src_gen */
    bool     pkg_looked;     /* ... (false: never yet) */
    bool     pkg_on;         /* /pkg is mounted */
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
static void say_later(enum line which, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say_later(enum line which, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tm.line[which], sizeof(tm.line[which]), fmt, ap);
    va_end(ap);
    __atomic_store_n(&tm.said[which], true, __ATOMIC_RELEASE);
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
static unsigned empty_dir(const char *dir, uint64_t deadline, bool *left)
{
    static char path[PATH_MAX_LEN], child[PATH_MAX_LEN];   /* the thread's alone */
    unsigned skip[DEPTH_MAX + 1] = { 0 };
    unsigned depth = 0, removed = 0;
    snprintf(path, sizeof(path), "%s", dir);
    *left = false;
    for (;;) {
        if (now() >= deadline) {
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

/* A view of the directory dir (flags: fs.view's) into *out, the directory
 * made if it isn't there. ERR_NOT_SUPPORTED: the volume has no views (FAT:
 * asked first, so nothing is made on it). */
static status_t view_of(const char *dir, uint32_t flags, handle_t *out)
{
    status_t st = fs_subview(dir, flags, out);
    if (st == ERR_NOT_FOUND) {
        st = fs_mkdir_mode(dir, FS_MODE_DIR_DEFAULT);
        if (st == OK || st == ERR_ALREADY_EXISTS)
            st = fs_subview(dir, flags, out);
    }
    return st;
}

/* A view of s's tmp directory into *out (dir: its path), emptied the
 * first time this boot. */
static status_t tmp_view_of(enum src s, handle_t *out, char *dir, char *note, size_t cap)
{
    snprintf(dir, 16, "%s/tmp", src_mount[s]);
    status_t st = view_of(dir, 0, out);
    if (st != OK)
        return st;
    note[0] = '\0';
    if (!tm.emptied[s]) {
        bool left = false;
        unsigned removed = empty_dir(dir, now() + TMP_LIMIT, &left);
        tm.emptied[s] = true;   /* once: what couldn't go in TMP_LIMIT stays */
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

/* The memory /tmp's pager, as volumes makes a mount's (spare.c): a budget of
 * a sixteenth of the memory, page requests answered within FS_CALL_TIMEOUT.
 * HANDLE_INVALID when it can't be made (said): the volume works, its files
 * aren't mapped (file.map is ERR_NOT_SUPPORTED). */
static handle_t ram_pager(const struct sys_public_info *si, bool have_si, char *note, size_t cap)
{
    uint64_t pages = have_si && si->mem_total_pages >= 16 * 256 ? si->mem_total_pages / 16
                                                                : PAGER_PAGES;
    handle_t p = HANDLE_INVALID;
    status_t st = jam_pager_create(startup_handle(SR_RESOURCE), pages, FS_CALL_TIMEOUT, 0, &p);
    if (st != OK) {
        snprintf(note, cap, ", no pager (%s): its files aren't mapped", status_str(st));
        return HANDLE_INVALID;
    }
    snprintf(note, cap, ", a pager of %lu pages", (unsigned long)pages);
    return p;
}

/* bin/extfs on a new VMO, formatting it, with its pager: *client its `fs`
 * channel; pnote what became of the pager. */
static status_t ram_start(handle_t *client, uint64_t *size, char *pnote, size_t cap)
{
    if (tm.ram_starts >= RAM_STARTS)
        return ERR_NO_RESOURCES;
    ram_stop();   /* a dead one's handles */
    struct sys_public_info si;
    bool have_si = jam_sys_public_info(&si) == OK;
    *size = RAM_MAX;
    if (have_si && si.mem_total_pages * PAGE_SIZE / 8 < RAM_MAX)
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
    struct spawn_handle x[3] = { { EXTFS_SR_RAM, vmo }, { EXTFS_SR_SERVE, theirs } };
    /* The pager with what extfs needs of it (volumes' spare.c gives the same):
     * no duplicate, no transfer. */
    rights_t xr[3] = { RIGHT_SAME, RIGHT_SAME, RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT };
    unsigned nx = 2;
    x[nx] = (struct spawn_handle){ EXTFS_SR_PAGER, ram_pager(&si, have_si, pnote, cap) };
    if (x[nx].h)
        nx++;
    struct spawn_args a = {
        .path = EXTFS, .name = "extfs-tmp", .argc = 3, .argv = argv, .job = tm.ram_job,
        .extra = x, .nextra = nx, .extra_rights = xr,
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

/* Where /tmp and /pkg should be from now: the first source that is there
 * and read-write (a view of it may still be refused: then memory for /tmp,
 * none for /pkg). */
static enum src wanted(void)
{
    bool ro = true;
    if (fs_statfs(JAM_MOUNT, NULL, NULL, &ro, NULL) == OK && !ro)
        return SRC_JAM;
    if (fs_statfs(DATA_MOUNT, NULL, NULL, &ro, NULL) == OK && !ro)
        return SRC_DATA;
    return SRC_RAM;
}

/* /tmp from the best source, if it isn't. */
static void pass_tmp(enum src want)
{
    uint32_t sgen = __atomic_load_n(&tm.src_gen[want], __ATOMIC_ACQUIRE);
    if (want == tm.cur && sgen == tm.used_gen && (want != SRC_RAM || ram_alive()))
        return;   /* as it should be */
    handle_t h = HANDLE_INVALID;
    char note[64] = "", dir[16] = "", pnote[64] = "";
    status_t st = ERR_NOT_FOUND;
    if (want != SRC_RAM) {
        st = tmp_view_of(want, &h, dir, note, sizeof(note));
        if (st != OK && st != ERR_NOT_SUPPORTED)
            say_later(LINE_TMP, "init: /tmp can't be a view of %s (%s): it is in memory", dir,
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
        st = ram_start(&h, &size, pnote, sizeof(pnote));
        if (st != OK) {
            say_later(LINE_TMP, "init: no /tmp in memory (%s)%s", status_str(st),
                      tm.cur != SRC_NONE ? ": the last one stays" : ": no /tmp");
            return;
        }
        snprintf(note, sizeof(note), " (%lu MiB at most%s%s)", (unsigned long)(size >> 20),
                 pnote, again ? ", made again: its service ended" : "");
    }
    st = ns_mount(TMP_MOUNT, h);   /* replaces the old /tmp */
    if (st != OK) {
        say_later(LINE_TMP, "init: /tmp can't be mounted (%s)", status_str(st));
        if (want == SRC_RAM)
            ram_stop();
        return;
    }
    if (want != SRC_RAM && tm.ram_proc)
        ram_stop();   /* the memory one is replaced: it goes */
    tm.cur = want;
    tm.used_gen = sgen;
    if (want == SRC_RAM)
        say_later(LINE_TMP, "init: /tmp: in memory%s", note);
    else
        say_later(LINE_TMP, "init: /tmp: a view of %s%s", dir, note);
    __atomic_store_n(&tm.changed, true, __ATOMIC_RELEASE);
}

/* /pkg from the source bin/pkg installs to, if it isn't: /jam/pkg or
 * /data/pkg, read-only; none when neither volume is there and read-write,
 * or the one that is can't give a view (FAT). */
static void pass_pkg(enum src want)
{
    if (want == SRC_RAM)
        want = SRC_NONE;   /* no volume holds packages */
    uint32_t sgen = __atomic_load_n(&tm.src_gen[want], __ATOMIC_ACQUIRE);
    if (tm.pkg_looked && want == tm.pkg_from && sgen == tm.pkg_gen)
        return;   /* as it should be (or can't be, and was said) */
    tm.pkg_looked = true;
    tm.pkg_from = want;
    tm.pkg_gen = sgen;
    char dir[16] = "";
    status_t st = ERR_NOT_FOUND;
    if (want != SRC_NONE) {
        handle_t h = HANDLE_INVALID;
        snprintf(dir, sizeof(dir), "%s/pkg", src_mount[want]);
        st = view_of(dir, FS_VIEW_READ_ONLY, &h);
        if (st == OK)
            st = ns_mount(PKG_MOUNT, h);   /* replaces the old /pkg */
    }
    if (st == OK) {
        tm.pkg_on = true;
        say_later(LINE_PKG, "init: /pkg: a view of %s, read-only", dir);
    } else {
        bool was = tm.pkg_on;
        if (was)
            (void)ns_unmount(PKG_MOUNT);   /* the view of a source that went */
        tm.pkg_on = false;
        if (want != SRC_NONE)
            say_later(LINE_PKG, "init: no /pkg: %s can't be a view (%s)", dir,
                      st == ERR_NOT_SUPPORTED ? "its volume is FAT" : status_str(st));
        else if (was)
            say_later(LINE_PKG, "init: /pkg is gone: no read-write /jam or /data");
        if (!was)
            return;   /* nothing changed in the namespace */
    }
    __atomic_store_n(&tm.changed, true, __ATOMIC_RELEASE);
}

/* One pass: /tmp and /pkg from the best source, if they aren't (the thread
 * goes round again while the generation moves). */
static void pass(void)
{
    enum src want = wanted();
    pass_tmp(want);
    pass_pkg(want);
    if (__atomic_load_n(&tm.changed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&tm.said[LINE_TMP], __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&tm.said[LINE_PKG], __ATOMIC_ACQUIRE))
        wake();
}

static void thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t gen = __atomic_load_n(&tm.gen, __ATOMIC_ACQUIRE);
        pass();
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
        init_say("init: no thread for /tmp and /pkg (%s)", status_str(st));
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
    for (unsigned i = 0; i < LINE_COUNT; i++)
        if (__atomic_exchange_n(&tm.said[i], false, __ATOMIC_ACQ_REL))
            init_say("%s", tm.line[i]);
    return __atomic_exchange_n(&tm.changed, false, __ATOMIC_ACQ_REL);
}
