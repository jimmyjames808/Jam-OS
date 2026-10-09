/* volumes: the list of mounts, its generation, and the watches
 * (volumes.watch): channels on which each change is sent as `mounts`
 * events (abi/idl/volumes.idl).
 *
 * The mounts themselves are disk.c's (disk_mounts). This file keeps the
 * list as the watches were last told it: whenever something may have
 * changed it (a service started or ended, a disk came or went)
 * mounts_update compares, and only a real difference moves the generation
 * on. A mount with a new `fs` channel (a remount, a service started again
 * after it was given up on) differs by its channel's generation, so it is
 * `unmounted` and `mounted` again even though the path is the same; a
 * filesystem service restarted on the channel volumes kept is no change
 * (fat keeps its views and files across its deaths: namespaces need
 * nothing new). Generations count up from a number taken from the clock
 * (first_generation says why).
 *
 * A watch is one end of a channel volumes made (the watcher has the
 * other), marked to receive no handles: nothing comes back on it. A new
 * one gets the whole list (every mount, then `settled`) once its end has
 * gone out in the reply (mounts_list_new): sent before, the `fs` channels
 * in its queue would make the kernel refuse to send the end at all. A
 * watcher that leaves MOUNTS_UNREAD_MAX events unread (<jam/abi.h>'s
 * handle_info: peer_queued), whose end is closed, or whose queue refuses
 * an event is dropped: its channel is closed, and it watches again. */
#include "internal.h"

#define MAX_WATCHES 16

struct watch {
    handle_t ch;      /* our end; 0: a free slot */
    bool     listed;  /* it has had the whole list */
};

static struct mount  cur[MAX_VOLS];   /* the list of generation `generation` */
static unsigned      ncur;
static uint32_t      generation;      /* 0 only until first_generation() */
static struct watch  watches[MAX_WATCHES];

/* The first generation comes from the clock (uptime in ms, made odd: never
 * 0). A volumes that init started again after the first one died would
 * otherwise count 1, 2, ... like the first, and a watcher might take the
 * new one's for the old one's. */
static void first_generation(void)
{
    if (!generation)
        generation = (uint32_t)(now() / NS_PER_MS) | 1;
}

static void drop(struct watch *w, const char *why)
{
    if (why)
        say(false, "volumes: a watch %s: dropped", why);
    jam_handle_close(w->ch);
    *w = (struct watch){ 0 };
}

/* May w be sent n more events? Dropped (false) if its watcher is gone or
 * has left too many unread. */
static bool room(struct watch *w, unsigned n)
{
    struct handle_info info;
    if (jam_handle_info(w->ch, &info) != OK || !info.related_koid) {
        drop(w, NULL);   /* the watcher closed its end: nobody to tell */
        return false;
    }
    if (info.peer_queued + n > MOUNTS_UNREAD_MAX) {
        char why[64];
        snprintf(why, sizeof(why), "left %u events unread", info.peer_queued);
        drop(w, why);
        return false;
    }
    return true;
}

/* `mounted` for m, with a duplicate of its channel: OK, or why not. */
static status_t tell_mounted(handle_t ch, const struct mount *m)
{
    handle_t fs;
    status_t st = jam_handle_duplicate(vols[m->vol].client, RIGHT_SAME, &fs);
    if (st != OK)
        return st;
    return mounts_mounted(ch, m->path, (uint32_t)strnlen(m->path, sizeof(m->path)), generation,
                          m->flags, fs);   /* fs goes either way */
}

static bool same_mount(const struct mount *a, const struct mount *b)
{
    return a->vol == b->vol && a->gen == b->gen && !strcmp(a->path, b->path);
}

/* Is m in list l (n entries)? */
static bool listed(const struct mount *l, unsigned n, const struct mount *m)
{
    for (unsigned i = 0; i < n; i++)
        if (same_mount(&l[i], m))
            return true;
    return false;
}

/* Tell w the change from old (nold) to cur: what went, what came, then
 * `settled`. False if it was dropped. */
static bool tell_change(struct watch *w, const struct mount *old, unsigned nold)
{
    unsigned n = 1;
    for (unsigned i = 0; i < nold; i++)
        n += !listed(cur, ncur, &old[i]);
    for (unsigned i = 0; i < ncur; i++)
        n += !listed(old, nold, &cur[i]);
    if (!room(w, n))
        return false;
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < nold; i++)
        if (!listed(cur, ncur, &old[i]))
            st = mounts_unmounted(w->ch, old[i].path,
                                  (uint32_t)strnlen(old[i].path, sizeof(old[i].path)), generation);
    for (unsigned i = 0; st == OK && i < ncur; i++)
        if (!listed(old, nold, &cur[i]))
            st = tell_mounted(w->ch, &cur[i]);
    if (st == OK)
        st = mounts_settled(w->ch, generation);
    if (st != OK) {
        char why[48];
        snprintf(why, sizeof(why), "couldn't be told (%s)", status_str(st));
        drop(w, why);
        return false;
    }
    return true;
}

/* The whole list to a new watch w. */
static void tell_all(struct watch *w)
{
    w->listed = true;
    if (!room(w, ncur + 1))
        return;
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < ncur; i++)
        st = tell_mounted(w->ch, &cur[i]);
    if (st == OK)
        st = mounts_settled(w->ch, generation);
    if (st != OK) {
        char why[48];
        snprintf(why, sizeof(why), "couldn't be told the mounts (%s)", status_str(st));
        drop(w, why);
    }
}

void mounts_update(void)
{
    static struct mount fresh[MAX_VOLS], old[MAX_VOLS];
    memset(fresh, 0, sizeof(fresh));
    unsigned n = disk_mounts(fresh);
    bool same = n == ncur;
    first_generation();
    for (unsigned i = 0; same && i < n; i++)
        same = same_mount(&fresh[i], &cur[i]) && fresh[i].flags == cur[i].flags;
    if (same)
        return;
    memcpy(old, cur, sizeof(old));
    unsigned nold = ncur;
    memcpy(cur, fresh, sizeof(cur));
    ncur = n;
    if (++generation == 0)
        generation = 1;
    char list[MAX_VOLS * 17 + 8] = " (none)";
    for (unsigned i = 0, at = 0; i < n; i++)
        at += (unsigned)snprintf(list + at, sizeof(list) - at, " %s", cur[i].path);
    say(false, "volumes: mounts, generation %u:%s", generation, list);
    for (unsigned i = 0; i < MAX_WATCHES; i++)
        if (watches[i].ch && watches[i].listed)
            (void)tell_change(&watches[i], old, nold);
}

status_t mounts_watch(handle_t *out)
{
    struct watch *w = NULL;
    for (unsigned i = 0; i < MAX_WATCHES; i++) {
        struct handle_info info;
        if (watches[i].ch && (jam_handle_info(watches[i].ch, &info) != OK || !info.related_koid))
            drop(&watches[i], NULL);   /* its watcher has gone */
        if (!watches[i].ch && !w)
            w = &watches[i];
    }
    if (!w)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_channel_set_flags(mine, CHANNEL_NO_HANDLES);   /* nothing comes back */
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    *w = (struct watch){ .ch = mine };
    *out = theirs;
    return OK;
}

void mounts_list_new(void)
{
    first_generation();
    for (unsigned i = 0; i < MAX_WATCHES; i++)
        if (watches[i].ch && !watches[i].listed)
            tell_all(&watches[i]);
}

void mounts_close_all(void)
{
    for (unsigned i = 0; i < MAX_WATCHES; i++)
        if (watches[i].ch)
            drop(&watches[i], NULL);
}
