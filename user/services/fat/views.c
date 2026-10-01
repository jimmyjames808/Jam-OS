/* fat: views (fs.view, <fsview.h>): narrower `fs` channels onto the
 * volume, for programs that get less than the whole of it (read-only, or
 * with the top-level `etc` left alone). Each is served like the fs
 * channel devmgr gave us, with its flags checked first (fs_view_serve_one),
 * FAT_BATCH requests per wakeup. A view goes when its client closes it,
 * and every view goes when fat ends: a view never keeps the volume served
 * (main.c ends when the fs channel's clients are gone). */
#include <fsview.h>
#include "fat.h"

static struct {
    handle_t ch;      /* our end (0: a free slot) */
    uint32_t flags;   /* FS_VIEW_* */
    uint32_t gen;     /* bumped on every use of the slot: its port key */
} views[FAT_VIEWS];

/* Wait (ONCE) for slot i's next request or its client's close. */
static status_t arm(unsigned i)
{
    return jam_port_bind(vol.port, views[i].ch, FAT_KEY_VIEW(i, views[i].gen),
                         SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

static void drop(unsigned i)
{
    jam_handle_close(views[i].ch);
    views[i].ch = HANDLE_INVALID;
}

status_t views_add(void *host, handle_t ch, uint32_t flags)
{
    (void)host;
    for (unsigned i = 0; i < FAT_VIEWS; i++) {
        if (views[i].ch)
            continue;
        views[i].ch = ch;
        views[i].flags = flags;
        views[i].gen++;
        status_t st = arm(i);
        if (st != OK)
            drop(i);
        return st;
    }
    jam_handle_close(ch);
    return ERR_NO_RESOURCES;
}

void views_event(uint64_t key)
{
    unsigned i = FAT_KEY_SLOT(key);
    if (i >= FAT_VIEWS || !views[i].ch || views[i].gen != FAT_KEY_GEN(key))
        return;   /* a closed slot's packet, or a reused one's old one */
    status_t st = OK;
    for (unsigned k = 0; k < FAT_BATCH && st == OK && !vol.disk_gone; k++) {
        files_reap();
        st = fs_view_serve_one(views[i].ch, views[i].flags, &fat_fs_ops, NULL, views_add, NULL);
    }
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(i);
    if (st != OK)
        drop(i);   /* the client is gone, or the channel failed */
}
