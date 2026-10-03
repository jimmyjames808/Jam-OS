/* fat: every request read into a slot of the state and run from there
 * (docs/M11.6-PLAN.md, "The request in progress").
 *
 * Every request fat serves (fs on the mount's channel or a view, file,
 * fsctl) is read into a slot of the state VMO (svcstate_take), run from
 * there with the protocol's dispatch (<proto>_dispatch_on, or
 * fs_view_dispatch for fs), committed (its reply in the slot and the
 * commit word, svcstate_commit) and answered (svcstate_reply).
 *
 * The bounce buffer. The slot's request area holds the request, then, a
 * page in (FAT_SLOT_DATA), its data: a file.write's bytes are copied there
 * from the client's transfer buffer before FatFs sees them, and a
 * file.read's go from FatFs to there and on to the client. That is why a
 * slot is a little over 64 KiB (state.c): one buffer, the one copy it
 * always was, now in the state.
 *
 * There is no restart yet: a request is never run twice. */
#include <fsview.h>
#include "fat.h"

static unsigned cur_slot;   /* the slot of the request running */

uint8_t *op_bounce(void)
{
    return (uint8_t *)svcstate_request(state_slots(), cur_slot, NULL) + FAT_SLOT_DATA;
}

/* The next message on ch has more handles than a slot takes: off the
 * queue, answered ERR_INVALID_ARGS (idl_drain). */
static status_t drain(handle_t ch)
{
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, NULL, 0, &n, NULL, 0, &nh);
    if (st != ERR_BUFFER_TOO_SMALL)
        return st;   /* OK: an empty message, taken; or nothing there any more */
    return idl_drain(ch, n, nh);
}

static uint32_t dispatch(const struct fat_chan *c, const void *req, uint32_t n, void *rep,
                         handle_t *rhs, uint32_t *rhn)
{
    if (c->proto == FAT_PROTO_FILE)
        return file_dispatch_on(c->ch, &fat_file_ops, c->file, req, n, rep, rhs, rhn);
    if (c->proto == FAT_PROTO_CTL)
        return fsctl_dispatch_on(c->ch, &fat_ctl_ops, NULL, req, n, rep, rhs, rhn);
    const struct fs_view_server v = {
        .flags = c->flags, .ops = &fat_fs_ops, .add = views_add,
    };
    return fs_view_dispatch(&v, req, n, rep, rhs, rhn);
}

/* The reply, with its handles; those that can't be sent are closed. */
static void answer(const struct fat_chan *c, unsigned slot, uint32_t rn, handle_t *rhs,
                   uint32_t rhn)
{
    struct svcstate *s = state_slots();
    if (!rn) {   /* no txid: nothing to answer */
        svcstate_sent(s, slot);
        idl_close_all(rhs, rhn);
        return;
    }
    if (svcstate_reply(s, slot, c->ch, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* the client is gone */
}

/* A request that carried handles: refused, as the generated servers do. */
static void refuse(const struct fat_chan *c, unsigned slot)
{
    struct svcstate *s = state_slots();
    idl_close_all(s->handles, s->h->slot[slot].nhandles);
    const struct idl_req_hdr *q = svcstate_request(s, slot, NULL);
    struct idl_rep_hdr *r = svcstate_reply_area(s, slot);
    r->txid = q->txid;
    r->status = ERR_INVALID_ARGS;
    (void)svcstate_commit(s, slot, sizeof(*r));   /* within rep_cap */
    answer(c, slot, sizeof(*r), NULL, 0);
}

status_t serve_one(const struct fat_chan *c)
{
    struct svcstate *s = state_slots();
    unsigned slot = 0;
    status_t st = svcstate_take(s, c->id, c->ch, &slot);
    if (st == ERR_BUFFER_TOO_SMALL)
        return drain(c->ch);
    if (st == ERR_INVALID_ARGS)
        return OK;   /* under 4 bytes: no txid, nothing to answer; thrown away */
    if (st != OK)
        return st;
    if (s->h->slot[slot].nhandles) {
        refuse(c, slot);
        return OK;
    }
    uint32_t n = 0;
    const void *req = svcstate_request(s, slot, &n);
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    cur_slot = slot;
    uint32_t rn = dispatch(c, req, n, svcstate_reply_area(s, slot), rhs, &rhn);
    (void)svcstate_commit(s, slot, rn);   /* the dispatch's reply fits rep_cap */
    answer(c, slot, rn, rhs, rhn);
    return OK;
}
