/* The mixer's streams, as a program sees them: the shared ring's layout,
 * the event's bits, `vol`'s stream list (abi/idl/audioctl.idl), and the
 * client side in libos (user/lib/mixer_client.c): open a stream on the
 * mixer's service channel (SR_AUDIO), write frames into its ring, start,
 * drain, close. The protocol is abi/idl/audio.idl; the design is
 * docs/A2-PLAN.md.
 *
 * The ring is one VMO: MIXER_RING_HDR bytes of header (one page), then
 * `frames` frames of 48 kHz stereo s16 samples (4 bytes a frame). The
 * client owns `write` and `waiting`; the mixer owns `read` and `idle`.
 * Each side writes only its own fields and reads the other's: `write`
 * and `read` are frame counts since the open (never wrapped), so frame f
 * is at data offset (f % frames) * 4, and the client may write frames
 * [read, read + frames) before it moves `write` past them. The mixer
 * never maps a ring (a client with the VMO could shrink it under the
 * mapping); it reads the header and the frames with vmo_read and writes
 * `read` and `idle` with vmo_write, and keeps its own copy of `read`.
 *
 * The event wakes only someone who would otherwise wait:
 *   MIXER_SIG_SPACE  the mixer -> a client that set `waiting` (it took
 *                    frames: there is room)
 *   MIXER_SIG_DATA   a client -> the mixer, after a write while `idle`
 *                    is set (the mixer closed the output because every
 *                    playing stream was empty, and sleeps until one has
 *                    frames again)
 * A missed wake costs at most one of the mixer's periods: it looks at
 * every playing ring each period. */
#pragma once

#include <os.h>

#define MIXER_RATE       48000u
#define MIXER_CHANNELS   2u
#define MIXER_FRAME      4u            /* bytes: left, right */
#define MIXER_RING_HDR   4096u         /* the header page; the frames follow */
#define MIXER_MAX_STREAMS 16u
#define MIXER_RING_MAGIC 0x474e524du   /* "MRNG" */

#define MIXER_SIG_SPACE  (1u << 24)    /* SIG_USER_ALL bits of the event */
#define MIXER_SIG_DATA   (1u << 25)

/* The header page. Each side's fields sit on a cache line of their own. */
struct mixer_ring {
    uint32_t magic;          /* MIXER_RING_MAGIC (the mixer's, set once) */
    uint32_t frames;         /* the ring's size in frames (a power of two) */
    uint32_t reserved0[14];
    /* the client's line */
    uint64_t write;          /* frames written since the open */
    uint32_t waiting;        /* 1: blocked for space, wake me (MIXER_SIG_SPACE) */
    uint32_t reserved1[13];
    /* the mixer's line */
    uint64_t read;           /* frames taken since the open */
    uint32_t idle;           /* 1: the output is closed: signal MIXER_SIG_DATA after a write */
    uint32_t reserved2[13];
};
#define MIXER_RING_CLIENT 64u          /* offset of the client's line */
#define MIXER_RING_MIXER  128u         /* offset of the mixer's line */

/* One entry of audioctl.streams' list. */
struct mixer_stream_info {
    uint32_t id;             /* audio.open_output's id */
    int32_t  volume;         /* centibels */
    uint32_t state;          /* MIXER_STATE_* */
    uint32_t underruns;      /* periods it had too few frames for while playing */
    uint64_t played;         /* frames heard (estimated) */
    char     name[16];       /* open_output's name, NUL-padded */
};
#define MIXER_STATE_STOPPED 0u
#define MIXER_STATE_PLAYING 1u
#define MIXER_STATE_IDLE    2u   /* playing, but the output is closed until it writes */

/* ---- the client side (libos) -------------------------------------------------- */

/* An open stream: its channel, ring mapping and event. */
struct mixer_stream {
    handle_t           ch;       /* the stream channel (audio.idl's stream_* methods) */
    handle_t           vmo;      /* the ring */
    handle_t           event;
    struct mixer_ring *hdr;      /* the mapped header */
    int16_t           *data;     /* its frames */
    uint32_t           frames;   /* ring size in frames */
    uint32_t           id;       /* for `vol` */
    uint32_t           lead;     /* frames the mixer takes ahead of what is heard */
    uint64_t           write;    /* frames written (our copy of hdr->write) */
    bool               started;
};

/* Open a stream on the mixer's service channel svc (SR_AUDIO), named for
 * `vol` (up to 15 characters), and map its ring. Waits for the mixer
 * until deadline (a restarting mixer answers late). Errors: audio.idl's
 * open_output's (ERR_NOT_FOUND: no audio output; ERR_NO_RESOURCES: too
 * many streams), ERR_BAD_STATE for a ring that isn't the mixer's layout,
 * the map's. */
status_t mixer_open(handle_t svc, const char *name, uint64_t deadline, struct mixer_stream *s);
/* Copy up to n frames (interleaved s16) into the ring, as many as there
 * is room for, without waiting; *done: how many. Never a syscall, unless
 * the mixer sleeps (then one event_signal). */
void     mixer_put(struct mixer_stream *s, const int16_t *frames, size_t n, size_t *done);
/* Write all n frames, waiting for room until deadline (the stream is
 * started first if it is full and not started yet, or it would never
 * empty). *done: how many went in, also on a failure. ERR_TIMED_OUT:
 * deadline passed; ERR_PEER_CLOSED: the mixer is gone (close the stream
 * and open another). */
status_t mixer_write(struct mixer_stream *s, const int16_t *frames, size_t n, uint64_t deadline,
                     size_t *done);
/* audio.stream_start / stream_stop. */
status_t mixer_start(struct mixer_stream *s, uint64_t deadline);
status_t mixer_stop(struct mixer_stream *s, uint64_t deadline);
/* Wait until everything written so far has been heard (started first if
 * it isn't). */
status_t mixer_drain(struct mixer_stream *s, uint64_t deadline);
/* audio.stream_set_volume: *out (may be NULL) gets the volume set. */
status_t mixer_set_volume(struct mixer_stream *s, int32_t cb, uint64_t deadline, int32_t *out);
/* Unmap and close everything; the mixer drops the stream (what it took
 * already may still be heard). */
void     mixer_close(struct mixer_stream *s);
