/* notify: a notice on the desktop (abi/idl/notify.idl's notify, through
 * /svc/notify: a channel that can post notices and nothing else). With
 * buttons (-b, up to three), the card stays until one is pressed; -w
 * waits here for that and says which (Ctrl+C stops waiting and takes the
 * card away). Without -w the shell's own channel posts it (svc_get's: its
 * cards stay up while the shell runs). */
#include <idl/notify.h>
#include <notice.h>
#include "sh.h"

#define CALL_WAIT (2 * NS_PER_S)
#define WAIT_TXID 0x6e770001u
#define BUTTONS   3

/* s cut to at most n bytes, at a character's start (notify.idl's texts
 * are whole UTF-8). */
static uint32_t cut(const char *s, uint32_t n)
{
    uint32_t len = (uint32_t)strlen(s);
    if (len <= n)
        return len;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;
    return n;
}

static void usage(void)
{
    sh_tty("usage: notify [-b button]... [-w] <title> [body...]\n");
}

/* Wait on ch (our own) for a press of notice id's buttons: its index, or
 * -1 (Ctrl+C, or the compositor went: said). */
static int wait_press(handle_t ch, uint32_t id)
{
    status_t st = notify_notify_wait_send(ch, WAIT_TXID);
    while (st == OK) {
        signals_t seen = 0;
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, now() + 100 * NS_PER_MS,
                                  &seen);
        if (sh_interrupted()) {
            sh_tty("^C: notify: not waiting any more\n");
            return -1;
        }
        uint8_t rep[NOTIFY_REP_MAX];
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
        st = notify_notify_wait_result(rep, &m, &got, &button);
        if (st == OK && got == id)
            return button;
        if (st == OK)
            st = notify_notify_wait_send(ch, WAIT_TXID);   /* not ours: wait on */
    }
    sh_tty("notify: %s\n", sh_why(st));
    return -1;
}

SH_CMD(notify)
{
    const char *label[BUTTONS] = { "", "", "" };
    unsigned nb = 0;
    bool wait = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-w")) {
            wait = true;
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc && nb < BUTTONS &&
                   strlen(argv[i + 1]) < 24 && argv[i + 1][0]) {
            label[nb++] = argv[++i];
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
    const char *title = argv[i++];
    char body[512] = "";
    for (size_t o = 0; i < argc && o + 1 < sizeof(body); i++)
        o += (size_t)snprintf(body + o, sizeof(body) - o, "%s%s", o ? " " : "", argv[i]);
    uint32_t tn = cut(title, 63), bn = cut(body, 95);   /* the card cuts them further */
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
    for (int tries = 0; tries < 2; tries++) {
        st = notify_notify_until(ch, now() + CALL_WAIT, title, tn, body, bn,
                                 0, 0, label[0], (uint32_t)strlen(label[0]), label[1],
                                 (uint32_t)strlen(label[1]), label[2],
                                 (uint32_t)strlen(label[2]), &id);
        if (st != ERR_PEER_CLOSED || wait || (ch = svc_get(SVC_NOTIFY)) == HANDLE_INVALID)
            break;
    }
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
    if (!notice_pressed((uint8_t)b))
        sh_say("notify: notice %u: dismissed (pushed off the screen)\n", id);
    else
        sh_say("notify: notice %u: %s\n", id, b < (int)nb ? label[b] : "?");
    return 0;
}
