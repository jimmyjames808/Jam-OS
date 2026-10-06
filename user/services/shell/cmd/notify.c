/* notify: a notice on the desktop (abi/idl/compctl.idl's notify, through
 * /svc/notify: a NOTIFY channel, which can post notices and nothing
 * else). With buttons (-b, up to three), the card stays until one is
 * pressed; -w waits here for that and says which (Ctrl+C stops waiting
 * and takes the card away). Without -w the shell's own channel posts it
 * (svc_get's: its cards stay up while the shell runs). Under `nocomp`
 * there is no desktop and no /svc/notify. */
#include <idl/compctl.h>
#include "sh.h"

#define CALL_WAIT (2 * NS_PER_S)
#define WAIT_TXID 0x6e770001u
#define BUTTONS   3

static void usage(void)
{
    sh_tty("usage: notify [-b button]... [-w] <title> [body...]\n");
}

/* Wait on ch (our own) for a press of notice id's buttons: its index, or
 * -1 (Ctrl+C, or the compositor went: said). */
static int wait_press(handle_t ch, uint32_t id)
{
    status_t st = compctl_notify_wait_send(ch, WAIT_TXID);
    while (st == OK) {
        signals_t seen = 0;
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, now() + 100 * NS_PER_MS,
                                  &seen);
        if (sh_interrupted()) {
            sh_tty("^C: notify: not waiting any more\n");
            return -1;
        }
        uint8_t rep[COMPCTL_REP_MAX];
        struct idl_msg m;
        st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT) {
            st = OK;
            continue;
        }
        if (st != OK)
            break;
        if (m.txid != WAIT_TXID) {
            idl_msg_drop(&m);
            continue;
        }
        uint32_t got = 0;
        uint8_t button = 0;
        st = compctl_notify_wait_result(rep, &m, &got, &button);
        if (st == OK && got == id)
            return button;
        if (st == OK)
            st = compctl_notify_wait_send(ch, WAIT_TXID);   /* not ours: wait on */
    }
    sh_tty("notify: %s\n", sh_why(st));
    return -1;
}

SH_CMD(notify)
{
    uint8_t buttons[24 * BUTTONS] = { 0 };
    const char *label[BUTTONS];
    unsigned nb = 0;
    bool wait = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-w")) {
            wait = true;
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc && nb < BUTTONS &&
                   strlen(argv[i + 1]) < 24 && argv[i + 1][0]) {
            label[nb] = argv[++i];
            memcpy(buttons + 24 * nb, label[nb], strlen(label[nb]));
            nb++;
        } else {
            usage();
            return 2;
        }
    }
    if (i >= argc || (wait && !nb)) {
        usage();
        if (wait && !nb)
            sh_tty("notify: -w waits for a button: give one with -b\n");
        return 2;
    }
    uint8_t title[64] = { 0 }, body[96] = { 0 };
    snprintf((char *)title, sizeof(title), "%s", argv[i++]);
    for (size_t o = 0; i < argc && o + 1 < sizeof(body); i++)
        o += (size_t)snprintf((char *)body + o, sizeof(body) - o, "%s%s", o ? " " : "", argv[i]);
    handle_t ch = HANDLE_INVALID;
    status_t st = wait ? svc_open(SVC_NOTIFY, &ch) : OK;
    if (!wait)
        ch = svc_get(SVC_NOTIFY);
    if (st != OK || !ch) {
        sh_tty("notify: no desktop to post to (%s)\n",
               st != OK ? sh_why(st) : "no /svc/notify: a boot without the compositor");
        return 1;
    }
    uint32_t id = 0;
    st = compctl_notify_until(ch, now() + CALL_WAIT, title, body, 0, 0, buttons, &id);
    if (st == ERR_PEER_CLOSED && !wait && (ch = svc_get(SVC_NOTIFY)) != HANDLE_INVALID)
        st = compctl_notify_until(ch, now() + CALL_WAIT, title, body, 0, 0, buttons, &id);
    if (st != OK) {
        sh_tty("notify: %s\n", st == ERR_INVALID_ARGS ? "a control character, or no title"
                               : sh_why(st));
        if (wait)
            jam_handle_close(ch);
        return 1;
    }
    sh_say("notify: notice %u posted\n", id);
    if (!wait)
        return 0;
    int b = wait_press(ch, id);
    jam_handle_close(ch);   /* its card goes with it, if still up */
    if (b < 0)
        return 1;
    sh_say("notify: notice %u: %s\n", id, b < (int)nb ? label[b] : "?");
    return 0;
}
