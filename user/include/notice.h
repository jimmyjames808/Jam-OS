/* A service's notices on the desktop (abi/idl/notify.idl, "/svc/notify"):
 * the posters' side, for a service whose loop must not wait (devmgr's
 * drivers, volumes' sticks, netstack's link).
 *
 * The service is started with a duplicate of /svc/notify's shared channel
 * (none without a compositor: then nothing is posted, and the service's
 * own log lines are all there is). notice_post makes its notify channel on
 * first use (svc.connect, waiting at most NOTICE_CONNECT_WAIT: the
 * compositor answers at once unless it is being started again), and again
 * after the compositor's restart closed the old one; the notice itself is
 * notify.post, an event (review W10): nothing is answered, nothing waits.
 * (The connect still waits, bounded: /svc/notify's end is one endpoint
 * every holder shares, so a connect's answer must be the kernel's to
 * route to its caller.) A notice that can't go is said once in the log
 * and forgotten: notices are news, not state. No buttons: a poster that
 * wants an answer calls notify.notify and notify_wait itself (the shell's
 * `notify`, init's notices). */
#pragma once

#include <stdint.h>
#include <os.h>

#define NOTICE_CONNECT_WAIT (200 * NS_PER_MS)

/* Tints of the notice's tile (notify.idl's TINT_*). */
enum { NOTICE_BLACKCURRANT, NOTICE_RASPBERRY, NOTICE_APRICOT };

struct notice_box {
    handle_t svc;      /* /svc/notify's shared channel (0: none: nothing is posted) */
    handle_t ch;       /* our notify channel (0: not connected) */
    bool     said;     /* a failure was said in the log */
};

/* b ready to post through svc (consumed; HANDLE_INVALID: none). */
void notice_init(struct notice_box *b, handle_t svc);
/* Post title and body (NULL: none) with the tile's letter (0: an "i")
 * and tint. */
void notice_post(struct notice_box *b, const char *title, const char *body, char icon,
                 uint8_t tint);
