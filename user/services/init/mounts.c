/* init's mounts from volumes: /data and /esp, and another stick's /usbN,
 * once volumes' fat services serve them.
 *
 * init follows them through a watch (volumes.watch): a channel on which
 * volumes sends `mounts` events (abi/idl/volumes.idl), bound on init's
 * port. Each `mounted` goes straight into init's namespace (again, for a
 * path that is there: its channel is a new one), each `unmounted` leaves
 * it, and `settled` says a change is whole: mounts_event's caller tells
 * the followers then. Nothing here waits for a change; the one call
 * (volumes.watch) is answered at once.
 *
 * Mounts flagged MOUNTS_TEST are the test suite's mock disks (volumes'
 * test_disk): init leaves them alone. /boot is init's own: volumes can't
 * put something else there.
 *
 * A watch that closes while volumes still runs (volumes drops a watcher
 * that falls MOUNTS_UNREAD_MAX events behind) is made again at once, and
 * the new one lists every mount. When volumes itself is gone,
 * mounts_unwatch takes its mounts away. */
#include <os.h>
#include <volumes.h>
#include "init.h"

#define MAX_HAVE  16                  /* mounts init keeps from volumes */
#define CALL_WAIT (5 * NS_PER_S)      /* volumes.watch */
#define SYNC_WAIT (2 * NS_PER_S)

static handle_t ctl;      /* a duplicate of volumes' control channel (0: none) */
static handle_t watch;    /* our end of the watch (0: none) */
static handle_t port;     /* where the watch is bound (0: nowhere) */
static uint64_t port_key; /* ... with this key */
/* What is mounted from volumes now, and what was at the last `settled`
 * (the log says what came and went, once a change is whole). */
static char     have[MAX_HAVE][NS_NAME_MAX], had[MAX_HAVE][NS_NAME_MAX];
static unsigned nhave, nhad;
static bool     changing;   /* an event came since the last `settled` */

static int find(char list[][NS_NAME_MAX], unsigned n, const char *path)
{
    for (unsigned i = 0; i < n; i++)
        if (!strncmp(list[i], path, NS_NAME_MAX))
            return (int)i;
    return -1;
}

static void begin_change(void)
{
    if (changing)
        return;
    changing = true;
    memcpy(had, have, sizeof(had));
    nhad = nhave;
}

static void ev_mounted(void *ctx, const char *p, uint32_t len, uint32_t generation, uint8_t flags,
                       handle_t fs)
{
    (void)ctx;
    (void)generation;
    char path[NS_NAME_MAX];
    if (flags & MOUNTS_TEST || len >= sizeof(path)) {
        jam_handle_close(fs);
        return;
    }
    memcpy(path, p, len);
    path[len] = '\0';
    begin_change();
    status_t st = strcmp(path, BOOT_MOUNT) ? ns_mount(path, fs) : ERR_ACCESS_DENIED;
    if (st == ERR_ACCESS_DENIED)
        jam_handle_close(fs);
    if (st == OK && find(have, nhave, path) < 0 && nhave < MAX_HAVE)
        memcpy(have[nhave++], path, sizeof(path));
    else if (st != OK)
        printf("init: can't mount %s (%s)\n", path, status_str(st));
}

/* path leaves the namespace and the list. */
static void forget(const char *path)
{
    int i = find(have, nhave, path);
    if (i < 0)
        return;
    (void)ns_unmount(path);
    memmove(have[i], have[i + 1], (size_t)(nhave - (unsigned)i - 1) * NS_NAME_MAX);
    nhave--;
}

static void ev_unmounted(void *ctx, const char *p, uint32_t len, uint32_t generation)
{
    (void)ctx;
    (void)generation;
    char path[NS_NAME_MAX];
    if (len >= sizeof(path))
        return;
    memcpy(path, p, len);
    path[len] = '\0';
    begin_change();
    forget(path);
}

/* The change is whole: what went and what came, in the log. */
static void ev_settled(void *ctx, uint32_t generation)
{
    bool *settled = ctx;
    (void)generation;
    begin_change();
    for (unsigned i = 0; i < nhad; i++)
        if (find(have, nhave, had[i]) < 0)
            printf("init: %s is gone\n", had[i]);
    for (unsigned i = 0; i < nhave; i++)
        if (find(had, nhad, have[i]) < 0)
            printf("init: %s mounted\n", have[i]);
    changing = false;
    *settled = true;
}

static const struct mounts_events events = {
    .mounted = ev_mounted, .unmounted = ev_unmounted, .settled = ev_settled,
};

static void unmount_all(void)
{
    while (nhave) {
        char path[NS_NAME_MAX];
        memcpy(path, have[nhave - 1], sizeof(path));
        forget(path);
        printf("init: %s is gone\n", path);
    }
    changing = false;
}

static void close_watch(void)
{
    if (!watch)
        return;
    if (port)
        (void)jam_port_unbind(port, watch, port_key);
    jam_handle_close(watch);
    watch = HANDLE_INVALID;
}

/* A new watch on ctl, bound on the port. */
static status_t open_watch(void)
{
    status_t st = volumes_watch_until(ctl, now() + CALL_WAIT, &watch);
    if (st != OK) {
        watch = HANDLE_INVALID;
        return st;
    }
    if (port)
        st = jam_port_bind(port, watch, port_key, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK)
        close_watch();
    return st;
}

status_t mounts_watch(handle_t volumes_ctl, handle_t to_port, uint64_t key)
{
    if (ctl) {
        jam_handle_close(volumes_ctl);
        return ERR_BAD_STATE;
    }
    ctl = volumes_ctl;
    port = to_port;
    port_key = key;
    status_t st = open_watch();
    if (st != OK) {
        jam_handle_close(ctl);
        ctl = HANDLE_INVALID;
    }
    return st;
}

bool mounts_event(void)
{
    bool settled = false;
    status_t st = OK;
    for (int guard = 0; watch && guard < 256 && st != ERR_SHOULD_WAIT; guard++) {
        st = mounts_read_event(watch, &events, &settled);
        if (st != ERR_PEER_CLOSED)
            continue;
        /* volumes dropped us (we fell behind), or it is gone: unmount what
         * it gave, and watch again if it still answers. */
        close_watch();
        unmount_all();
        settled = true;
        st = ctl ? open_watch() : ERR_PEER_CLOSED;
        printf("init: volumes' watch closed: %s\n",
               st == OK ? "watching again" : "only " BOOT_MOUNT " until volumes is back");
    }
    return settled;
}

void mounts_unwatch(void)
{
    close_watch();
    if (ctl)
        jam_handle_close(ctl);
    ctl = HANDLE_INVALID;
    unmount_all();
}

void mounts_settle(void)
{
    (void)fs_sync_by(DATA_MOUNT, now() + NS_PER_S);   /* best effort: the reset comes next */
}

void mounts_sync(void)
{
    if (!ctl)
        return;   /* no volumes: nothing was written */
    uint64_t t0 = now();
    mounts_synced(volumes_sync_until(ctl, t0 + SYNC_WAIT), t0);
}

void mounts_synced(status_t st, uint64_t t0)
{
    bool data = find(have, nhave, DATA_MOUNT) >= 0;
    if (st == OK && data)
        printf("init: " DATA_MOUNT " synced in %lu ms\n",
               (unsigned long)((now() - t0) / NS_PER_MS));
    else if (st != OK)
        printf("init: %s not synced (%s)\n", data ? DATA_MOUNT : "the mounts", status_str(st));
}
