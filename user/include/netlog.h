/* netlog's core (user/lib/netlog.c): the kernel log, from its first
 * byte, over UDP to the Mac's tools/netlog-recv.py (docs/M9-PLAN.md,
 * "netlog: the log over UDP to the Mac"). A state machine with no clock
 * and no socket of its own: the caller hands it the time and every
 * datagram that arrives, it reads the log through a source and sends
 * through the caller's io, so bin/netlog runs it over a UDP socket and
 * utest over a fake log, a scripted clock and a fake receiver.
 *
 * Streams: NETLOG_LIVE, this boot's log (a klog reader: positions are the
 * kernel's byte counts, so the ring's 4 MiB still holds the boot's start
 * when the network comes up); NETLOG_CRASH, after a panic, the panicked
 * boot's log (init's SR_CRASHLOG VMO), which has an end.
 *
 * Go-back-N: at most NETLOG_WINDOW bytes of a stream sent and not acked;
 * the Mac acks the offset below which it has every byte; with no progress
 * for the current wait (NETLOG_RESEND at first) every stream goes back to
 * its acked offset and sends again, and the wait doubles up to
 * NETLOG_WAIT_MAX. After NETLOG_SILENT_AFTER waits in a row without an
 * answer it says once that the Mac doesn't answer, and from then on sends
 * one datagram per try; the first ack says once that it answers again.
 * Those two lines, and "the last boot's log is sent", are all it ever
 * says (io.say): never a line per datagram or per retry, so its own lines
 * in the log it sends can't multiply.
 *
 * Bounds: a datagram carries at most NETLOG_TEXT_MAX bytes of text, cut
 * after the last '\n' that fits (a line longer than that is split), at
 * most NETLOG_BURST datagrams go out per poll and the caller polls again
 * at the time poll returns.
 *
 * If the ring dropped bytes before they were sent (or before a resend),
 * the next datagram is flagged NETLOG_F_LOST: the bytes below its offset
 * that the Mac doesn't have are gone, and the Mac marks the gap in its
 * file instead of waiting for them.
 *
 * The datagrams, all fields little-endian (<wire.h>):
 *   data, PC -> Mac, NETLOG_DATA_HDR bytes then `length` bytes of text:
 *     0 u32 magic NETLOG_MAGIC   4 u8 version NETLOG_VERSION   5 u8 type NETLOG_DATA
 *     6 u8 stream                7 u8 flags (NETLOG_F_*)
 *     8 u64 boot id: the kernel's start, UTC ns (0: the clock wasn't known)
 *    16 u64 offset of the text in the stream
 *    24 u64 acked: the offset the PC was last acked (a Mac that lost its
 *       state, a receiver started afresh, skips to it with a note: an
 *       earlier receiver has those bytes)
 *    32 u32 seq: datagrams of this stream sent, resends included (the Mac
 *       counts the ones that didn't arrive)
 *    36 u16 length, 0..NETLOG_TEXT_MAX (0 only with NETLOG_F_END)
 *    38 u16 reserved, 0
 *   ack, Mac -> PC, NETLOG_ACK_SIZE bytes:
 *     0 u32 magic   4 u8 version   5 u8 type NETLOG_ACK   6 u8 stream   7 u8 reserved, 0
 *     8 u64 boot id (the PC's)    16 u64 acked: the Mac has every byte below it */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define NETLOG_PORT         5021u
#define NETLOG_MAGIC        0x474c4e4au          /* "JNLG" */
#define NETLOG_VERSION      1u
#define NETLOG_TEXT_MAX     1400u
#define NETLOG_DATA_HDR     40u
#define NETLOG_DATA_MAX     (NETLOG_DATA_HDR + NETLOG_TEXT_MAX)
#define NETLOG_ACK_SIZE     24u
#define NETLOG_WINDOW       (64u << 10)          /* bytes of a stream sent, not acked */
#define NETLOG_RESEND       (500 * NS_PER_MS)    /* the first wait for progress */
#define NETLOG_WAIT_MAX     (30 * NS_PER_S)
#define NETLOG_SILENT_AFTER 3u                   /* waits without an answer: "no answer" */
#define NETLOG_BURST        16u                  /* datagrams per poll */
#define NETLOG_PACE         (10 * NS_PER_MS)     /* the next poll, while there is more */

enum { NETLOG_DATA = 1, NETLOG_ACK = 2 };
enum { NETLOG_LIVE, NETLOG_CRASH, NETLOG_STREAMS };
#define NETLOG_F_LOST 0x01u   /* bytes below offset the Mac lacks are gone on the PC */
#define NETLOG_F_END  0x02u   /* the stream ends with this datagram's text */
#define NETLOG_F_ALL  0x03u

/* A data datagram's header, decoded (text points into the datagram). */
struct netlog_data {
    uint8_t        stream, flags;
    uint64_t       boot_id, offset, acked;
    uint32_t       seq;
    uint16_t       length;
    const uint8_t *text;
};

/* Encode a data datagram (header + d->length bytes at d->text) into out
 * (NETLOG_DATA_MAX bytes); its size into *len. ERR_INVALID_ARGS: d breaks
 * the rules above. */
status_t netlog_data_encode(const struct netlog_data *d, uint8_t *out, size_t *len);
/* Decode one, strictly (size, magic, version, type, stream, flags,
 * reserved, length). ERR_INVALID_ARGS: not one. */
status_t netlog_data_decode(const void *dgram, size_t len, struct netlog_data *out);
/* An ack into out (NETLOG_ACK_SIZE bytes), and decoded strictly. */
void     netlog_ack_encode(uint8_t stream, uint64_t boot_id, uint64_t acked, uint8_t *out);
status_t netlog_ack_decode(const void *dgram, size_t len, uint8_t *stream, uint64_t *boot_id,
                           uint64_t *acked);

/* ---- where the text comes from ----------------------------------------------- */

/* read: up to cap bytes from position pos into buf; how many (0: nothing
 * there yet, or the end; < 0: an error, taken as nothing); *first: where
 * they start, > pos if the bytes from pos were dropped (klog_read's
 * contract). */
struct netlog_source {
    void    *ctx;
    int64_t (*read)(void *ctx, uint64_t pos, void *buf, uint64_t cap, uint64_t *first);
    uint64_t end;    /* a stream with an end: its length; UINT64_MAX: none (the live log) */
};

/* A klog reader as the source (the handle stays the caller's). */
void     netlog_klog_source(handle_t reader, struct netlog_source *out);
/* init's SR_CRASHLOG VMO as the source: checked (crashlog_header_ok),
 * the text after its header; *name gets the panicked boot's log name.
 * ERR_INVALID_ARGS: not a crash log. The VMO stays the caller's; ctx
 * holds what reading needs and must live as long as the source. */
struct netlog_vmo_ctx {
    handle_t vmo;
    uint64_t base;     /* where the text starts in the VMO */
    uint64_t len;      /* bytes of text */
};
status_t netlog_crash_source(handle_t vmo, struct netlog_vmo_ctx *ctx,
                             struct netlog_source *out, char name[32]);

/* ---- the sender ----------------------------------------------------------------- */

struct netlog_io {
    void    *ctx;
    /* Send one datagram to the Mac. A failure counts as a lost datagram. */
    status_t (*send)(void *ctx, const uint8_t *dgram, size_t len);
    /* One line for the log (a state change), without its newline. */
    void     (*say)(void *ctx, const char *line);
};

struct netlog_stream {
    bool                 on;        /* has a source */
    bool                 done;      /* an ended stream, all of it acked */
    struct netlog_source src;
    uint64_t             acked;     /* the Mac has every byte below */
    uint64_t             next;      /* the next byte to send */
    uint64_t             high;      /* one past the highest byte ever sent */
    uint32_t             seq;       /* datagrams sent */
    bool                 lost;      /* the next datagram carries NETLOG_F_LOST */
};

struct netlog {
    struct netlog_io     io;
    uint64_t             boot_id;
    struct netlog_stream s[NETLOG_STREAMS];
    uint64_t             wait;          /* the current wait for progress */
    uint64_t             retry_at;      /* go back at this time (0: nothing unacked) */
    unsigned             quiet;         /* waits in a row without an answer */
    bool                 silent;        /* said "no answer" and not yet "answers again" */
    /* Counts, for `netlog` lines at the end and the tests. */
    uint64_t             sent, resent_rounds, acks, ignored, lost_bytes;
    uint8_t              buf[NETLOG_DATA_MAX];   /* the datagram being made */
};

/* Start with the live stream's source (its first byte: 0). */
void     netlog_start(struct netlog *n, const struct netlog_io *io, uint64_t boot_id,
                      const struct netlog_source *live);
/* Add the crash stream (after a panic). */
void     netlog_add_crash(struct netlog *n, const struct netlog_source *crash);
/* Send what is due at `now` (uptime ns). Returns when to poll next:
 * UINT64_MAX when only new log text (the reader's SIG_READABLE) or an ack
 * would change anything. */
uint64_t netlog_poll(struct netlog *n, uint64_t now);
/* A datagram arrived from the Mac's address (the caller drops others). */
void     netlog_ack(struct netlog *n, const void *dgram, size_t len, uint64_t now);
