/* mixer: the driver's side. The hda driver played through is the first
 * sound card's (in the order of our devmgr device channels) that answers
 * hda.info with a path to a jack, and its one
 * output stream is open only while a stream plays, so the path is muted
 * whenever nothing plays (the driver unmutes only while its stream runs).
 *
 * The periods: at open, OUT_LEAD periods are mixed into the driver's ring
 * before it starts. Then a wait_period is always out, sent without
 * waiting for its answer (the answer arrives on the port with the txid we
 * chose, and calls made meanwhile on the same channel take only their
 * own replies); each answer is the end of a period, and the mixer mixes
 * until OUT_LEAD periods are written ahead of the play position again.
 * So a frame taken from a client is heard OUT_LEAD - 1 to OUT_LEAD
 * periods (128-171 ms) later, plus the limiter's MIX_LOOKAHEAD (1 ms).
 * An answer that comes with less than LATE_GUARD frames still ahead is
 * counted (`late`, per stream for `play -s`); past the play position it
 * has cost the driver's clear-behind silence, and mixing goes on from
 * the play position.
 *
 * The order (internal.h): the periods are mixed and written into the
 * driver's ring, the numbers committed, and only then is each stream's
 * new `read` published in its ring header (publish), so its client can't
 * reuse frames a commit doesn't count as taken yet. A wait_period goes to
 * the driver after a commit that holds its txid.
 *
 * The format: the largest sample size the driver's DAC takes (hda.info's
 * pcm, which `hda bits` caps), asked for at every open. At 20, 24 or 32
 * bits the mix goes out with all its 24 bits (a volume below 0 dB loses
 * nothing a DAC can play); at 16 bits it is dithered where a volume left
 * a fraction (<mixmath.h>). Either way a stream at 0 dB alone is
 * bit-exact.
 *
 * A driver that dies closes its stream channel: the output is closed and
 * opened again on the restarted driver (devmgr hands out the channel its
 * restart serves); the streams keep their frames meanwhile. */
#include <devmgr.h>
#include <idl/hda.h>
#include <mixmath.h>
#include "internal.h"

#define FIND_WAIT    (2 * NS_PER_S)          /* a driver devmgr is restarting answers late */
#define OPEN_WAIT    (3 * NS_PER_S)
#define CALL_WAIT    (2 * NS_PER_S)
#define STALL_NS     (1 * NS_PER_S)          /* no period's end for this long: the stream stalled */
#define RETRY_NS     (500 * NS_PER_MS)
#define TXID_BASE    0x6d780000u             /* our wait_period txids ("mx"), never 0 */

/* Card i's driver channel: GET_SERVICE on its device channel (all device
 * fields 0: its own device, <devmgr.h> DEVMGR_DEVICE_CHANNEL). The
 * caller's. */
static status_t driver_channel(const struct mixer *m, unsigned i, handle_t *out)
{
    struct devmgr_rep r;
    handle_t ch = HANDLE_INVALID;
    uint32_t nh = 0;
    status_t st = devmgr_call(m->cards[i], DEVMGR_GET_SERVICE, 0, 0, 0, &r, &ch, 1, &nh,
                              now() + FIND_WAIT);
    if (st == OK && nh != 1)
        st = ERR_INTERNAL;
    if (st != OK) {
        if (nh)
            jam_handle_close(ch);
        return st;
    }
    *out = ch;
    return OK;
}

/* The first card whose driver has a path to a jack, into m->own_out.svc. */
status_t out_find(struct mixer *m)
{
    if (m->own_out.svc)
        return OK;
    for (unsigned i = 0; i < m->ncards; i++) {
        handle_t ch;
        status_t st = driver_channel(m, i, &ch);
        if (st == ERR_PEER_CLOSED)
            return st;   /* devmgr is gone */
        if (st != OK)
            continue;    /* no driver running for it now */
        uint32_t codec, pin = 0, dac, pcm, formats, amp, jack, count;
        uint8_t nodes[8], text[240];
        st = hda_info_until(ch, now() + FIND_WAIT, &codec, &pin, &dac, &pcm, &formats, &amp,
                            &jack, &count, nodes, text);
        if (st == OK && pin) {
            m->own_out.svc = ch;
            printf("mixer: playing through sound card %u (codec %u, dac %02x, pin %02x)\n", i,
                   codec, dac, pin);
            return OK;
        }
        jam_handle_close(ch);
    }
    return ERR_NOT_FOUND;
}

/* Ask for the end of the period after frame `after`, without waiting:
 * its txid committed first, so the answer is always one the state waits
 * for. */
static void send_wait(struct mixer *m, uint64_t after)
{
    struct out *o = &m->nums->out;
    struct hda_wait_period_req q = {
        .txid = TXID_BASE | (++m->nums->next_txid & 0xffff), .ordinal = HDA_WAIT_PERIOD,
        .after = after,
    };
    o->waiting = true;
    o->wait_txid = q.txid;
    o->wait_sent = now();
    state_commit(m);
    status_t st = jam_channel_write(m->own_out.ch, &q, sizeof(q), NULL, 0);
    if (st != OK) {
        o->waiting = false;
        printf("mixer: can't ask the driver for the period's end (%s)\n", status_str(st));
    }
}

/* Up to `want` frames of s into m->buf: how many it had. Its `read`
 * moves on; publish puts it in the header (and wakes a client that was
 * waiting for room) after the commit. */
static uint32_t take(struct mixer *m, struct stream *s, uint32_t want)
{
    struct stream_own *w = stream_own(m, s);
    struct { uint64_t write; uint32_t waiting; } c;
    if (jam_vmo_read(w->vmo, MIXER_RING_CLIENT, &c, sizeof(c)) != OK)
        return 0;   /* it shrank its ring: nothing to take, ever (it can close it) */
    uint64_t have = c.write >= s->read ? c.write - s->read : 0;
    if (have > RING_FRAMES)
        have = RING_FRAMES;   /* it wrote over frames not taken yet: its own loss */
    s->written = c.write;
    uint32_t n = have < want ? (uint32_t)have : want, done = 0;
    while (done < n) {
        uint32_t off = (uint32_t)(s->read % RING_FRAMES);
        uint32_t k = RING_FRAMES - off < n - done ? RING_FRAMES - off : n - done;
        if (jam_vmo_read(w->vmo, MIXER_RING_HDR + (uint64_t)off * MIXER_FRAME,
                         m->buf + 2 * done, (uint64_t)k * MIXER_FRAME) != OK)
            break;
        done += k;
        s->read += k;
    }
    if (done) {
        w->took = true;
        if (c.waiting)
            w->wake = true;
    }
    return done;
}

/* Every `read` that moved since the last publish, into its stream's
 * header (after a commit: see the file's header); a client that was
 * waiting for room is woken. */
static void publish(struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream_own *w = &m->own_s[i];
        if (!w->took)
            continue;
        const struct stream *s = &m->nums->s[i];
        (void)jam_vmo_write(w->vmo, MIXER_RING_MIXER, &s->read, sizeof(s->read));
        /* `waiting` looked at again after `read` moved, as the client
         * expects (it sets `waiting`, then reads `read`): one that began
         * waiting since take() looked would otherwise sleep a whole slice. */
        uint32_t waiting = 0;
        if (!w->wake &&
            jam_vmo_read(w->vmo, offsetof(struct mixer_ring, waiting),
                         &waiting, sizeof(waiting)) == OK && waiting)
            w->wake = true;
        if (w->wake)
            (void)jam_event_signal(w->event, 0, MIXER_SIG_SPACE);
        w->took = w->wake = false;
    }
}

static void remember(struct stream *s, uint64_t at, uint64_t from, uint32_t n)
{
    if (s->nhist == HIST) {
        memmove(&s->hist[0], &s->hist[1], (HIST - 1) * sizeof(s->hist[0]));
        s->nhist--;
    }
    s->hist[s->nhist++] = (struct hist){ .at = at, .from = from, .n = n };
}

/* The next period of the driver's ring: every playing stream's frames,
 * each at its gain, summed, at the master gain. */
static void mix_period(struct mixer *m)
{
    struct out *o = &m->nums->out;
    uint32_t pf = o->period;
    memset(m->acc + 2 * MIX_LOOKAHEAD, 0, 2 * pf * sizeof(m->acc[0]));
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->nums->s[i];
        if (!s->used || !s->playing)
            continue;
        uint64_t from = s->read;
        uint32_t n = take(m, s, pf);
        if (n < pf && !s->empty && !s->draining)
            s->underruns++;   /* it had frames last period, and ran short in this one */
        s->empty = n ? 0 : s->empty + 1;
        if (!n)
            continue;
        mix_add(m->acc + 2 * MIX_LOOKAHEAD, m->buf, n, s->gain);
        remember(s, o->written + MIX_LOOKAHEAD, from, n);   /* the limiter's delay */
    }
    mix_master(m->acc + 2 * MIX_LOOKAHEAD, pf, m->nums->master_gain);
    if (mix_limit(&o->lim, m->acc, pf))
        for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
            if (m->nums->s[i].used && m->nums->s[i].playing)
                m->nums->s[i].limited++;
    uint32_t off = (uint32_t)(o->written % o->frames);
    if (o->frame_bytes == 8)
        mix_out32((int32_t *)m->own_out.ring + 2 * (size_t)off, m->acc, pf);
    else
        mix_out16((int16_t *)m->own_out.ring + 2 * (size_t)off, m->acc, pf, &o->seed);
    o->written += pf;
}

/* Every playing stream has been empty for IDLE_PERIODS: nothing will be
 * heard until one writes (MIXER_SIG_DATA) or another starts. */
static bool all_idle(const struct mixer *m)
{
    bool any = false;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        const struct stream *s = &m->nums->s[i];
        if (!s->used || !s->playing)
            continue;
        if (s->empty < IDLE_PERIODS || s->draining)
            return false;
        any = true;
    }
    return any;
}

/* A period ended at play position pos: count how late it was. */
static void count_late(struct mixer *m, uint64_t pos)
{
    struct out *o = &m->nums->out;
    uint64_t ahead = o->written > pos ? o->written - pos : 0;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        struct stream *s = &m->nums->s[i];
        if (!s->used || !s->playing)
            continue;
        if (ahead < s->min_lead)
            s->min_lead = (uint32_t)ahead;
        if (ahead < LATE_GUARD)
            s->late++;
    }
    if (ahead < LATE_GUARD) {
        o->late++;
        if (pos > o->written)
            printf("mixer: %lu frames late: the driver played silence\n",
                   (unsigned long)(pos - o->written));
        else
            printf("mixer: late: only %lu frames were ahead of the play position\n",
                   (unsigned long)ahead);
    }
}

/* A period ended: the play position is pos. */
static void period_end(struct mixer *m, uint64_t pos)
{
    struct out *o = &m->nums->out;
    uint32_t pf = o->period;
    o->played = pos;
    count_late(m, pos);
    if (pos > o->written)
        o->written = (pos + pf - 1) / pf * pf;
    uint64_t target = pos / pf * pf + (uint64_t)OUT_LEAD * pf;
    while (o->written < target)
        mix_period(m);
    state_commit(m);
    publish(m);
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->nums->s[i].used)
            drain_check(m, &m->nums->s[i], pos, OK);
    if (all_idle(m)) {
        for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
            if (m->nums->s[i].used && m->nums->s[i].playing)
                stream_set_idle(m, &m->nums->s[i], true);
        out_close(m, "every playing stream is empty");
        return;
    }
    send_wait(m, pos);
}

static status_t map_ring(struct mixer *m, handle_t vmo, uint32_t size, uint32_t period)
{
    struct out *o = &m->nums->out;
    uint32_t fb = o->frame_bytes, frames = size / fb, pf = period / fb;
    if (size % fb || period % fb || !pf || pf > PERIOD_MAX ||
        frames % pf || frames < (OUT_LEAD + 1) * pf) {
        printf("mixer: the driver's ring (%u bytes, periods of %u) doesn't fit the mixer\n",
               size, period);
        return ERR_NOT_SUPPORTED;
    }
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size,
                               VMAR_READ | VMAR_WRITE, &va);
    if (st != OK)
        return st;
    m->own_out.ring = (void *)(uintptr_t)va;
    o->frames = frames;
    o->period = pf;
    return OK;
}

/* The largest sample size the driver's DAC takes now (hda.info's pcm:
 * bits 17-20 are 16, 20, 24 and 32 bits), 16 if it can't say. */
static uint32_t best_bits(const struct out_own *w)
{
    uint32_t codec, pin = 0, dac, pcm = 0, formats, amp, jack, count;
    uint8_t nodes[8], text[240];
    if (hda_info_until(w->svc, now() + CALL_WAIT, &codec, &pin, &dac, &pcm, &formats, &amp, &jack,
                       &count, nodes, text) != OK)
        return 16;
    return pcm & (1u << 20) ? 32 : pcm & (1u << 19) ? 24 : pcm & (1u << 18) ? 20 : 16;
}

/* The driver's stream on m->own_out.svc, mapped, bound, primed and
 * started. */
static status_t open_stream(struct mixer *m)
{
    struct out *o = &m->nums->out;
    struct out_own *w = &m->own_out;
    handle_t ch = HANDLE_INVALID, vmo = HANDLE_INVALID;
    uint32_t size = 0, period = 0, bits = best_bits(w);
    status_t st = hda_open_output_until(w->svc, now() + OPEN_WAIT, MIXER_RATE, MIXER_CHANNELS,
                                        (uint8_t)bits, &ch, &vmo, &size, &period);
    if (st == ERR_NOT_SUPPORTED && bits != 16) {   /* capped meanwhile: 16 is always there */
        bits = 16;
        st = hda_open_output_until(w->svc, now() + OPEN_WAIT, MIXER_RATE, MIXER_CHANNELS, 16, &ch,
                                   &vmo, &size, &period);
    }
    if (st != OK)
        return st;
    o->bits = bits;
    o->frame_bytes = bits == 16 ? 4 : 8;
    mix_limit_init(&o->lim);
    w->ch = ch;
    w->vmo = vmo;
    o->written = o->played = 0;
    o->waiting = false;
    w->pending = false;
    st = map_ring(m, vmo, size, period);
    if (st == OK)
        st = jam_port_bind(m->port, ch, KEY_OUT | (uint64_t)++o->gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    for (unsigned i = 0; st == OK && i < OUT_LEAD; i++)
        mix_period(m);
    state_commit(m);
    publish(m);
    if (st == OK)
        st = hda_start_until(ch, now() + CALL_WAIT);
    if (st == OK)
        send_wait(m, 0);
    return st;
}

static status_t out_open(struct mixer *m)
{
    struct out *o = &m->nums->out;
    struct out_own *w = &m->own_out;
    status_t st = out_find(m);
    if (st == OK)
        st = open_stream(m);
    if (st == ERR_PEER_CLOSED && !w->ch) {   /* the driver died since we found it */
        jam_handle_close(w->svc);
        w->svc = HANDLE_INVALID;
        st = out_find(m);
        if (st == OK)
            st = open_stream(m);
    }
    if (st != OK) {
        out_close(m, NULL);
        return st;
    }
    o->opens++;
    o->retry_at = 0;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++)
        if (m->nums->s[i].used && m->nums->s[i].playing)
            stream_set_idle(m, &m->nums->s[i], false);
    printf("mixer: output open: 48 kHz %u-bit, periods of %u frames, %u written ahead "
           "(%u-%u ms)\n", o->bits, o->period, OUT_LEAD * o->period,
           (OUT_LEAD - 1) * o->period * 1000 / MIXER_RATE, OUT_LEAD * o->period * 1000 / MIXER_RATE);
    return OK;
}

void out_close(struct mixer *m, const char *why)
{
    struct out *o = &m->nums->out;
    struct out_own *w = &m->own_out;
    if (w->ring)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)w->ring,
                       (uint64_t)o->frames * o->frame_bytes);
    if (w->ch)
        jam_handle_close(w->ch);   /* the driver stops the stream and mutes the path */
    if (w->vmo)
        jam_handle_close(w->vmo);
    bool was = w->ch != HANDLE_INVALID && w->ring;
    w->ring = NULL;
    w->ch = w->vmo = HANDLE_INVALID;
    o->waiting = false;
    w->pending = false;
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        m->nums->s[i].nhist = 0;   /* heard, or never will be */
        if (m->nums->s[i].used)
            drain_check(m, &m->nums->s[i], 0, OK);
    }
    if (was && why)
        printf("mixer: output closed (%s); %lu frames mixed, %lu limited, %lu late period(s), "
               "peak %d%%\n", why, (unsigned long)o->written, (unsigned long)o->lim.limited,
               (unsigned long)o->late, (int)((int64_t)o->lim.peak * 100 / MIX_FULL));
    o->late = 0;
}

static bool any_audible(const struct mixer *m)
{
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        const struct stream *s = &m->nums->s[i];
        if (s->used && s->playing && !s->idle)
            return true;
    }
    return false;
}

status_t out_need(struct mixer *m)
{
    struct out *o = &m->nums->out;
    if (m->own_out.ch || !any_audible(m))
        return OK;
    status_t st = out_open(m);
    if (st != OK) {
        if (!o->retry_at)
            printf("mixer: can't open the output (%s): trying again every %u ms\n",
                   status_str(st), (unsigned)(RETRY_NS / NS_PER_MS));
        o->retry_at = now() + RETRY_NS;
    }
    return st;
}

/* The driver died: its service channel is dead too; a restart is due. */
static void driver_gone(struct mixer *m, const char *why)
{
    out_close(m, why);
    if (m->own_out.svc)
        jam_handle_close(m->own_out.svc);
    m->own_out.svc = HANDLE_INVALID;
    m->nums->out.retry_at = now() + RETRY_NS / 2;
}

void out_serve(struct mixer *m)
{
    struct out *o = &m->nums->out;
    struct out_own *w = &m->own_out;
    w->pending = false;
    for (int i = 0; i < 16 && w->ch; i++) {
        struct hda_wait_period_rep r;
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = w->ch, .bytes_cap = sizeof(r), .bytes = (uint64_t)(uintptr_t)&r,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st == ERR_PEER_CLOSED) {
            driver_gone(m, "the driver went away");
            return;
        }
        if (st == ERR_BUFFER_TOO_SMALL) {
            (void)idl_drain(w->ch, n, nh);   /* nothing of ours is that big: dropped */
            continue;
        }
        if (st != OK || n < sizeof(struct idl_rep_hdr) || !o->waiting || r.txid != o->wait_txid)
            continue;   /* not the answer we wait for (a stale one, or too big: dropped) */
        o->waiting = false;
        if (r.status != OK || n != sizeof(r)) {
            out_close(m, r.status == ERR_PEER_CLOSED ? "the driver went away"
                                                     : "the driver's stream stopped");
            o->retry_at = now() + RETRY_NS;
            return;
        }
        period_end(m, r.frames);
    }
    w->pending = w->ch != HANDLE_INVALID;
}

uint64_t out_tick(struct mixer *m)
{
    struct out *o = &m->nums->out;
    handle_t *ch = &m->own_out.ch;
    uint64_t t = now();
    if (*ch && !any_playing(m))
        out_close(m, "no stream plays");
    if (*ch && o->waiting && t - o->wait_sent > STALL_NS) {
        out_close(m, "no period ended for a second: the driver's stream stalled");
        o->retry_at = t + RETRY_NS;
    }
    if (!*ch && o->retry_at && t >= o->retry_at)
        (void)out_need(m);
    if (!*ch && !any_audible(m))
        o->retry_at = 0;
    if (*ch && o->waiting)
        return o->wait_sent + STALL_NS + 1;
    return o->retry_at ? o->retry_at : DEADLINE_NEVER;
}

uint64_t out_position(struct mixer *m)
{
    uint64_t frames = 0;
    uint32_t off = 0;
    if (m->own_out.ch && hda_position_until(m->own_out.ch, now() + CALL_WAIT, &frames, &off) == OK)
        return frames;
    return m->nums->out.played;
}

int32_t out_device_gain(struct mixer *m)
{
    int32_t gain = 0, min, max;
    uint32_t step;
    if (!m->own_out.svc ||
        hda_get_gain_until(m->own_out.svc, now() + CALL_WAIT, &gain, &step, &min, &max) != OK)
        return 0;
    return gain;
}

/* audioctl.device: the index-th card's driver as a query channel. The
 * driver it plays through is not touched: a query channel can't open the
 * output (hda.idl). */
status_t out_query(struct mixer *m, uint32_t index, handle_t *out)
{
    handle_t ch, q;
    if (index >= m->ncards || driver_channel(m, index, &ch) != OK)
        return ERR_NOT_FOUND;   /* no such card, no devmgr, or its driver not running now */
    status_t st = hda_query_until(ch, now() + FIND_WAIT, &q);
    jam_handle_close(ch);
    if (st == OK)
        *out = q;
    return st;
}
