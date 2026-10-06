/* A service's notices on the desktop (<notice.h>). */
#include <idl/compctl.h>
#include <idl/svc.h>
#include <notice.h>

#define TXID_BASE 0x6e700000u   /* | a count: our notifies */

void notice_init(struct notice_box *b, handle_t svc)
{
    *b = (struct notice_box){ .svc = svc };
}

/* Answers to earlier notifies (their ids): read and dropped. False: the
 * channel's peer is gone (the compositor restarted): closed. */
static bool drain(struct notice_box *b)
{
    for (;;) {
        uint8_t rep[COMPCTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(b->ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return true;
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);
            continue;
        }
        jam_handle_close(b->ch);
        b->ch = HANDLE_INVALID;
        return false;
    }
}

static void say_once(struct notice_box *b, const char *title, status_t st)
{
    if (!b->said)
        printf("notice: \"%s\" not shown on the desktop (%s)\n", title, status_str(st));
    b->said = true;
}

void notice_post(struct notice_box *b, const char *title, const char *body, char icon,
                 uint8_t tint)
{
    if (!b->svc)
        return;   /* no compositor: the log has it */
    if (b->ch)
        (void)drain(b);
    status_t st = OK;
    if (!b->ch)
        st = svc_connect_within(b->svc, NOTICE_CONNECT_WAIT, &b->ch);
    if (st != OK) {
        b->ch = HANDLE_INVALID;
        say_once(b, title, st);
        return;
    }
    uint8_t t[64] = { 0 }, d[96] = { 0 }, buttons[72] = { 0 };
    snprintf((char *)t, sizeof(t), "%s", title);
    snprintf((char *)d, sizeof(d), "%s", body ? body : "");
    b->txids = (b->txids + 1) & 0xffff;
    st = compctl_notify_send(b->ch, TXID_BASE | b->txids, t, d, (uint8_t)icon, tint, buttons);
    if (st == ERR_PEER_CLOSED) {   /* the compositor restarted: once more on a new channel */
        jam_handle_close(b->ch);
        b->ch = HANDLE_INVALID;
        st = svc_connect_within(b->svc, NOTICE_CONNECT_WAIT, &b->ch);
        if (st == OK)
            st = compctl_notify_send(b->ch, TXID_BASE | b->txids, t, d, (uint8_t)icon, tint,
                                     buttons);
        if (st != OK && b->ch) {
            jam_handle_close(b->ch);
            b->ch = HANDLE_INVALID;
        }
    }
    if (st != OK)
        say_once(b, title, st);
    else
        b->said = false;
}
