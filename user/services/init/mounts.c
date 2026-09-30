/* init's mounts from devmgr: /data and /esp, and another stick's /usbN,
 * once devmgr's fat services serve them.
 *
 * DEVMGR_MOUNTS (<devmgr.h>) answers when the set of mounts differs from
 * the generation the caller knows, or says ERR_TIMED_OUT after two
 * seconds: a call made to wait. So the asking is done by a thread of its
 * own, the watcher, in a loop: init's main loop never waits for devmgr.
 * Each answer goes straight into init's namespace (libos's is safe to use
 * from two threads); the main loop only hears that something changed,
 * through its port.
 *
 * Mounts called /...-test are the test suite's mock disks (devmgr's
 * DEVMGR_TEST_DISK): init leaves them alone.
 *
 * Stopping: the watcher's call ends when devmgr dies (ERR_PEER_CLOSED) or
 * when the last handle of init's end is closed (mounts_unwatch closes the
 * watcher's duplicate). mounts_unwatch then waits for the thread before it
 * returns, so the closed handle's number is not reused under a watcher
 * that is still between two calls. */
#include <devmgr.h>
#include <os.h>
#include "init.h"

#define WATCH_STACK (64u << 10)
#define STOP_WAIT   (DEVMGR_MOUNTS_WAIT + 2 * NS_PER_S)   /* a call in flight ends within this */
#define SYNC_WAIT   (2 * NS_PER_S)

static handle_t ctl;      /* the watcher's end of devmgr's control channel */
static handle_t thread;   /* the watcher (0: none) */
static handle_t port;     /* where a change is announced (0: nowhere) */
static uint64_t port_key; /* ... with this key */
static bool stop;         /* set by mounts_unwatch before it closes ctl; read by the watcher */
static bool stuck;        /* a watcher didn't end in time: its stack can't be used again */
/* What the watcher mounted. The watcher's while it runs; mounts_unwatch
 * reads it only after the thread has ended. */
static char have[DEVMGR_MAX_MOUNTS][NS_NAME_MAX];
static unsigned nhave;
static uint8_t watch_stack[WATCH_STACK];

/* "/data-test", "/esp-test": a test's disk, not the system's. */
static bool test_mount(const char *path)
{
    size_t n = strnlen(path, NS_NAME_MAX);
    return n > 5 && n < NS_NAME_MAX && !strcmp(path + n - 5, "-test");
}

static bool listed(const struct devmgr_mounts_rep *rep, const char *path)
{
    for (uint32_t i = 0; i < rep->count; i++)
        if (!strncmp(rep->mounts[i].path, path, NS_NAME_MAX))
            return true;
    return false;
}

/* An answer into the namespace: the mounts it no longer lists go, the
 * ones it lists are mounted (again: a restarted service has a new
 * channel). hs: its handles, consumed. */
static void apply(const struct devmgr_mounts_rep *rep, const handle_t *hs)
{
    char had[DEVMGR_MAX_MOUNTS][NS_NAME_MAX];
    unsigned nhad = nhave;
    memcpy(had, have, sizeof(had));
    for (unsigned i = 0; i < nhave; i++)
        if (!listed(rep, have[i]) && ns_unmount(have[i]) == OK)
            printf("init: %s is gone\n", have[i]);
    nhave = 0;
    for (uint32_t i = 0; i < rep->count; i++) {
        char path[NS_NAME_MAX + 1];
        memcpy(path, rep->mounts[i].path, NS_NAME_MAX);
        path[NS_NAME_MAX] = '\0';
        if (test_mount(path)) {
            jam_handle_close(hs[i]);
            continue;
        }
        /* /boot is init's own: devmgr can't put something else there. */
        status_t st = strcmp(path, BOOT_MOUNT) ? ns_mount(path, hs[i]) : ERR_ACCESS_DENIED;
        if (st == ERR_ACCESS_DENIED)
            jam_handle_close(hs[i]);
        if (st != OK) {
            printf("init: can't mount %s (%s)\n", path, status_str(st));
            continue;
        }
        bool again = false;   /* still there: only its channel may be another */
        for (unsigned k = 0; k < nhad && !again; k++)
            again = !strcmp(had[k], path);
        if (!again)
            printf("init: %s mounted\n", path);
        memcpy(have[nhave++], path, NS_NAME_MAX);
    }
}

static void watcher(void *arg)
{
    (void)arg;
    uint32_t known = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        struct devmgr_mounts_rep rep;
        handle_t hs[DEVMGR_MAX_MOUNTS];
        status_t st = devmgr_mounts(ctl, known, &rep, hs);
        if (st == ERR_TIMED_OUT)
            continue;   /* nothing changed: ask again */
        /* Gone (or going, or stopped by mounts_unwatch) needs no words. */
        if (st != OK && st != ERR_PEER_CLOSED && st != ERR_CANCELED && st != ERR_BAD_HANDLE)
            printf("init: devmgr's mounts: %s: only " BOOT_MOUNT " from now on\n",
                   status_str(st));
        if (st != OK)
            return;
        known = rep.generation;
        apply(&rep, hs);
        struct port_packet pkt = { .key = port_key, .type = PORT_PACKET_USER };
        /* A full port: the shell hears of it with the next change. */
        if (port)
            (void)jam_port_queue(port, &pkt);
    }
}

status_t mounts_watch(handle_t devmgr_ctl, handle_t to_port, uint64_t key)
{
    if (thread || stuck) {
        jam_handle_close(devmgr_ctl);
        return ERR_BAD_STATE;
    }
    ctl = devmgr_ctl;
    port = to_port;
    port_key = key;
    __atomic_store_n(&stop, false, __ATOMIC_RELEASE);
    status_t st = thread_spawn("mounts", watcher, NULL, watch_stack, sizeof(watch_stack), &thread);
    if (st != OK) {
        jam_handle_close(ctl);
        ctl = thread = HANDLE_INVALID;
    }
    return st;
}

void mounts_unwatch(void)
{
    if (!thread)
        return;
    __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
    jam_handle_close(ctl);   /* the last handle of our end: the watcher's call ends */
    ctl = HANDLE_INVALID;
    signals_t seen;
    status_t st = jam_object_wait_one(thread, SIG_TERMINATED, now() + STOP_WAIT, &seen);
    jam_handle_close(thread);
    thread = HANDLE_INVALID;
    if (st != OK) {
        init_say("init: the mounts watcher didn't end (%s): no more mounts from devmgr",
                 status_str(st));
        stuck = true;
        return;
    }
    for (unsigned i = 0; i < nhave; i++)
        if (ns_unmount(have[i]) == OK)
            printf("init: %s is gone\n", have[i]);
    nhave = 0;
}

void mounts_settle(void)
{
    (void)fs_sync_by(DATA_MOUNT, now() + NS_PER_S);   /* best effort: the reset comes next */
}

void mounts_sync(void)
{
    uint64_t t0 = now();
    status_t st = fs_sync_by(DATA_MOUNT, t0 + SYNC_WAIT);
    if (st == OK)
        printf("init: " DATA_MOUNT " synced in %lu ms\n",
               (unsigned long)((now() - t0) / NS_PER_MS));
    else if (st != ERR_NOT_FOUND)   /* no /data: nothing was written */
        printf("init: " DATA_MOUNT " not synced (%s)\n", status_str(st));
    /* Another stick's mounts, in what is left of the time: a read-only one
     * answers at once, one made writable is flushed. */
    for (unsigned n = 0; n < USB_MOUNTS; n++) {
        char path[NS_NAME_MAX];
        snprintf(path, sizeof(path), USB_MOUNT "%u", n);
        st = fs_sync_by(path, t0 + SYNC_WAIT);
        if (st != OK && st != ERR_NOT_FOUND)
            printf("init: %s not synced (%s)\n", path, status_str(st));
    }
}
