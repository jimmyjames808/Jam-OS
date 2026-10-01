/* The mixer: what its files share. main.c is the loop (one thread, one
 * port: every channel and event it serves, devmgr's channel and the
 * driver's stream channel); streams.c the streams (the `audio` service
 * channel, each stream's channel and event, the `audioctl` control
 * channel); output.c the driver's side (finding the hda driver, its one
 * output stream, the periods, the mixing). docs/A2-PLAN.md has the
 * design; <mixer.h> the ring.
 *
 * Time is the driver's: at the end of each period it played (2048 frames,
 * 42.7 ms) the mixer mixes until OUT_LEAD periods are written ahead of
 * the play position again, so between OUT_LEAD - 1 and OUT_LEAD periods
 * are always ahead (128-171 ms): the mixer may be scheduled up to 128 ms
 * late before anything is lost. Nothing here is shared between threads:
 * there is one. */
#pragma once

#include <mixer.h>
#include <mixmath.h>
#include <os.h>

#define OUT_LEAD       4u      /* periods written ahead of the play position */
#define IDLE_PERIODS   12u     /* every playing stream empty this long: close the output */
#define PERIOD_MAX     MIX_BLOCK_MAX   /* frames: the most a driver's period may hold */
#define PERIOD_GUESS   2048u   /* frames: a period before the output was first opened */
#define RING_FRAMES    16384u  /* each stream's ring: 341 ms */
#define HIST           4u      /* periods remembered per stream for `played` */

/* Port keys: these, or a stream's slot with its generation. */
#define KEY_SVC     1u
#define KEY_CTL     2u
#define KEY_DEVMGR  3u
#define KEY_OUT     4u          /* | out.gen << 8 */
#define KEY_STREAM  0x10u       /* + slot, | gen << 8 */
#define KEY_EVENT   0x30u       /* + slot, | gen << 8 */

/* One period a stream gave frames to: its frames [from, from + n) went to
 * the driver's frames [at, at + n). */
struct hist {
    uint64_t at, from;
    uint32_t n;
};

struct stream {
    bool        used;
    uint32_t    id;                /* for `vol`, never reused while the mixer runs */
    uint32_t    gen;               /* the slot's generation (its port keys) */
    char        name[16];
    handle_t    ch, vmo, event;    /* our ends */
    bool        pending;           /* the channel may have messages */
    bool        playing;           /* started */
    int32_t     volume;            /* centibels */
    uint32_t    gain;              /* Q15, from volume */
    uint64_t    read;              /* frames taken: ours, published to the header */
    uint64_t    written;           /* the header's `write` when last looked at */
    uint32_t    underruns;
    uint32_t    limited;           /* periods the limiter turned down while it played */
    uint32_t    empty;             /* periods in a row it gave nothing */
    bool        idle;              /* its header's `idle` is set */
    struct hist hist[HIST];        /* the last periods it gave frames to, oldest first */
    unsigned    nhist;
    bool        draining;          /* a stream_drain waits */
    uint32_t    drain_txid;
    uint64_t    drain_to;          /* frames that must be heard */
};

/* The driver's output stream (output.c). */
struct out {
    handle_t svc;          /* the hda driver's channel from devmgr, or 0 */
    handle_t ch;           /* its stream channel while open, or 0 */
    handle_t vmo;
    void    *ring;         /* mapped */
    uint32_t bits;         /* the samples' size: 16, or 20/24/32 in 32-bit containers */
    uint32_t frame_bytes;  /* 4 or 8 */
    uint32_t frames;       /* ring size in frames */
    uint32_t period;       /* frames per period */
    uint32_t gen;          /* port key generation */
    bool     pending;      /* its channel may have messages */
    bool     waiting;      /* a wait_period is out */
    uint32_t wait_txid;
    uint64_t wait_sent;    /* when (ns) */
    uint64_t written;      /* frames mixed into the ring since the open */
    uint64_t played;       /* the play position last heard of */
    uint64_t retry_at;     /* a failed open: when to try again (0: no retry due) */
    uint64_t opens, late;  /* times opened; frames the mixer was late for */
    struct mix_limiter lim;
    uint32_t seed;         /* the 16-bit output's dither */
};

struct mixer {
    handle_t      port, devmgr, svc, ctl;
    bool          svc_pending, ctl_pending;
    struct stream s[MIXER_MAX_STREAMS];
    struct out    out;
    int32_t       master;          /* centibels */
    uint32_t      master_gain;     /* Q15 */
    uint32_t      next_id;
    uint32_t      next_txid;
    int32_t       acc[2 * (MIX_LOOKAHEAD + PERIOD_MAX)];
    int16_t       buf[2 * PERIOD_MAX];
};

/* ---- streams.c ---------------------------------------------------------------- */

/* Up to a budget of messages from the service, control or a stream's
 * channel; sets the pending flag again if more may be queued. */
void serve_svc(struct mixer *m);
void serve_ctl(struct mixer *m);
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
void stream_set_idle(struct stream *s, bool idle);

/* ---- output.c ----------------------------------------------------------------- */

/* The hda driver with a path to a jack, found through devmgr (kept in
 * m->out.svc). ERR_NOT_FOUND: there is none. */
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
/* The driver's gain in centibels (hda.get_gain), or 0 if it can't say. */
int32_t  out_device_gain(struct mixer *m);
