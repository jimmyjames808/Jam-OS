/* fat: views (fs.view, <fsview.h>): narrower `fs` channels onto the
 * volume, for programs that get less than the whole of it (read-only, or
 * with the top-level `etc` left alone). Each is served like the fs
 * channel devmgr gave us, with its flags checked first (fs_view_dispatch),
 * FAT_BATCH requests per wakeup. A view goes when its client closes it,
 * and every view goes when fat ends: a view never keeps the volume served
 * (main.c ends when the fs channel's clients are gone).
 *
 * Each slot's flags and generation are in the state (kept->views); its
 * channel, a handle, is here, by the same index. A view's requests are
 * served as the fs channel's are (request.c: serve_one, fs_view_dispatch),
 * its flags taken from the state, never from a request. Dropping a view is
 * one store (its `used`), so it needs no operation of its own: a successor
 * that finds a view whose client has gone drops it again.
 *
 * Each view's channel is put with the keeper (keeper.c) before the fs.view
 * that made it commits, and dropped there once the state has forgotten
 * it, so a successor is handed it back (views_take, views_adopt). */
#include "fat.h"

static handle_t view_ch[FAT_VIEWS];   /* our end of slot i's channel */

/* Wait (ONCE) for slot i's next request or its client's close. */
static status_t arm(unsigned i)
{
    return jam_port_bind(vol.port, view_ch[i], FAT_KEY_VIEW(i, kept->views[i].gen),
                         SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

/* Slot i's channel goes (its binding with it), and the keeper's. */
static void close_channel(unsigned i)
{
    if (view_ch[i] != HANDLE_INVALID)
        jam_handle_close(view_ch[i]);
    view_ch[i] = HANDLE_INVALID;
    kept_drop(FAT_KEEP_VIEW(i));
}

static void drop(unsigned i)
{
    kept->views[i].used = false;
    close_channel(i);
}

/* Slot i's new channel ch (consumed): kept, then waited on. */
static status_t attach(unsigned i, handle_t ch)
{
    view_ch[i] = ch;
    status_t st = kept_put(FAT_KEEP_VIEW(i), &ch, 1);
    return st == OK ? arm(i) : st;
}

status_t views_add(void *host, handle_t ch, uint32_t flags)
{
    (void)host;
    for (unsigned i = 0; i < FAT_VIEWS; i++) {
        struct fat_view *v = &kept->views[i];
        if (v->used)
            continue;
        v->used = true;
        v->flags = flags;
        v->gen++;
        status_t st = attach(i, ch);
        if (st != OK)
            drop(i);
        else
            op_made(FAT_MADE_VIEW, i);
        return st;
    }
    jam_handle_close(ch);
    return ERR_NO_RESOURCES;
}

void views_drop_unknown(void)
{
    for (unsigned i = 0; i < FAT_VIEWS; i++)
        if (!kept->views[i].used && view_ch[i] != HANDLE_INVALID)
            close_channel(i);
}

void views_event(uint64_t key)
{
    unsigned i = FAT_KEY_SLOT(key);
    if (i >= FAT_VIEWS || !kept->views[i].used || kept->views[i].gen != FAT_KEY_GEN(key))
        return;   /* a closed slot's packet, or a reused one's old one */
    const struct fat_chan c = {
        .ch = view_ch[i], .id = FAT_CHAN_VIEW(i, kept->views[i].gen), .proto = FAT_PROTO_FS,
        .flags = kept->views[i].flags,
    };
    status_t st = OK;
    for (unsigned k = 0; k < FAT_BATCH && st == OK && !vol.disk_gone; k++) {
        files_reap();
        st = serve_one(&c);
    }
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(i);
    if (st != OK)
        drop(i);   /* the client is gone, or the channel failed */
}

/* ---- a successor's views (adopt.c) ------------------------------------------------- */

bool views_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= FAT_VIEWS || !kept->views[i].used || n != 1 || view_ch[i])
        return false;
    view_ch[i] = hs[0];
    return true;
}

unsigned views_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < FAT_VIEWS; i++) {
        if (!kept->views[i].used)
            continue;
        if (!view_ch[i] || arm(i) != OK)
            drop(i);   /* not handed back: its client's end is closed */
        else
            n++;
    }
    return n;
}

bool views_reply_arrived(uint32_t i)
{
    if (i >= FAT_VIEWS || !kept->views[i].used || !view_ch[i])
        return true;   /* dropped since: it had arrived, or it is gone anyway */
    signals_t seen = 0;
    return jam_object_wait_one(view_ch[i], SIG_PEER_CLOSED, 0, &seen) == ERR_TIMED_OUT;
}

status_t views_remake(uint32_t i, handle_t *out)
{
    handle_t mine = HANDLE_INVALID, client = HANDLE_INVALID;
    if (view_ch[i])
        jam_handle_close(view_ch[i]);   /* not bound yet: views_adopt comes after */
    view_ch[i] = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &client);
    if (st == OK) {
        view_ch[i] = mine;
        st = kept_put(FAT_KEEP_VIEW(i), &mine, 1);
    }
    if (st != OK) {
        if (client)
            jam_handle_close(client);
        drop(i);
        return st;
    }
    *out = client;
    return OK;
}

bool views_chan(uint32_t i, uint32_t gen16, struct fat_chan *out)
{
    if (i >= FAT_VIEWS || !kept->views[i].used || !view_ch[i] ||
        (kept->views[i].gen & 0xffffu) != gen16)
        return false;
    *out = (struct fat_chan){
        .ch = view_ch[i], .id = FAT_CHAN_VIEW(i, kept->views[i].gen), .proto = FAT_PROTO_FS,
        .flags = kept->views[i].flags,
    };
    return true;
}
