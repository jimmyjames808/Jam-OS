/* mixer: a channel per opener. The shared `audio` and `audioctl` channels
 * (SR_AUDIO, SR_AUDIO_CTL, published as /svc/audio and /svc/audioctl)
 * also answer the svc protocol's connect (abi/idl/svc.idl) with a new
 * channel of the caller's own that speaks the same protocol, so each
 * opener (libos's svc_open, svc_get) has one: a reply that comes after its
 * caller stopped waiting (a stream's handles, a query channel) waits on
 * that caller's channel, which goes with it, never on a channel others
 * read. The streams opened on an opener's channel count to it, at most
 * MIXER_STREAMS_PER_CLIENT (streams.c).
 *
 * At most MIXER_CLIENTS openers at once: a connect past that is
 * ERR_NO_RESOURCES. An opener's channel is bound PERSISTENT on the port and
 * served a budget at a time, like the shared ones; once its client end is
 * gone it is closed and its slot (with a new generation) is free again.
 * A slot's numbers (used, ctl, gen) are in the state, its channel and
 * flag the process's own, at the same index. */
#include <idl/svc.h>
#include "internal.h"

status_t clients_connect(struct mixer *m, bool ctl, handle_t *out)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        struct client *c = &m->nums->c[i];
        struct client_own *w = &m->own_c[i];
        if (c->used)
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        c->gen++;
        st = jam_port_bind(m->port, mine, (KEY_CLIENT + i) | (uint64_t)c->gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
        if (st != OK) {
            jam_handle_close(mine);
            jam_handle_close(theirs);
            return st;
        }
        c->used = true;
        c->ctl = ctl;
        w->ch = mine;
        w->pending = true;   /* a request may come before the first packet is read */
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

void clients_connect_reply(struct mixer *m, handle_t ch, unsigned slot, bool ctl)
{
    const struct idl_req_hdr *q = svcstate_request(&m->state, slot, NULL);
    struct svc_connect_rep *r = svcstate_reply_area(&m->state, slot);
    handle_t h = HANDLE_INVALID;
    status_t st = clients_connect(m, ctl, &h);
    *r = (struct svc_connect_rep){ .txid = q->txid, .status = st };
    /* The caller gone: the new channel goes with it (req_answer closes it). */
    req_answer(m, slot, ch, sizeof(*r), &h, st == OK ? 1 : 0);
}

struct client_own *clients_keyed(struct mixer *m, uint64_t key)
{
    uint32_t slot = (uint32_t)(key & 0xff) - KEY_CLIENT;
    if (slot >= MIXER_CLIENTS)
        return NULL;
    const struct client *c = &m->nums->c[slot];
    return c->used && (uint32_t)(key >> 8) == c->gen ? &m->own_c[slot] : NULL;
}

bool clients_pending(const struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++)
        if (m->own_c[i].ch && m->own_c[i].pending)
            return true;
    return false;
}

void clients_serve(struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        struct client *c = &m->nums->c[i];
        struct client_own *w = &m->own_c[i];
        if (!w->ch || !w->pending)
            continue;
        w->pending = false;
        status_t st = c->ctl ? serve_control(m, w->ch, (KEY_CLIENT + i) | c->gen << 8)
                             : serve_audio(m, w->ch, i + 1);
        if (st == OK) {
            w->pending = true;   /* its budget is spent: more may be queued */
        } else if (st == ERR_PEER_CLOSED) {
            jam_handle_close(w->ch);   /* its persistent binding goes with our only handle */
            w->ch = HANDLE_INVALID;
            c->used = false;
        } else if (st != ERR_SHOULD_WAIT) {
            printf("mixer: reading an opener's channel: %s\n", status_str(st));
        }
    }
}
