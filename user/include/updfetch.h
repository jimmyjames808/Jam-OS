/* The update fetcher's window (user/lib/updfetch.c): what bin/update
 * asks the Mac for and when, as a state machine with no clock and no
 * socket of its own. The caller hands it the time and every datagram that
 * arrives, and it sends through the caller's io, so the same code runs in
 * bin/update over a UDP socket and in utest over a scripted clock and a
 * fake server that loses, repeats and reorders.
 *
 * First the manifest (a new snapshot), asked again every UPDFETCH_RETRY
 * until it comes; it is parsed (<update.h>) to learn the two files'
 * sizes, and io.begin is told. Then the kernel's and the boot image's
 * bytes, in UPDWIRE_CHUNK_MAX pieces, with up to UPDFETCH_WINDOW requests
 * in flight: each reply that matches a request in flight exactly (file,
 * offset, length, snapshot, and the file's size the manifest gave) is
 * stored (io.store) and frees its place for the next request; a request
 * unanswered for UPDFETCH_RETRY is sent again, at most UPDFETCH_TRIES
 * times in all. Every other datagram (a repeat, a stale snapshot's, one
 * nothing asked for, garbage) is counted and ignored. If the server says
 * the snapshot is gone, the fetch starts again from the manifest, at most
 * UPDFETCH_RESTARTS times.
 *
 * The bytes are only stored here, never believed: init checks them
 * against the manifest (it parses the manifest itself) before anything
 * uses them. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <update.h>
#include <updwire.h>

#define UPDFETCH_WINDOW   32u                    /* requests in flight */
#define UPDFETCH_RETRY    (300 * NS_PER_MS)      /* unanswered this long: sent again */
#define UPDFETCH_TRIES    10u                    /* sends of one request, at most */
#define UPDFETCH_RESTARTS 3u                     /* "snapshot gone": manifest again */

enum updfetch_state { UPDFETCH_MANIFEST, UPDFETCH_FILES, UPDFETCH_DONE, UPDFETCH_FAILED };

/* What the fetcher does through its caller. Each returns OK or why not. */
struct updfetch_io {
    void *ctx;
    /* Send one request datagram. A failure counts as a lost datagram. */
    status_t (*send)(void *ctx, const uint8_t *dgram, size_t len);
    /* A manifest was taken (m parsed from text, len bytes): make room for
     * its files (again, after a restart). A failure fails the fetch. */
    status_t (*begin)(void *ctx, const struct update_manifest *m, const uint8_t *text,
                      size_t len);
    /* Bytes of file (UPDATE_KERNEL, UPDATE_BOOTFS) at offset, inside the
     * size begin was given. A failure fails the fetch. */
    status_t (*store)(void *ctx, unsigned file, uint64_t offset, const uint8_t *data,
                      size_t len);
};

/* A request in flight. */
struct updfetch_slot {
    bool     used;
    uint8_t  file;        /* UPDWIRE_* */
    uint32_t offset;
    uint16_t length;
    uint64_t sent_at;     /* uptime ns of the last send */
    unsigned tries;       /* sends so far */
};

struct updfetch {
    struct updfetch_io     io;
    enum updfetch_state    state;
    status_t               why;         /* FAILED: ERR_TIMED_OUT (no answer), ERR_NOT_FOUND
                                         * (the snapshot kept going), the manifest's parse
                                         * error, ERR_OUT_OF_RANGE (a manifest longer than
                                         * a datagram), ERR_IO (the server refused a
                                         * request), or io's error */
    uint32_t               snapshot;    /* ours, once the manifest came */
    struct update_manifest m;
    unsigned               file;        /* the file asked for next (UPDATE_*) */
    uint64_t               next;        /* its next offset never asked for */
    uint64_t               stored;      /* bytes of both files stored */
    uint64_t               total;       /* bytes of both files */
    unsigned               restarts;
    struct updfetch_slot   slots[UPDFETCH_WINDOW];
    /* Counts, for the log and the tests. */
    uint64_t               sent, resent, replies, ignored;
};

/* Start: the manifest's request goes out with the first poll. */
void     updfetch_start(struct updfetch *f, const struct updfetch_io *io);
/* Send what is due at `now` (uptime ns): new requests into free places,
 * repeats of the overdue ones. Returns when to poll next (UINT64_MAX once
 * DONE or FAILED). */
uint64_t updfetch_poll(struct updfetch *f, uint64_t now);
/* A datagram arrived (len bytes, from the server's address: the caller
 * drops the rest). */
void     updfetch_reply(struct updfetch *f, const void *dgram, size_t len, uint64_t now);
