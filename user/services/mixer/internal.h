/* The mixer: what its files share. main.c is the loop (one thread, one
 * port: every channel and event it serves, devmgr's device channels and the
 * driver's stream channel); streams.c the streams (the `audio` service
 * channel, each stream's channel and event, the `audioctl` control
 * channel); clients.c the openers' own `audio` and `audioctl` channels
 * (svc.connect); output.c the driver's side (finding the hda driver, its one
 * output stream, the periods, the mixing); device.c a second thread for
 * audioctl.device, which calls devmgr and the drivers and so must not
 * hold up the loop; state.c the state VMO; adopt.c a restart.
 * docs/history/A2-PLAN.md has the design; <mixer.h> the ring.
 *
 * Time is the driver's: at the end of each period it played (2048 frames,
 * 42.7 ms) the mixer mixes until OUT_LEAD periods are written ahead of
 * the play position again, so between OUT_LEAD - 1 and OUT_LEAD periods
 * are always ahead (128-171 ms): the mixer may be scheduled up to 128 ms
 * late before anything is lost. The loop's state is its own: device.c's
 * thread reads only m->cards and m->ctl, which never change.
 *
 * **What the mixer knows lives in its state VMO** (<svcstate.h>; state.c):
 * every number (each stream's, each opener's, the output's, the master
 * volume) is in struct mixer_state, apart from the handles and the
 * mapping, which are the process's own (struct stream_own, client_own,
 * out_own: the same index as the numbers they go with). The loop changes
 * the working copy (m->nums) freely; a commit (state_commit) copies it to
 * whichever of the two saved copies isn't current and then moves the
 * commit word, one store, so a saved copy is always whole. The order:
 * a period is mixed and written into the driver's ring, then committed,
 * then each stream's new `read` is published in its ring header; every
 * answer (a request's, a drain's, a wait_period sent to the driver) goes
 * out after a commit; and the loop commits before it waits. Requests are
 * read into the state's two request slots (svcstate_prepare and the
 * protocol's generated <proto>_take_slot), run with <proto>_run_slot and
 * answered from the slot's reply area.
 *
 * **A restart is not seen by clients** (adopt.c; docs/M11.6-PLAN.md): init
 * keeps the state VMO (SR_STATE) and, through the keep channel (SR_KEEP,
 * <keep.h>), a duplicate of every handle a client holds the other end of
 * (each stream's channel, ring and event, each opener's channel) and of
 * the driver's channel, stream channel and ring; a successor finds them
 * all, carries on from the last commit and finishes the request in
 * progress. Each such handle is put to the keeper (keep_slot_put) before
 * the state records it (the driver's stream: right after its ring's shape
 * is committed, output.c), and dropped (keep_slot_drop) after the state
 * has forgotten it. */
#pragma once

#include <idl/audio.h>
#include <idl/audioctl.h>
#include <mixer.h>
#include <mixmath.h>
#include <os.h>
#include <svcstate.h>

#define OUT_LEAD       4u      /* periods written ahead of the play position */
#define IDLE_PERIODS   24u     /* every playing stream empty this long (1 s): close the output */
#define PERIOD_MAX     MIX_BLOCK_MAX   /* frames: the most a driver's period may hold */
#define PERIOD_GUESS   2048u   /* frames: a period before the output was first opened */
#define LATE_GUARD     256u    /* frames (5.3 ms): a period end with less than this
                                * written ahead counts as late. The controller fetches
                                * up to a FIFO (SD_FIFOS bytes, the driver's open line)
                                * ahead of its position, so frames written closer than
                                * that may already have been fetched as silence */
#define RING_FRAMES    65536u  /* each stream's ring: 1.37 s, a program's read-ahead
                                * (`play` reads its file in chunks between writes, so
                                * a slow read is covered by what its ring holds) */
#define HIST           4u      /* periods remembered per stream for `played` */
#define MIXER_CARDS    4u      /* sound cards (devmgr device channels) taken at the start */

/* Port keys: these, or a stream's slot with its generation. */
#define KEY_SVC     1u
#define KEY_CTL     2u
#define KEY_DEVMGR  3u
#define KEY_OUT     4u          /* | out.gen << 8 */
#define KEY_DESK    5u          /* the desktop's channel (SR_AUDIO_DESK) */
#define KEY_STREAM  0x10u       /* + slot, | gen << 8 */
#define KEY_EVENT   0x30u       /* + slot, | gen << 8 */
#define KEY_CLIENT  0x50u       /* + an opener's slot, | gen << 8 */

#define MIXER_CLIENTS 24u       /* openers' channels at once (clients.c) */

#define STATE_KIND     0x6d697872u   /* "mixr": the state's svcstate kind */
#define STATE_LAYOUT   3u            /* struct mixer_saved's layout version */
#define DEVICE_QUEUE   8u            /* audioctl.device requests handed to device.c's thread */

/* The keeper's slots (<keep.h>): which handles each one holds, in order. */
#define KEEP_STREAM    0u            /* + a stream's slot: its channel, ring and event */
#define KEEP_CLIENT    32u           /* + an opener's slot: its channel */
#define KEEP_OUT       64u           /* the driver's stream channel and ring */
#define KEEP_DRIVER    65u           /* the driver's channel (from devmgr) */

/* What the last request that made something made (struct mixer_state's
 * made_kind). */
#define MADE_STREAM    1u
#define MADE_CLIENT    2u
#define REQ_CAP_AUDIO  (AUDIO_REQ_MAX + 8)   /* bytes read from an `audio` channel at most */
#define REQ_CAP_CTL    AUDIOCTL_REQ_MAX      /* ... and from an `audioctl` channel */

/* One period a stream gave frames to: its frames [from, from + n) went to
 * the driver's frames [at, at + n). */
struct hist {
    uint64_t at, from;
    uint32_t n;
};

/* A stream's numbers (in the state). */
struct stream {
    bool        used;
    uint32_t    id;                /* for `vol`, never reused while the mixer runs */
    uint32_t    owner;             /* the opener it counts to: 0 the shared channel, else
                                    * 1 + its clients.c slot */
    uint32_t    owner_gen;         /* that slot's generation then (0 for the shared one) */
    uint32_t    gen;               /* the slot's generation (its port keys) */
    char        name[16];
    char        title[64];         /* what plays on it (stream_set_title), "" none */
    bool        playing;           /* started */
    int32_t     volume;            /* centibels */
    uint32_t    gain;              /* Q15, from volume */
    uint64_t    read;              /* frames taken: ours, published to the header */
    uint64_t    written;           /* the header's `write` when last looked at */
    uint32_t    underruns;
    uint32_t    late;              /* periods the mixer was late for while it played */
    uint32_t    min_lead;          /* the least written ahead of the play position (frames) */
    uint32_t    limited;           /* periods the limiter turned down while it played */
    uint32_t    empty;             /* periods in a row it gave nothing */
    bool        idle;              /* its header's `idle` is set */
    struct hist hist[HIST];        /* the last periods it gave frames to, oldest first */
    unsigned    nhist;
    bool        draining;          /* a stream_drain waits */
    uint32_t    drain_txid;
    uint64_t    drain_to;          /* frames that must be heard */
};

/* A stream's handles and the loop's flags about it: the process's own. */
struct stream_own {
    handle_t    ch, vmo, event;    /* our ends (0: a free slot) */
    bool        pending;           /* the channel may have messages */
    bool        took;              /* frames taken since its `read` was last published */
    bool        wake;              /* its client waited for room when they were taken */
};

/* The driver's output stream's numbers (in the state; output.c). */
struct out {
    uint32_t bits;         /* the samples' size: 16, or 20/24/32 in 32-bit containers */
    uint32_t frame_bytes;  /* 4 or 8 */
    uint32_t frames;       /* ring size in frames */
    uint32_t period;       /* frames per period */
    uint32_t gen;          /* port key generation */
    bool     waiting;      /* a wait_period is out */
    uint32_t wait_txid;
    uint64_t wait_sent;    /* when (ns) */
    uint64_t written;      /* frames mixed into the ring since the open */
    uint64_t played;       /* the play position last heard of */
    uint64_t retry_at;     /* a failed open: when to try again (0: no retry due) */
    uint64_t opens, late;  /* times opened; periods the mixer was late for */
    struct mix_limiter lim;
    uint32_t seed;         /* the 16-bit output's dither */
};

/* The output's handles and mapping: the process's own. */
struct out_own {
    handle_t svc;          /* the hda driver's channel from devmgr, or 0 */
    handle_t ch;           /* its stream channel while open, or 0 */
    handle_t vmo;
    void    *ring;         /* mapped */
    bool     pending;      /* its channel may have messages */
    bool     kept;         /* ch and vmo are put to the keeper (KEEP_OUT) */
    bool     svc_kept;     /* svc is (KEEP_DRIVER) */
};

/* An opener's own `audio` or `audioctl` channel (clients.c): its numbers
 * (in the state). */
struct client {
    bool     used;      /* a channel is open in this slot */
    bool     ctl;       /* an `audioctl` channel (else `audio`) */
    bool     desk;      /* ... with the desktop's authority only (desk_channel) */
    uint32_t gen;       /* the slot's generation (its port key) */
};

/* An opener's channel: the process's own. */
struct client_own {
    handle_t ch;        /* our end, bound PERSISTENT (0: a free slot) */
    bool     pending;   /* its channel may have messages */
};

/* Everything the mixer knows: the numbers a successor would carry on
 * from. No handles and no pointers. */
struct mixer_state {
    uint64_t      req_done;        /* the svcstate number of the last request answered */
    uint64_t      made_seq;        /* the request that last made a stream or an opener's channel */
    uint32_t      made_kind;       /* MADE_STREAM or MADE_CLIENT */
    uint32_t      made_index;      /* its slot */
    struct client c[MIXER_CLIENTS];
    struct stream s[MIXER_MAX_STREAMS];
    struct out    out;
    int32_t       master;          /* centibels */
    uint32_t      master_gain;     /* Q15 */
    uint32_t      next_id;
    uint32_t      next_txid;
};

/* An audioctl.device request handed to device.c's thread, not answered
 * yet: a successor hands it to its own thread again. Written in place
 * (not committed): the loop fills it and sets `busy` last; the thread
 * clears `busy` once it has answered (both released). */
struct device_slot {
    uint32_t busy;      /* 1: handed over, not answered yet */
    uint32_t txid;      /* the caller's */
    uint32_t index;     /* the request's card index */
    uint32_t key;       /* the channel it came on (a port key: KEY_CTL or an opener's) */
    uint64_t seq;       /* the request slot's number it was read into */
};

/* The service's own area of the state VMO (svcstate_user). */
struct mixer_saved {
    uint64_t           commits;    /* the commit word: copy[commits & 1] is the committed state */
    struct mixer_state copy[2];    /* the last two commits */
    struct mixer_state work;       /* the loop's working copy (m->nums) */
    struct device_slot dev[DEVICE_QUEUE];
};

struct mixer {
    struct mixer_state *nums;      /* the working numbers: &saved->work */
    struct mixer_saved *saved;     /* in the state VMO */
    struct svcstate     state;     /* the state VMO, mapped */
    handle_t      port, svc, ctl;
    handle_t      desk;            /* the desktop's channel (SR_AUDIO_DESK), or 0 */
    bool          desk_pending;
    char          out_name[48];    /* the output's name (hda.output_name), "" not known yet */
    handle_t      cards[MIXER_CARDS];   /* devmgr's device channels, one per sound card */
    unsigned      ncards;
    bool          svc_pending, ctl_pending;
    struct client_own own_c[MIXER_CLIENTS];      /* by the index of nums->c */
    struct stream_own own_s[MIXER_MAX_STREAMS];  /* by the index of nums->s */
    struct out_own    own_out;
    handle_t      keep;            /* our end of the keep channel (SR_KEEP), or 0: nothing kept */
    struct idl_reply reply;        /* the last request's reply, waiting for the loop's next
                                    * take or port wait (req_answer) */
    unsigned      cur_slot;        /* the request slot being run (do_open_output's) */
    int32_t       acc[2 * (MIX_LOOKAHEAD + PERIOD_MAX)];
    int16_t       buf[2 * PERIOD_MAX];
};

/* The handles that go with stream s. */
static inline struct stream_own *stream_own(struct mixer *m, const struct stream *s)
{
    return &m->own_s[s - m->nums->s];
}

/* ---- state.c ------------------------------------------------------------------ */

/* Map the state VMO init gave (SR_STATE; without one, a new VMO of our
 * own) and set up the numbers (m->nums, m->saved, m->state). *adopted:
 * a dead instance's state passed every check, and the working numbers are
 * its last commit; else they are empty. Errors as svcstate_create's and
 * svcstate_open's. */
status_t state_init(struct mixer *m, bool *adopted);
/* Commit the working numbers (above). */
void     state_commit(struct mixer *m);
/* The saved numbers make sense (every count and index in range), or why
 * not. */
const char *state_check(const struct mixer_state *n);
/* Put hs[0..n) to the keeper as slot `slot` (<keep.h>): OK, or why not
 * (the caller refuses what it would have kept). OK and nothing sent
 * without a keep channel. */
status_t keep_slot_put(struct mixer *m, uint32_t slot, const handle_t *hs, unsigned n);
/* The keeper forgets slot (after the state has). */
void     keep_slot_drop(struct mixer *m, uint32_t slot);
/* Read the next request from ch (whose port key is `key`: an `audioctl`
 * channel if ctl, else an `audio` one) into a request slot, with the
 * protocol's generated take (<proto>_take_slot). OK with *slot set: a
 * request to run (svcstate_request); OK with *slot REQ_NONE: a message
 * was taken and answered or dropped as the generated server does
 * (handles, too long, under 4 bytes); else the read's status
 * (ERR_SHOULD_WAIT: empty; ERR_PEER_CLOSED). */
#define REQ_NONE (~0u)
status_t req_take(struct mixer *m, handle_t ch, uint32_t key, bool ctl, unsigned *slot);
/* Answer the request in slot on ch: the numbers committed (the request
 * counted), the slot committed, then the reply in the slot's reply area
 * (rn bytes with hs[0..nh), moved; nothing if rn is 0: answered later or
 * never), made to wait in m->reply for the loop's next take (req_take) or
 * port wait, which sends it and marks it out in the slot. A reply that
 * can't be written has its handles closed. */
void     req_answer(struct mixer *m, unsigned slot, handle_t ch, uint32_t rn, handle_t *hs,
                    uint32_t nh);
/* A channel's budget is spent: the reply waiting goes out now, on that
 * channel (idl_reply_flush). OK: more may be queued there; ERR_SHOULD_WAIT:
 * nothing is. */
status_t req_budget_spent(struct mixer *m);
/* Answer the request in slot on ch with a bare status. */
void     req_status(struct mixer *m, unsigned slot, handle_t ch, status_t st);

/* ---- streams.c ---------------------------------------------------------------- */

/* Up to a budget of messages from the service, control or a stream's
 * channel; sets the pending flag again if more may be queued. */
void serve_svc(struct mixer *m);
void serve_ctl(struct mixer *m);
/* The same for the desktop's channel. */
void serve_desk(struct mixer *m);
/* Up to a budget of messages from an `audio` (or `audioctl`) channel ch,
 * the shared one or an opener's (`owner`: struct stream's), OK if the
 * budget was spent (more may be queued), else the read's status
 * (ERR_SHOULD_WAIT: empty; ERR_PEER_CLOSED: its clients are gone). */
status_t serve_audio(struct mixer *m, handle_t ch, uint32_t owner);
/* The same for an `audioctl` channel ch whose port key is `key`. */
status_t serve_control(struct mixer *m, handle_t ch, uint32_t key);
void serve_stream(struct mixer *m, struct stream *s);
/* A stream's event fired (MIXER_SIG_DATA): take the bit down, watch again. */
void stream_event(struct mixer *m, struct stream *s);
/* Drop the stream: its handles closed, its slot free. */
void stream_drop(struct mixer *m, struct stream *s, const char *why);
/* Some stream is playing (started). */
bool any_playing(const struct mixer *m);
/* Frames of s heard, by the driver's position `pos`. */
uint64_t stream_played(const struct stream *s, uint64_t pos);
/* Answer s's waiting drain if its frames have been heard by `pos`
 * (or with st, unless st is OK). */
void drain_check(struct mixer *m, struct stream *s, uint64_t pos, status_t st);
/* Set or clear the `idle` flag in s's header. */
void stream_set_idle(struct mixer *m, struct stream *s, bool idle);
/* Run the request in slot, read from ch: an `audio` request of the opener
 * `owner` (struct stream's) or, with s, one of stream s's; an `audioctl`
 * one whose channel's port key is `key`. Answered (or handed on) as when
 * it was first read. */
void run_audio(struct mixer *m, unsigned slot, handle_t ch, uint32_t owner, struct stream *s);
void run_control(struct mixer *m, unsigned slot, handle_t ch, uint32_t key);

/* ---- output.c ----------------------------------------------------------------- */

/* The hda driver of the first sound card whose driver has a path to a
 * jack (kept in m->out.svc). ERR_NOT_FOUND: there is none;
 * ERR_PEER_CLOSED: devmgr is gone. */
status_t out_find(struct mixer *m);

/* Open the driver's output if a stream plays and it is closed (and no
 * retry is due later); OK if it is open now. */
status_t out_need(struct mixer *m);
/* Close it (the driver stops and mutes): every stream's period history
 * goes, as it played or never will. */
void     out_close(struct mixer *m, const char *why);
/* Messages on the driver's stream channel: wait_period's answers. */
void     out_serve(struct mixer *m);
/* What the loop should do about the output now: close it if nothing
 * plays, open it if a retry is due. The deadline for the next look. */
uint64_t out_tick(struct mixer *m);
/* The driver's play position now (a call to it), or the last one known. */
uint64_t out_position(struct mixer *m);
/* Mix until OUT_LEAD periods are written ahead of the play position pos
 * (frames already played are skipped: a gap), commit, publish. */
void     out_fill(struct mixer *m, uint64_t pos);
/* Ask the driver for the end of the period after frame `after` (the
 * state committed first). */
void     out_send_wait(struct mixer *m, uint64_t after);
/* Map the driver's ring vmo (size bytes, periods of `period` bytes) as
 * o->frames and o->period. ERR_NOT_SUPPORTED: a ring the mixer can't use. */
status_t out_map(struct mixer *m, handle_t vmo, uint32_t size, uint32_t period);
/* Every stream's `read` that moved, into its header (after a commit). */
void     out_publish(struct mixer *m);
/* Forget the driver's channel (it died, or is to be found again): closed,
 * the keeper told. */
void     out_forget_driver(struct mixer *m);
/* The output's name (hda.output_name), asked of the driver the first time
 * (found if need be: out_find); "" if there is none. The process's own,
 * not the state's: a successor asks again. */
const char *out_name(struct mixer *m);
/* The driver's gain in centibels (hda.get_gain), or 0 if it can't say (no
 * driver found yet: this never goes looking for one). */
int32_t  out_device_gain(struct mixer *m);
/* audioctl.device: the index-th sound card's driver as a query channel
 * (hda.query: everything but open_output) into *out, the caller's.
 * ERR_NOT_FOUND: no such driver. Calls devmgr and the driver: device.c's
 * thread calls it, never the loop. Uses only m->cards. */
status_t out_query(struct mixer *m, uint32_t index, handle_t *out);

/* ---- device.c ----------------------------------------------------------------- */

/* ---- clients.c ---------------------------------------------------------------- */

/* svc.connect: a new opener's channel (`audioctl` if ctl), watched on the
 * port; *out: the client end. ERR_NO_RESOURCES: MIXER_CLIENTS already. */
status_t clients_connect(struct mixer *m, bool ctl, bool desk, handle_t *out);
/* The same, answered on ch to the request in the request slot `slot`. */
void clients_connect_reply(struct mixer *m, handle_t ch, unsigned slot, bool ctl, bool desk);
/* The opener a port key names, if it still holds that slot's generation. */
struct client_own *clients_keyed(struct mixer *m, uint64_t key);
/* Opener i's channel closed and its slot free (the keeper told). */
void clients_drop(struct mixer *m, unsigned i);
/* Some opener's channel may have messages. */
bool clients_pending(const struct mixer *m);
/* A budget of messages from each opener's channel that may have some; a
 * channel whose client end is gone is closed. */
void clients_serve(struct mixer *m);

/* ---- device.c (continued) ------------------------------------------------------ */

struct audioctl_device_req;
/* Start the thread that answers audioctl.device (without one, device_ask
 * answers in the loop, as slow as that is). */
void device_init(struct mixer *m);
/* An audioctl.device request read into request slot `slot` from the
 * control channel ch (the shared one or an opener's) whose port key is
 * `key`: OK, it is answered on ch by the thread (or already was, without
 * one); else the status to answer it with now (ERR_NO_RESOURCES:
 * DEVICE_QUEUE requests wait already). */
status_t device_ask(struct mixer *m, handle_t ch, uint32_t key, unsigned slot,
                    const struct audioctl_device_req *q);
/* The request read as number seq is with the thread (a successor's
 * question about the request in progress). */
bool     device_holds(const struct mixer *m, uint64_t seq);
/* A successor: hand every request its state says the dead instance's
 * thread hadn't answered to ours (a channel that is gone: dropped). */
void     device_resume(struct mixer *m);

/* ---- adopt.c ------------------------------------------------------------------ */

/* The channel a port key names now (the shared ones, an opener's or a
 * stream's, by its slot and generation), or 0; *s: the stream, if a
 * stream's; *owner: the opener's struct stream owner, if an `audio` one;
 * *ctl: an `audioctl` channel. */
handle_t key_channel(struct mixer *m, uint32_t key, struct stream **s, uint32_t *owner,
                     bool *ctl);
/* Set up from the state: adopt a dead instance's (restart: argv[1],
 * "killed" or "crashed", or NULL), or start fresh; either way take what
 * the keeper kept (closing what the state doesn't know). The port and the
 * startup channels are set up already. */
void     adopt(struct mixer *m, bool adopted, const char *restart);
