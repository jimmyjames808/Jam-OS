/* fat: views (fs.view, <fsview.h>): narrower `fs` channels onto the
 * volume, for programs that get less than the whole of it (read-only, or
 * with the top-level `etc` left alone). Each is served like the fs
 * channel devmgr gave us, with its flags checked first (fs_view_serve_one),
 * FAT_BATCH requests per wakeup. A view goes when its client closes it,
 * and every view goes when fat ends: a view never keeps the volume served
 * (main.c ends when the fs channel's clients are gone).
 *
 * Each slot's flags and generation are in the state (kept->views); its
 * channel, a handle, is here, by the same index. */
#include <fsview.h>
#include "fat.h"

static handle_t view_ch[FAT_VIEWS];   /* our end of slot i's channel */

/* Wait (ONCE) for slot i's next request or its client's close. */
static status_t arm(unsigned i)
{
    return jam_port_bind(vol.port, view_ch[i], FAT_KEY_VIEW(i, kept->views[i].gen),
                         SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

static void drop(unsigned i)
{
    jam_handle_close(view_ch[i]);
    view_ch[i] = HANDLE_INVALID;
    kept->views[i].used = false;
}

status_t views_add(void *host, handle_t ch, uint32_t flags)
{
    (void)host;
    for (unsigned i = 0; i < FAT_VIEWS; i++) {
        struct fat_view *v = &kept->views[i];
        if (v->used)
            continue;
        view_ch[i] = ch;
        v->used = true;
        v->flags = flags;
        v->gen++;
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
    if (i >= FAT_VIEWS || !kept->views[i].used || kept->views[i].gen != FAT_KEY_GEN(key))
        return;   /* a closed slot's packet, or a reused one's old one */
    status_t st = OK;
    for (unsigned k = 0; k < FAT_BATCH && st == OK && !vol.disk_gone; k++) {
        files_reap();
        st = fs_view_serve_one(view_ch[i], kept->views[i].flags, &fat_fs_ops, NULL, views_add,
                               NULL);
    }
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(i);
    if (st != OK)
        drop(i);   /* the client is gone, or the channel failed */
}
