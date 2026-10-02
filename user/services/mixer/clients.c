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
 * gone it is closed and its slot (with a new generation) is free again. */
#include <idl/svc.h>
#include "internal.h"

status_t clients_connect(struct mixer *m, bool ctl, handle_t *out)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        struct client *c = &m->c[i];
        if (c->ch)
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
        c->ch = mine;
        c->ctl = ctl;
        c->pending = true;   /* a request may come before the first packet is read */
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

void clients_connect_reply(struct mixer *m, handle_t ch, uint32_t txid, bool ctl)
{
    handle_t h = HANDLE_INVALID;
    struct svc_connect_rep r = { .txid = txid, .status = clients_connect(m, ctl, &h) };
    if (jam_channel_write(ch, &r, sizeof(r), &h, r.status == OK ? 1 : 0) != OK && h)
        jam_handle_close(h);   /* the caller is gone: the new channel goes with it */
}

struct client *clients_keyed(struct mixer *m, uint64_t key)
{
    uint32_t slot = (uint32_t)(key & 0xff) - KEY_CLIENT;
    if (slot >= MIXER_CLIENTS)
        return NULL;
    struct client *c = &m->c[slot];
    return c->ch && (uint32_t)(key >> 8) == c->gen ? c : NULL;
}

bool clients_pending(const struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++)
        if (m->c[i].ch && m->c[i].pending)
            return true;
    return false;
}

void clients_serve(struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_CLIENTS; i++) {
        struct client *c = &m->c[i];
        if (!c->ch || !c->pending)
            continue;
        c->pending = false;
        status_t st = c->ctl ? serve_control(m, c->ch) : serve_audio(m, c->ch, i + 1);
        if (st == OK) {
            c->pending = true;   /* its budget is spent: more may be queued */
        } else if (st == ERR_PEER_CLOSED) {
            jam_handle_close(c->ch);   /* its persistent binding goes with our only handle */
            c->ch = HANDLE_INVALID;
        } else if (st != ERR_SHOULD_WAIT) {
            printf("mixer: reading an opener's channel: %s\n", status_str(st));
        }
    }
}
