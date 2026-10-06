/* A service's notices on the desktop (abi/idl/compctl.idl, "/svc/notify"):
 * the posters' side, for a service whose loop must not wait (devmgr's
 * sticks, netstack's link).
 *
 * The service is started with a duplicate of /svc/notify's shared channel
 * (none under `nocomp`: then nothing is posted, and the service's own log
 * lines are all there is, as before the desktop). notice_post makes its
 * NOTIFY channel on first use (svc.connect, waiting at most
 * NOTICE_CONNECT_WAIT: the compositor answers at once unless it is being
 * started again), and again after the compositor's restart closed the old
 * one; the notify itself is written without waiting, and its answer (the
 * notice's id) is read and dropped by the next post. A notice that can't
 * go is said once in the log and forgotten: notices are news, not state.
 * No buttons: a poster that wants an answer calls compctl's notify and
 * notify_wait itself (the shell's `notify`, init's update notice). */
#pragma once

#include <stdint.h>
#include <os.h>

#define NOTICE_CONNECT_WAIT (200 * NS_PER_MS)

/* Tints of the notice's tile (compctl.notify's). */
enum { NOTICE_BLACKCURRANT, NOTICE_RASPBERRY, NOTICE_APRICOT };

struct notice_box {
    handle_t svc;      /* /svc/notify's shared channel (0: none: nothing is posted) */
    handle_t ch;       /* our NOTIFY channel (0: not connected) */
    uint32_t txids;    /* the notifies sent */
    bool     said;     /* a failure was said in the log */
};

/* b ready to post through svc (consumed; HANDLE_INVALID: none). */
void notice_init(struct notice_box *b, handle_t svc);
/* Post title and body (NULL: none) with the tile's letter (0: an "i")
 * and tint. */
void notice_post(struct notice_box *b, const char *title, const char *body, char icon,
                 uint8_t tint);
