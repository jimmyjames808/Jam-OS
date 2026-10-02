/* mixer: every program's sound through the one output stream of the HD
 * Audio driver (docs/A2-PLAN.md). init starts it in shell mode, after
 * devmgr, with
 *   SR_DEVMGR_AUDIO  devmgr's audio channel (a client end): the one that
 *                 hands out the hda driver's channel (GET_SERVICE; the
 *                 query channel refuses it, <devmgr.h>), so the mixer is
 *                 the driver's only client but for the tests; its closing
 *                 (devmgr died, with every driver) ends the mixer, and init
 *                 starts it again with the new devmgr's
 *   SR_AUDIO      the server end of the `audio` channel (abi/idl/audio.idl)
 *   SR_AUDIO_CTL  the server end of the `audioctl` channel
 * init keeps a duplicate of both server ends, so a restarted mixer serves
 * the same channels and calls made meanwhile wait for it.
 *
 * This file is the loop: one thread and one port (and device.c's thread,
 * which answers audioctl.device, the one call that waits on others). Channels are bound
 * PERSISTENT and served a budget at a time, with a flag saying more may be
 * queued (a binding fires on edges only); each stream's event is bound
 * ONCE and watched again after each wake. */
#include <devmgr.h>
#include <mixmath.h>
#include "internal.h"

/* The stream a key names, if it still holds that slot's generation. */
static struct stream *keyed(struct mixer *m, uint64_t key, uint32_t base)
{
    uint32_t slot = (uint32_t)(key & 0xff) - base;
    if (slot >= MIXER_MAX_STREAMS)
        return NULL;
    struct stream *s = &m->s[slot];
    return s->used && (uint32_t)(key >> 8) == s->gen ? s : NULL;
}

static void packet(struct mixer *m, const struct port_packet *p)
{
    uint32_t low = (uint32_t)(p->key & 0xff);
    struct stream *s;
    if (p->key == KEY_SVC) {
        m->svc_pending = true;
    } else if (p->key == KEY_CTL) {
        m->ctl_pending = true;
    } else if (p->key == KEY_DEVMGR) {
        printf("mixer: devmgr is gone (and the hda driver with it): ending, init starts "
               "me again with the new devmgr\n");
        jam_process_exit(3);
    } else if (low == KEY_OUT) {
        if (m->out.ch && (uint32_t)(p->key >> 8) == m->out.gen)
            m->out.pending = true;
    } else if (low >= KEY_EVENT && (s = keyed(m, p->key, KEY_EVENT)) != NULL) {
        stream_event(m, s);
    } else if (low >= KEY_STREAM && (s = keyed(m, p->key, KEY_STREAM)) != NULL) {
        s->pending = true;
    }
}

static bool anything_pending(const struct mixer *m)
{
    if (m->svc_pending || m->ctl_pending || m->out.pending)
        return true;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->s[i].used && m->s[i].pending)
            return true;
    return false;
}

static void serve_all(struct mixer *m)
{
    if (m->svc_pending)
        serve_svc(m);
    if (m->ctl_pending)
        serve_ctl(m);
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->s[i].used && m->s[i].pending)
            serve_stream(m, &m->s[i]);
    if (m->out.pending)
        out_serve(m);
}

static status_t setup(struct mixer *m)
{
    m->devmgr = startup_handle(SR_DEVMGR_AUDIO);
    m->svc = startup_handle(SR_AUDIO);
    m->ctl = startup_handle(SR_AUDIO_CTL);
    if (!m->svc || !m->ctl) {
        printf("mixer: started without SR_AUDIO and SR_AUDIO_CTL: nothing to serve\n");
        return ERR_BAD_HANDLE;
    }
    status_t st = jam_port_create(&m->port);
    if (st == OK)
        st = jam_port_bind(m->port, m->svc, KEY_SVC, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(m->port, m->ctl, KEY_CTL, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK && m->devmgr)
        st = jam_port_bind(m->port, m->devmgr, KEY_DEVMGR, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st != OK) {
        printf("mixer: can't set up its port (%s)\n", status_str(st));
        return st;
    }
    m->master_gain = MIX_UNITY;
    m->next_id = 1;
    m->svc_pending = m->ctl_pending = true;   /* calls may be queued from before a restart */
    device_init(m);
    return OK;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct mixer *m = calloc(1, sizeof(*m));
    if (!m)
        return 1;
    if (setup(m) != OK)
        return 1;
    printf("mixer: serving; %u streams at most, each a ring of %u frames; the output opens "
           "while one plays\n", MIXER_MAX_STREAMS, RING_FRAMES);
    for (;;) {
        serve_all(m);
        uint64_t deadline = out_tick(m);
        if (anything_pending(m))
            continue;
        struct port_packet p;
        status_t st = jam_port_wait(m->port, deadline, &p);
        if (st == OK)
            packet(m, &p);
        else if (st != ERR_TIMED_OUT)
            break;
    }
    printf("mixer: its port failed: ending\n");
    return 1;
}
