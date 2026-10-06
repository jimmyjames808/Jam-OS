/* mixer: every program's sound through the one output stream of the HD
 * Audio driver (docs/history/A2-PLAN.md). init starts it in shell mode, after
 * devmgr, with
 *   SR_DEVMGR_DEVICE  devmgr's device channel for each HD Audio
 *                 controller (client ends, one handle each, at most
 *                 MIXER_CARDS; none without one): each answers about its
 *                 device alone, and the query channel won't hand a driver's
 *                 channel out while its device has one (<devmgr.h>), so the
 *                 mixer is the drivers' only client but for the tests. The
 *                 first one's closing (devmgr died, with every driver) ends
 *                 the mixer, and init starts it again with the new devmgr's
 *   SR_AUDIO      the server end of the `audio` channel (abi/idl/audio.idl)
 *   SR_AUDIO_CTL  the server end of the `audioctl` channel
 *   SR_AUDIO_DESK optional: the server end of the desktop's `audioctl`
 *                 channel (desk and set_master only), kept by init as the
 *                 others; its client end is the compositor's
 *   SR_STATE      its state VMO (<svcstate.h>), kept by init
 *   SR_KEEP       its end of the keep channel (<keep.h>)
 * and, when it replaces one that ended, argv[1]: "killed" (a deliberate
 * kill) or "crashed". init keeps a duplicate of both server ends, so a
 * restarted mixer serves the same channels and calls made meanwhile wait
 * for it, and with the state and the keeper a restart is not seen by any
 * client at all (adopt.c). Started without SR_STATE (a test), it makes a
 * state of its own; without SR_KEEP, nothing outlives it.
 *
 * This file is the loop: one thread and one port (and device.c's thread,
 * which answers audioctl.device, the one call that waits on others). Channels are bound
 * PERSISTENT and served a budget at a time, with a flag saying more may be
 * queued (a binding fires on edges only); each stream's event is bound
 * ONCE and watched again after each wake. The numbers are committed
 * before the loop waits (internal.h). */
#include <devmgr.h>
#include <mixmath.h>
#include "internal.h"

/* The stream a key names, if it still holds that slot's generation. */
static struct stream *keyed(struct mixer *m, uint64_t key, uint32_t base)
{
    uint32_t slot = (uint32_t)(key & 0xff) - base;
    if (slot >= MIXER_MAX_STREAMS)
        return NULL;
    struct stream *s = &m->nums->s[slot];
    return s->used && (uint32_t)(key >> 8) == s->gen ? s : NULL;
}

static void packet(struct mixer *m, const struct port_packet *p)
{
    uint32_t low = (uint32_t)(p->key & 0xff);
    struct stream *s;
    struct client_own *c;
    if (p->key == KEY_SVC) {
        m->svc_pending = true;
    } else if (p->key == KEY_CTL) {
        m->ctl_pending = true;
    } else if (p->key == KEY_DESK) {
        m->desk_pending = m->desk != HANDLE_INVALID;
    } else if (p->key == KEY_DEVMGR) {
        printf("mixer: devmgr is gone (and the hda driver with it): ending, init starts "
               "me again with the new devmgr\n");
        jam_process_exit(3);
    } else if (low == KEY_OUT) {
        if (m->own_out.ch && (uint32_t)(p->key >> 8) == m->nums->out.gen)
            m->own_out.pending = true;
    } else if (low >= KEY_CLIENT && (c = clients_keyed(m, p->key)) != NULL) {
        c->pending = true;
    } else if (low >= KEY_EVENT && (s = keyed(m, p->key, KEY_EVENT)) != NULL) {
        stream_event(m, s);
    } else if (low >= KEY_STREAM && (s = keyed(m, p->key, KEY_STREAM)) != NULL) {
        stream_own(m, s)->pending = true;
    }
}

static bool anything_pending(const struct mixer *m)
{
    if (m->svc_pending || m->ctl_pending || m->desk_pending || m->own_out.pending ||
        clients_pending(m))
        return true;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->nums->s[i].used && m->own_s[i].pending)
            return true;
    return false;
}

static void serve_all(struct mixer *m)
{
    if (m->svc_pending)
        serve_svc(m);
    if (m->ctl_pending)
        serve_ctl(m);
    if (m->desk_pending)
        serve_desk(m);
    clients_serve(m);
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->nums->s[i].used && m->own_s[i].pending)
            serve_stream(m, &m->nums->s[i]);
    if (m->own_out.pending)
        out_serve(m);
}

static status_t setup(struct mixer *m, const char *restart)
{
    bool adopted = false;
    status_t st = state_init(m, &adopted);
    if (st != OK) {
        printf("mixer: can't map its state (%s)\n", status_str(st));
        return st;
    }
    for (unsigned i = 0; i < startup_handle_count(); i++) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        if (role == SR_DEVMGR_DEVICE && m->ncards < MIXER_CARDS)
            m->cards[m->ncards++] = h;
    }
    m->svc = startup_handle(SR_AUDIO);
    m->ctl = startup_handle(SR_AUDIO_CTL);
    m->keep = startup_handle(SR_KEEP);
    m->desk = startup_handle(SR_AUDIO_DESK);
    if (!m->svc || !m->ctl) {
        printf("mixer: started without SR_AUDIO and SR_AUDIO_CTL: nothing to serve\n");
        return ERR_BAD_HANDLE;
    }
    st = jam_port_create(&m->port);
    if (st == OK)
        st = jam_port_bind(m->port, m->svc, KEY_SVC, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(m->port, m->ctl, KEY_CTL, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK && m->desk)
        st = jam_port_bind(m->port, m->desk, KEY_DESK, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK && m->ncards)   /* they all end with devmgr: one is enough to watch */
        st = jam_port_bind(m->port, m->cards[0], KEY_DEVMGR, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st != OK) {
        printf("mixer: can't set up its port (%s)\n", status_str(st));
        return st;
    }
    m->svc_pending = m->ctl_pending = true;   /* calls may be queued from before a restart */
    m->desk_pending = m->desk != HANDLE_INVALID;
    device_init(m);
    adopt(m, adopted, restart);
    return OK;
}

int main(int argc, char **argv)
{
    struct mixer *m = calloc(1, sizeof(*m));
    if (!m)
        return 1;
    if (setup(m, argc > 1 ? argv[1] : NULL) != OK)
        return 1;
    printf("mixer: serving; %u streams at most, each a ring of %u frames; the output opens "
           "while one plays\n", MIXER_MAX_STREAMS, RING_FRAMES);
    for (;;) {
        serve_all(m);
        uint64_t deadline = out_tick(m);
        if (anything_pending(m))
            continue;
        state_commit(m);
        struct port_packet p;
        status_t st = idl_wait_after(m->port, deadline, &p, &m->reply);   /* a reply goes first */
        if (st == OK)
            packet(m, &p);
        else if (st != ERR_TIMED_OUT)
            break;
    }
    printf("mixer: its port failed: ending\n");
    return 1;
}
