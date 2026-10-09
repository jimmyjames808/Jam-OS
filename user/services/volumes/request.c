/* volumes: the answers to its protocol's requests (<volumes.h>): which
 * filesystem service a request names (find), and what each method does
 * with it. chans.c reads the requests off volumes' channels and decides
 * what each channel may ask (devmgr's request.c answered these before the
 * storage split, with the same wire). */
#include "disk.h"

static struct vol *find(const struct volumes_req *q)
{
    if (q->vendor == VOLUMES_FS_SVC)
        return fs_find(q->instance, q->device);
    if (q->vendor == VOLUMES_FS_MOUNT)
        return fs_find_mount(q->device, q->instance);
    return NULL;
}

/* GET_DRIVER: read-only views of b's process and job. */
static void get_driver(const struct vol *b, bool known, struct volumes_rep *r, handle_t *hs,
                       uint32_t *nh)
{
    if (!known) {
        r->status = ERR_NOT_FOUND;
        return;
    }
    if (!b->proc) {
        r->status = ERR_BAD_STATE;
        return;
    }
    r->a = 0;
    const handle_t src[2] = { b->proc, b->job };
    uint32_t got = 0;
    while (got < 2 && (r->status = jam_handle_duplicate(src[got], RIGHTS_BASIC, &hs[got])) == OK)
        got++;
    if (r->status != OK)   /* all or nothing: close what was duplicated */
        while (got > 0)
            jam_handle_close(hs[--got]);
    *nh = got;
}

/* SUPERVISION: b's state, restarts, backoff, promotions, and the spare. */
static void supervision(const struct vol *b, struct volumes_rep *r)
{
    if (!b) {
        r->status = ERR_NOT_FOUND;
        return;
    }
    r->a = b->state;
    r->b = b->win.restarts;
    r->c = b->win.backoff_ms;
    r->d = b->promoted;
    r->e = spare_waits();
}

void request_handle(const struct volumes_req *q, enum level lv, struct volumes_rep *r,
                    handle_t *hs, uint32_t *nh)
{
    (void)lv;   /* chans.c lets the ESP channel ask ESP_WRITE alone */
    *nh = 0;
    r->status = OK;
    if (q->ordinal == VOLUMES_SHUTDOWN) {
        stopping = true;   /* run() sees it once this reply is sent */
        return;
    }
    if (q->ordinal == VOLUMES_REMOUNT) {
        bool valid = q->vendor == VOLUMES_USB_MOUNT && !(q->instance & ~3u);
        r->status = valid ? disk_remount(q->device, q->instance & VOLUMES_REMOUNT_TEST,
                                         q->instance & VOLUMES_REMOUNT_WRITE)
                          : ERR_INVALID_ARGS;
        return;
    }
    if (q->ordinal == VOLUMES_ESP_WRITE) {   /* only on init's ESP channel (chans.c) */
        bool writable = q->instance & VOLUMES_ESP_WRITABLE;
        if (q->vendor || q->device || (q->instance & ~VOLUMES_ESP_WRITABLE))
            r->status = ERR_INVALID_ARGS;
        else if ((r->status = disk_esp_write(writable, &hs[0])) == OK && writable)
            *nh = 1;
        return;
    }
    struct vol *b = find(q);
    bool known = b && b->path && b->state != VOLUMES_SUP_NONE;   /* an instance was started */
    switch (q->ordinal) {
    case VOLUMES_GET_SERVICE:
        /* A filesystem's channel is MOUNTS's to hand out, never this. */
        r->status = ERR_ACCESS_DENIED;
        return;
    case VOLUMES_GET_DRIVER:
        get_driver(b, known, r, hs, nh);
        return;
    case VOLUMES_KILL:
        r->status = known ? sup_kill(b) : ERR_NOT_FOUND;
        return;
    case VOLUMES_SUPERVISION:
        supervision(b, r);
        return;
    default:
        r->status = ERR_NOT_SUPPORTED;
        return;
    }
}
