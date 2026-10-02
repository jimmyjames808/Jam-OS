/* devmgr: its channels. init gives it the server ends of a control channel
 * (SR_DEVMGR_CTL), a query channel (SR_DEVMGR) and an audio channel
 * (SR_DEVMGR_AUDIO); each is watched on devmgr's port, read here and
 * answered by request.c. What a channel may ask is decided by which channel
 * it is (its level), never by who sent the request (<devmgr.h> "Trust"). */
#include "internal.h"

/* What the query channel (SR_DEVMGR) and the audio channel
 * (SR_DEVMGR_AUDIO) may ask; the control channel (SR_DEVMGR_CTL) may ask
 * everything. The two differ in GET_SERVICE alone (get_service). */
static bool query_ok(uint32_t ordinal)
{
    return ordinal == DEVMGR_STATUS || ordinal == DEVMGR_GET_SERVICE ||
           ordinal == DEVMGR_GET_DRIVER || ordinal == DEVMGR_SUPERVISION;
}

/* Answer everything queued on ch, a channel of level lv. Returns
 * ERR_SHOULD_WAIT once the queue is empty, ERR_PEER_CLOSED once every
 * client is gone and nothing is left to read. */
static status_t serve(handle_t ch, enum level lv)
{
    bool control = lv == LEVEL_CONTROL;
    for (;;) {
        _Alignas(8) uint8_t buf[64];
        handle_t in[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = ch, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)in,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(ch, n, nh);   /* nothing of ours is that big */
            continue;
        }
        if (st != OK)
            return st;
        /* Only SET_CONSOLE and TEST_DISK carry a handle (one), and only on
         * control. */
        const struct devmgr_req *q = (const struct devmgr_req *)buf;
        bool denied = n >= 8 && !control && !query_ok(q->ordinal);
        bool whole = !denied && n == sizeof(*q);
        bool takes_handle = whole && nh == 1 && (q->ordinal == DEVMGR_SET_CONSOLE ||
                                                 q->ordinal == DEVMGR_TEST_DISK);
        if (!takes_handle)
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(in[i]);
        if (n < 4)
            continue;   /* no txid: nobody to answer */
        if (whole && !nh && q->ordinal == DEVMGR_MOUNTS) {
            mounts_request(ch, q->txid, q->instance);   /* answered now or later */
            continue;
        }
        struct devmgr_rep r = { .txid = q->txid, .status = ERR_INVALID_ARGS };
        handle_t hs[DEVMGR_MAX_HANDLES];
        rights_t rs[DEVMGR_MAX_HANDLES];
        uint32_t nout = 0;
        if (denied) {
            r.status = ERR_ACCESS_DENIED;
        } else if (takes_handle && q->ordinal == DEVMGR_SET_CONSOLE) {
            usb_new_console(in[0]);
            r.status = OK;
        } else if (takes_handle) {
            uint32_t id = 0;
            r.status = disk_test(in[0], &id);
            r.a = id;
        } else if (whole && !nh) {
            request_handle(q, lv, &r, hs, rs, &nout);
        }
        uint32_t rn = r.status == OK ? sizeof(r) : DEVMGR_REP_HDR;
        if (jam_channel_write_rights(ch, &r, rn, hs, rs, nout) != OK)
            for (uint32_t i = 0; i < nout; i++)
                jam_handle_close(hs[i]);   /* the client is gone */
        sup_run_due();   /* a long burst of requests mustn't hold up a restart */
    }
}

/* devmgr's channels: chans[CH_CONTROL] (SR_DEVMGR_CTL), chans[CH_QUERY]
 * (SR_DEVMGR) and chans[CH_AUDIO] (SR_DEVMGR_AUDIO), each 0 if init gave
 * none; their port keys and levels. devmgr lives as long as chans[life]'s
 * clients do (the control channel, or without one the query channel). */
enum { CH_CONTROL, CH_QUERY, CH_AUDIO, NCHANS };
static const uint64_t chan_keys[NCHANS] = { KEY_CONTROL, KEY_CHANNEL, KEY_AUDIO };
static const enum level chan_levels[NCHANS] = { LEVEL_CONTROL, LEVEL_QUERY, LEVEL_AUDIO };
static handle_t chans[NCHANS];
static bool     armed[NCHANS];   /* bound to the port (ONCE), not fired yet */
static unsigned life;

bool chans_init(void)
{
    chans[CH_CONTROL] = startup_handle(SR_DEVMGR_CTL);
    chans[CH_QUERY] = startup_handle(SR_DEVMGR);
    chans[CH_AUDIO] = startup_handle(SR_DEVMGR_AUDIO);
    life = chans[CH_CONTROL] ? CH_CONTROL : CH_QUERY;
    return chans[life] != HANDLE_INVALID;
}

status_t chans_serve(void)
{
    status_t st = ERR_SHOULD_WAIT;
    for (unsigned c = 0; c < NCHANS && st == ERR_SHOULD_WAIT; c++) {
        if (!chans[c])
            continue;
        st = serve(chans[c], chan_levels[c]);
        if (st == ERR_PEER_CLOSED && c != life) {
            if (armed[c])
                jam_port_unbind(port, chans[c], chan_keys[c]);
            jam_handle_close(chans[c]);   /* nobody queries any more */
            chans[c] = HANDLE_INVALID;
            st = ERR_SHOULD_WAIT;
        }
    }
    return st;
}

/* ONCE, re-armed after it fires (it fires at once if a message came in
 * meanwhile); a driver's death arrives on the same port, and a due
 * restart ends the wait. */
status_t chans_arm(void)
{
    status_t st = ERR_SHOULD_WAIT;
    for (unsigned c = 0; c < NCHANS && st == ERR_SHOULD_WAIT; c++) {
        if (!chans[c] || armed[c])
            continue;
        st = jam_port_bind(port, chans[c], chan_keys[c], SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_ONCE);
        if (st != OK)
            break;
        armed[c] = true;
        st = ERR_SHOULD_WAIT;
    }
    return st;
}

bool chans_packet(uint64_t key)
{
    bool ours = false;
    for (unsigned c = 0; c < NCHANS; c++)
        if (chan_keys[c] == key) {
            armed[c] = false;
            ours = true;
        }
    return ours;
}
